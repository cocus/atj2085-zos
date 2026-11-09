"""High-level (HLE) model of the ATJ2085 DSP running MP3DEC.DSP.

The DSP core itself is NOT emulated. This model plays the DSP's side of the host-interface
protocol that the v04 MUSIC.AP codec driver (bank 0x03, IRQ handler 0x253A, dispatcher
0x2561) expects, and decodes the MP3 bitstream on the PC (pymp3).

Host interface (ATJ2085X PG + codec code):
  port 0x2E bit7       DSP run (1) / reset (0)         -> start / stop of this model
  ports 0x30..0x37     mailbox; reads = DSP->MCU latches, writes = MCU->DSP latches
  port 0x35            request code (DSP->MCU) / reply code (MCU->DSP)
  port 0x26/0x27 bit0  DSP->MCU interrupt status (W1C) / enable
  DSP RAM              IDM low byte lane (port 05 = 0x24 -> 0x4000+word) for bulk values
  DMA1                 512-byte bitstream blocks, ZRAM -> IDM (descriptor at 0x2B47)

Request/reply contract implemented here (v04 bank 0x03; see README "DSP model"):
  0x11 -> MCU writes c_mp3deSecondRestFlag to port 0x34 (0 play, 1 resume, 0xFF duration scan)
          (+3 bytes to IDM 0x3E08 when 1), replies 0x22
  0xEE -> DSP sets 0x31 = sync status (!=0; 100 requests with 0 -> "not an MP3"), 0x32/0x33 =
          signed 16-bit sector step for the next read (1 = sequential; hook m_Mp3deRead_Data
          bank 0x21 0x1D2C: anything else -> FS_FSeek(step, relative) first), 0x34 = 0x11
          (forward; 0x22 = backward); MCU (first time) writes the
          file length to IDM word 0x1C03 (L,M,H) + 0x1C04, DMAs 512 bytes, sets port 0x30
          (0 = data, 1/2 = FF/REW markers, 3 = no more data) and 0x33, replies 0xDD
  0xCC -> frame info: 0x30 mode, 0x31-0x33 bitrate as 3 ASCII digits, 0x34 DAC rate code
          (port 0x81), IDM-L 0x1C01 = total sec, 0x1C02 = min, 0x1D52 = hour; MCU replies 0x99
          (no reply in scan mode: it sets DecodeInfor = 0xFE and resets the DSP)
  0xBB -> status/params: IDM-L 0x1D57 hour, 0x1D58 min, 0x1D59 sec, 0x1D5A/B ms (LE) =
          current play time -> c_mp3deNowTime*; MCU writes EQ/AB params, replies 0x00
  0xAA -> end of stream (no reply; MCU sets DecodeInfor = 0xFE)
  (0x55 -> 0x66, 0xA8 -> 0x95, 0xA9 -> 0x96, 0x77 -> 0x88, 0x2B -> 0x3B, 0xE0 are understood
   by the driver but not needed by this model.)

Program recognition (lisong_rebuild/analysis/mp3_driver_spec.md section 1): when port 0x2E bit 7
rises, the model fingerprints DSP program memory (CRC32 of IPM lanes L|M|H, words 0..n-1) and
compares it with every ADEF .DSP file of the firmware image plus a built-in fingerprint of the
v04 MP3DEC.DSP. So a DSP program written into the lanes by any loader (e.g. zos through port 0x05
pages 0xC0-0xC2 / 0x34-0x36) is recognised; the InitDSPCode name hook is only a fallback.

Hardware rules the model enforces for from-scratch drivers (logged once each):
  - the DSP only runs while 0x2E bit 1 (DSPCKEN) is set and bit 7 (run) is 1
  - IPM halves owned by the MCU (port 05 bits 7/6) while the DSP runs -> warning
  - PCM is audible only while the DAC is on: port 0x80 bit 5 = 1, bit 1 (mute) = 0 and port 0x8B
    bit 4 (FIFO counter run) = 1 (otherwise silence is emitted, timing is kept)
  - port 0x81 low nibble must match the stream's rate code (warning otherwise)
  - bitstream DMA: any DMA1 into an IDM lane while 0xEE is pending is taken; a descriptor that
    differs from the stock one (DA 0x1CB3, lane 6, mode 0x30, 512 bytes) is reported
"""
import io
import struct
import zlib

import numpy as np

BR1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
BR2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0]
SR = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}
# DAC sample-rate codes for port 0x81 (ATJ2085X PG p.51-52). The codec (c_mp3deChangDAC3fs,
# v04 bank 0x03 0x2ABC) writes the DSP's code to port 0x81 and tests its bit 7 (SDK label
# "_sample_44_10_22_05") to select the 11.2896 MHz PLL (port 0x42 |= 3) -> the DSP reports the
# 44.1 kHz family with bit 7 set.
DAC_CODE = {48000: 0x0, 24000: 0x2, 12000: 0x3, 32000: 0x4, 16000: 0x6, 8000: 0x7,
            44100: 0x88, 22050: 0x8A, 11025: 0x8B}

# CRC32 of IPM lanes L|M|H (0x2400 words each) of LSD_501_04 (v04) MP3DEC.DSP
BUILTIN_PM_FINGERPRINTS = {('mp3dec.dsp (v04 builtin)', 0x2400): 0x387A5BB9}


def adef_pm_fingerprint(data):
    """(pm_words, crc32 of PM lanes L|M|H) of an ADEF .DSP image, or None."""
    if len(data) < 0x80 or data[:4] != b'ADEF':
        return None
    lanes = []
    n = None
    for i in range(3, 6):                       # PMLS, PMMS, PMHS
        tag = data[0x10 + i * 8:0x14 + i * 8]
        if tag[:2] != b'PM':
            return None
        sec, cnt = struct.unpack_from('<HH', data, 0x14 + i * 8)
        ln = struct.unpack_from('<H', data, 0x40 + (i + 1) * 8 + 2)[0] or cnt * 512
        n = ln if n is None else min(n, ln)
        lanes.append(data[sec * 512:sec * 512 + ln])
    if not n:
        return None
    return n, zlib.crc32(b''.join(l[:n] for l in lanes)) & 0xffffffff


def id3v2_size(b):
    if len(b) >= 10 and b[:3] == b'ID3':
        return 10 + ((b[6] & 0x7f) << 21 | (b[7] & 0x7f) << 14 | (b[8] & 0x7f) << 7 | (b[9] & 0x7f))
    return 0


def parse_header(b, off):
    if off + 4 > len(b):
        return None
    h = struct.unpack_from('>I', b, off)[0]
    if (h >> 21) & 0x7ff != 0x7ff:
        return None
    ver = (h >> 19) & 3          # 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    layer = (h >> 17) & 3        # 1 = layer III
    bri = (h >> 12) & 0xf
    sri = (h >> 10) & 3
    if ver == 1 or layer != 1 or bri in (0, 15) or sri == 3:
        return None
    br = (BR1 if ver == 3 else BR2)[bri]
    sr = SR[ver][sri]
    pad = (h >> 9) & 1
    mode = (h >> 6) & 3
    spf = 1152 if ver == 3 else 576
    flen = (144 if ver == 3 else 72) * br * 1000 // sr + pad
    return dict(ver=ver, br=br, sr=sr, mode=mode, spf=spf, flen=flen, ch=1 if mode == 3 else 2)


def find_first_frame(b, start):
    """First header at >= start that is followed by another valid header (or end of data)."""
    for off in range(start, max(start, len(b) - 4)):
        if b[off] != 0xff:
            continue
        h = parse_header(b, off)
        if not h:
            continue
        nxt = off + h['flen']
        if nxt + 4 > len(b):
            return None           # need more data to confirm
        h2 = parse_header(b, nxt)
        if h2 and h2['sr'] == h['sr'] and h2['ver'] == h['ver']:
            return off, h
    return None


def xing_frames(b, off, h):
    side = (32 if h['ch'] == 2 else 17) if h['ver'] == 3 else (17 if h['ch'] == 2 else 9)
    p = off + 4 + side
    if b[p:p + 4] in (b'Xing', b'Info') and len(b) >= p + 12:
        flags = struct.unpack_from('>I', b, p + 4)[0]
        if flags & 1:
            return struct.unpack_from('>I', b, p + 8)[0]
    return None


class _Stream(io.RawIOBase):
    """File-like view of the bitstream the DSP has received so far (for pymp3)."""
    def __init__(self, buf, pos):
        self.buf, self.pos = buf, pos

    def readable(self):
        return True

    def readinto(self, out):
        n = min(len(out), len(self.buf) - self.pos)
        out[:n] = self.buf[self.pos:self.pos + n]
        self.pos += n
        return n


class DspHLE:
    REQ_GAP = 20000          # T-states between two DSP requests (DSP "processing time")
    LOW_WATER = 8192         # keep at least this many undecoded bitstream bytes buffered
    BB_PERIOD = 0.25         # seconds of audio between two 0xBB (time report) requests

    def __init__(self, soc, sink=None, pacing='emu', log=print, verbose=False):
        self.soc = soc
        self.sink = sink
        self.pacing = pacing           # 'emu' = emulated clock, 'live' = audio device clock
        self.log = log
        self.verbose = verbose
        self.program = None
        self.ctrl = 0x01
        self.to_mcu = bytearray(8)     # what the MCU reads at 0x30..0x37
        self.from_mcu = bytearray(8)   # what the MCU wrote at 0x30..0x37
        self.running = False
        self.stats = dict(starts=0, requests={}, bitstream=0, samples=0)
        self.history = []              # (tick, event) for tests / README
        self.loaded_name = None        # from the InitDSPCode hook (fallback only)
        self.identified = None         # how the running program was recognised
        self._fingerprints = None
        self._warned = set()
        self._reset_state()

    def _warn(self, key, msg):
        if key not in self._warned:
            self._warned.add(key)
            self._ev('WARNING: ' + msg)
            self.log('[%08d] DSP WARNING: %s' % (self.soc.ticks, msg))

    # ---------------------------------------------------------------- program recognition
    def _known_programs(self):
        """{(name, pm_words): crc} from the firmware image's ADEF .DSP files + builtin."""
        if self._fingerprints is None:
            fp = {}                       # firmware files first (their names win), then builtin
            fw = getattr(self.soc, 'fw', None)
            for name in sorted(getattr(fw, 'files', {}) or {}):
                if not name.endswith('.DSP'):
                    continue
                try:
                    r = adef_pm_fingerprint(fw.file(name))
                except Exception:
                    r = None
                if r:
                    fp[(name.lower(), r[0])] = r[1]
            for k, v in BUILTIN_PM_FINGERPRINTS.items():
                if v not in fp.values():
                    fp[k] = v
            self._fingerprints = fp
        return self._fingerprints

    def identify_program(self):
        """Fingerprint the program memory as it is now -> program name or None."""
        s = self.soc
        cache = {}
        for (name, n), crc in self._known_programs().items():
            if n not in cache:
                buf = bytearray()
                for lane in (0, 1, 2):
                    buf += bytes(s.dsp_read(lane, w) for w in range(n))
                cache[n] = zlib.crc32(buf) & 0xffffffff
            if cache[n] == crc:
                return name
        return None

    # ---------------------------------------------------------------- lifecycle
    def _reset_state(self):
        self.pending = None
        self.next_req = 0
        self.mode = None
        self.bitstream = bytearray()
        self.audio_off = None
        self.header = None
        self.total_s = 0
        self.eof = False
        self.dec = None
        self.dec_stream = None
        self.exhausted = False
        self.playing = False
        self.t0 = 0
        self.pos = 0                   # samples delivered to the sink
        self.last_bb = 0
        self.finished = False
        self.file_len = 0

    def on_load(self, name, part):
        """InitDSPCode hook (stock OS path). Only a hint: the program that actually runs is
        identified from program memory when the DSP is started."""
        self.loaded_name = name.lower()
        self._ev('load %s part %d' % (name, part))

    def _ev(self, s):
        self.history.append((self.soc.ticks, s))
        if self.verbose:
            self.log('[%08d] DSP %s' % (self.soc.ticks, s))

    def _start(self):
        self._reset_state()
        self._warned = set()
        p05 = self.soc.ports[0x05] if hasattr(self.soc, 'ports') else 0
        name = self.identify_program()
        if name:
            self.program, self.identified = name, 'fingerprint'
        elif self.loaded_name:
            # unknown PM contents but the stock loader named the file (e.g. a firmware whose
            # MP3DEC differs from the known fingerprints)
            self.program, self.identified = self.loaded_name, 'InitDSPCode name'
        else:
            self.program, self.identified = None, None
        if not self.program or not self.program.startswith('mp3dec'):
            self.log('[%08d] DSP: program %r (%s) has no HLE model - DSP stays silent' %
                     (self.soc.ticks, self.program or 'unknown PM contents', self.identified))
            self._ev('run: no model for %r' % (self.program or 'unknown PM contents'))
            self.running = False
            return
        if p05 & 0xC0:
            self._warn('pm_owned', 'DSP started while port 05 = %02X gives an IPM half to the MCU'
                       ' (bits 7/6); the real DSP would fetch garbage' % p05)
        if p05 & 0x30:
            self._warn('dm_owned', 'DSP started while port 05 = %02X gives an IDM half to the MCU'
                       ' (bits 5/4)' % p05)
        self.running = True
        self.stats['starts'] += 1
        self.next_req = self.soc.ticks + 60000
        self._ev('run (%s, identified by %s)' % (self.program, self.identified))

    def _stop(self):
        if self.running:
            self._ev('reset (pos %.2f s)' % (self.pos / self.header['sr'] if self.header else 0))
        self.running = False
        self.pending = None

    # ---------------------------------------------------------------- MCU side
    def mcu_in(self, p):
        return self.to_mcu[p - 0x30]

    def mcu_out(self, p, v):
        if p == 0x2e:
            old, self.ctrl = self.ctrl, v
            if v & 0x80 and not old & 0x80:
                self._start()
            elif old & 0x80 and not v & 0x80:
                self._stop()
            return
        self.from_mcu[p - 0x30] = v
        if p == 0x35:
            self._on_reply(v)
        elif p == 0x37 and v & 0x40:
            self._ev('MCU set HSR7 bit6 (codec watchdog: DSP silent too long)')

    def _raise(self, code, **ports):
        for k, v in ports.items():
            self.to_mcu[int(k[1:], 16) - 0x30] = v
        self.to_mcu[5] = code
        self.pending = code
        self.stats['requests'][code] = self.stats['requests'].get(code, 0) + 1
        self.soc.ports[0x26] |= 0x01          # IRQ_DSP pending (MINT_STATUS bit0)
        if code != 0xEE or self.verbose:
            self._ev('request %02X' % code)

    def _on_reply(self, v):
        pc = self.pending
        if pc is None:
            if self.running:
                self._ev('reply %02X without request' % v)
            return
        self.pending = None
        self.next_req = self.soc.ticks + self.REQ_GAP
        if pc == 0x11:
            self.mode = self.from_mcu[4]
            self._ev('0x11 -> mode %02X (reply %02X)' % (self.mode, v))
        elif pc == 0xEE:
            st = self.from_mcu[0]
            if st == 3 and not self.eof:
                self.eof = True
                self._ev('end of data (port 0x30 = 3), %d bytes received' % len(self.bitstream))
        elif pc == 0xCC:
            self.playing = True
            self.t0 = self.soc.ticks
            self._ev('frame info acknowledged (reply %02X), playing' % v)

    STOCK_BITSTREAM_DMA = dict(da=0x1CB3, ida=6, n=512)

    def on_dma(self, st, data):
        if not (self.running and st['da3'] & 0x40 and st['ida'] in (4, 5, 6)):
            return
        if self.pending != 0xEE:
            self._warn('dma_nopend', 'DMA into IDM (DA %04X lane %d, %d bytes) without a pending '
                       '0xEE request - ignored by the DSP model' % (st['da'], st['ida'], st['n']))
            return
        if (st['da'] & 0x3fff, st['ida'], st['n']) != (0x1CB3, 6, 512) or st['mode'] & 0x38 != 0x30:
            self._warn('dma_desc', 'bitstream DMA differs from the stock descriptor (got DA %04X '
                       'lane %d mode %02X n %d; stock: DA 1CB3 lane 6 (IDM-H) mode 30 n 512) - '
                       'the model accepts it; the real MP3DEC expects the stock layout'
                       % (st['da'], st['ida'], st['mode'], st['n']))
        if self.file_len == 0:
            s = self.soc
            self.file_len = (s.dsp_read(4, 0x1c03) | s.dsp_read(5, 0x1c03) << 8 |
                             s.dsp_read(6, 0x1c03) << 16 | s.dsp_read(4, 0x1c04) << 24)
            self._ev('file length from IDM 0x1C03/04: %d' % self.file_len)
        if self.file_len and len(self.bitstream) + len(data) >= self.file_len:
            # the DSP knows the file length (IDM 0x1C03) and stops asking for more; the
            # driver keeps DMAing whole sectors (the last one is padded)
            data = data[:max(0, self.file_len - len(self.bitstream))]
            if not self.eof:
                self.eof = True
                self._ev('received the whole file (%d bytes)' % self.file_len)
        self.bitstream += data
        self.stats['bitstream'] += len(data)

    # ---------------------------------------------------------------- DSP side
    def _idm_l(self, addr, val):
        self.soc.dsp_write(4, addr, val)

    def _unread(self):
        if self.dec_stream is None:
            return len(self.bitstream) - (self.audio_off or 0)
        return len(self.bitstream) - self.dec_stream.pos

    def _try_header(self):
        b = self.bitstream
        if self.audio_off is None:
            if len(b) < 10:
                return False
            n = id3v2_size(b)
            if len(b) < n + 4:
                return False
            self.audio_off = n
        r = find_first_frame(b, self.audio_off)
        if r is None:
            return False
        off, h = r
        self.audio_off = off
        self.header = h
        nfr = xing_frames(b, off, h)
        if nfr:
            self.total_s = nfr * h['spf'] / h['sr']
        else:
            payload = (self.file_len or len(b)) - off
            self.total_s = payload * 8 / (h['br'] * 1000)
        self._ev('first frame @%d: MPEG%s L3 %d kbps %d Hz %s, total %.2f s%s' % (
            off, {3: '1', 2: '2', 0: '2.5'}[h['ver']], h['br'], h['sr'],
            ['stereo', 'joint', 'dual', 'mono'][h['mode']], self.total_s,
            ' (Xing)' if nfr else ' (len/bitrate)'))
        return True

    def _info_ports(self):
        """Frame-info mailbox values; the driver reads 0x30-0x34 on 0xCC *and* on every 0xBB
        (bank 0x03 0x2651 -> 0x283B -> 0x285A), so both requests must carry them."""
        h = self.header
        br = ('%3d' % h['br']).encode()[-3:]
        return dict(p30=h['mode'], p31=br[0], p32=br[1], p33=br[2], p34=DAC_CODE.get(h['sr'], 8))

    def _send_cc(self):
        t = int(round(self.total_s))
        self._idm_l(0x1c01, t % 60)
        self._idm_l(0x1c02, (t // 60) % 60)
        self._idm_l(0x1d52, t // 3600)
        self._raise(0xCC, **self._info_ports())

    def _send_bb(self):
        sr = self.header['sr']
        ms_total = int(self.pos * 1000 // sr)
        s, ms = divmod(ms_total, 1000)
        self._idm_l(0x1d57, s // 3600)
        self._idm_l(0x1d58, (s // 60) % 60)
        self._idm_l(0x1d59, s % 60)
        self._idm_l(0x1d5a, ms & 0xff)
        self._idm_l(0x1d5b, ms >> 8)
        self.last_bb = self.pos
        self._raise(0xBB, **self._info_ports())

    def _dac_on(self, sr):
        """DAC state as programmed by the MCU (ports 0x80/0x81/0x8B/0x9E/0xDD, PG p.51-54)."""
        p = self.soc.ports
        on = bool(p[0x80] & 0x20) and not p[0x80] & 0x02 and bool(p[0x8b] & 0x10)
        if not on:
            self._warn('dac_off', 'PCM produced while the DAC is off/muted (port 80 = %02X, '
                       '8B = %02X; need 80 bit5=1, bit1=0 and 8B bit4=1) - emitting silence'
                       % (p[0x80], p[0x8b]))
        want = DAC_CODE.get(sr)
        if want is not None and (p[0x81] ^ want) & 0x0f:
            self._warn('dac_rate', 'port 81 = %02X but the stream is %d Hz (code %X): the real '
                       'DAC would play at the wrong speed' % (p[0x81], sr, want & 0x0f))
        if on and want is not None and (p[0x42] & 0x01) != (1 if want & 0x80 else 0):
            self._warn('dac_pll', 'port 42 bit0 (FSSEL) = %d but %d Hz needs the %s MHz audio '
                       'clock (42 bits 0-1 = %s)' % (p[0x42] & 1, sr,
                                                     '11.2896' if want & 0x80 else '12.288',
                                                     '11' if want & 0x80 else '00'))
        if on and not (p[0xdd] & 0xc0) == 0xc0:
            self._warn('pa_off', 'DAC on but port DD = %02X: analog DAC/PA not enabled '
                       '(PA_Enable sets bits 7,6) - the headphones would be silent' % p[0xdd])
        return on

    def _render(self):
        """Decode and deliver PCM up to the pacing target."""
        h = self.header
        sr = h['sr']
        if self.pacing == 'live' and self.sink is not None and self.sink.live:
            def want():
                return self.sink.buffered_seconds() < 0.35
        else:
            target = (self.soc.ticks - self.t0) * sr // self.soc.CPU_HZ

            def want():
                return self.pos < target
        while want() and not self.exhausted:
            if self.dec is None:
                if self._unread() < 8192 and not self.eof:
                    return
                import mp3
                self.dec_stream = _Stream(self.bitstream, self.audio_off)
                self.dec = mp3.Decoder(self.dec_stream)
            if self._unread() < 4096 and not self.eof:
                return                       # underrun: wait for more bitstream
            raw = self.dec.read(4608)
            if not raw:
                self.exhausted = True
                self._ev('decoder drained at %.2f s' % (self.pos / sr))
                break
            pcm = np.frombuffer(raw, dtype='<i2')
            ch = self.dec.get_channels() or h['ch']
            pcm = pcm.reshape(-1, ch)
            if ch == 1:
                pcm = np.repeat(pcm, 2, axis=1)
            dsr = self.dec.get_sample_rate() or sr
            if not self._dac_on(dsr):
                pcm = np.zeros_like(pcm)
            if self.sink is not None:
                self.sink.write(pcm, dsr)
            self.pos += len(pcm)
            self.stats['samples'] += len(pcm)

    def step(self):
        if not self.running or self.finished:
            return
        if not self.ctrl & 0x02:
            # DSPCKEN = 0: the DSP clock is stopped (also during the stock 0x2B overlay load)
            if self.stats['starts'] and 'noclk' not in self._warned and self.pending is None \
                    and self.mode is None:
                self._warn('noclk', 'DSP released from reset (0x2E bit 7) with its clock off '
                           '(0x2E bit 1 = 0): nothing runs until DSPCKEN is set')
            if self.playing:
                self.t0 += self.soc.ticks - getattr(self, '_last_step', self.soc.ticks)
            self._last_step = self.soc.ticks
            return
        self._last_step = self.soc.ticks
        if self.pending is not None:
            return
        if self.soc.ticks < self.next_req:
            return
        if self.playing:
            self._render()
        if self.mode is None:
            self._raise(0x11)
            return
        if self.header is None:
            if self._try_header():
                self._send_cc()
            elif not self.eof:
                self._raise(0xEE, p31=1, p32=1, p33=0, p34=0x11)
            else:
                self._ev('no MP3 frame found - end of stream')
                self.finished = True
                self._raise(0xAA)
            return
        if not self.playing:
            return
        if self._unread() < self.LOW_WATER and not self.eof:
            self._raise(0xEE, p31=1, p32=1, p33=0, p34=0x11)
            return
        if self.pos - self.last_bb >= self.BB_PERIOD * self.header['sr']:
            self._send_bb()
            return
        if self.exhausted:
            live_left = self.sink.buffered_seconds() if (self.sink and self.sink.live) else 0
            if live_left < 0.05:
                self.finished = True
                self._ev('end of stream at %.2f s' % (self.pos / self.header['sr']))
                self._raise(0xAA)
