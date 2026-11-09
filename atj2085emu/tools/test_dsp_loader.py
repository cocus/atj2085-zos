"""Port-level test of the DSP model with a NON-stock loader and driver.

Everything here is done the way a from-scratch OS (zos) would do it on the chip, through
the emulated ports and the 0x4000-0x7FFF window only - no InitDSPCode, no MUSIC.AP:
  1. load MP3DEC.DSP's six ADEF sections into the DSP lanes via port 0x05
     (DM L/M/H = pages 0x34/0x35/0x36, PM L/M/H = 0xC0/0xC1/0xC2, word w at 0x4000+w)
  2. DSP reset/clock/start (port 0x42, 0x2E, 0x2F, 0x27) as RestDsp + StartDSP do
  3. DAC + PA on (ports 0x80, 0x8B, 0xDD, 0x9E) as DAC_Enable / PA_Enable do
  4. service the DSP requests (0x11, 0xEE + DMA1, 0xCC, 0xBB, 0xAA) by polling port 0x26 bit 0
  5. stop the DSP (STOPDSP)
It follows lisong_rebuild/analysis/mp3_driver_spec.md section 8 (driver pseudo-code).

Checks: the program is recognised by fingerprint (no InitDSPCode hook involved), the scan
pass reports the duration, play mode delivers the whole file's PCM identical to a direct
pymp3 decode, no model warnings; a corrupted program is NOT recognised.

usage: python tools/test_dsp_loader.py [fw] [mp3 ...]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import numpy as np                      # noqa: E402

from emu.fwimage import FwImage          # noqa: E402
from emu.soc import ATJ2085              # noqa: E402
from emu.dsp import DspHLE               # noqa: E402

LANE_PAGE = {b'DMLS': 0x34, b'DMMS': 0x35, b'DMHS': 0x36,
             b'PMLS': 0xC0, b'PMMS': 0xC1, b'PMHS': 0xC2}
BITSTREAM_DESC = bytes([0x00, 0x00, 0x00, 0xB3, 0x1C, 0x00, 0x40, 0x06, 0xFF, 0x01, 0x30])  # ports 08..12
BUF = 0x3000                              # 512-byte bitstream buffer in ZRAM1 (any MCU address)


class Sink:
    live = False

    def __init__(self):
        self.chunks = []
        self.rate = None

    def write(self, pcm, rate):
        self.rate = rate
        self.chunks.append(pcm.copy())

    def pcm(self):
        return np.concatenate(self.chunks) if self.chunks else np.zeros((0, 2), np.int16)


class Host:
    """Minimal port-level MP3 host driver (Python stand-in for the zos Z80 code)."""

    def __init__(self, soc):
        self.s = soc
        self.log = []

    def out(self, p, v):
        self.s._out(p, v & 0xff)

    def inp(self, p):
        return self.s._in(p)

    def wr(self, page, addr, val):         # MCU write into the 0x4000 window under page
        self.out(0x05, page)
        self.s.mem[addr] = val & 0xff

    def rd(self, page, addr):
        self.out(0x05, page)
        return self.s.mem[addr]

    # -- 1. loader (spec section 1)
    def load_adef(self, img, corrupt=False):
        assert img[:4] == b'ADEF'
        saved = self.inp(0x05)
        for i in range(6):
            tag = img[0x10 + i * 8:0x14 + i * 8]
            sec, cnt = struct.unpack_from('<HH', img, 0x14 + i * 8)
            data = bytearray(img[sec * 512:(sec + cnt) * 512])
            if corrupt and tag == b'PMLS':
                data[0x100] ^= 0x01
            self.out(0x05, LANE_PAGE[tag])
            self.s.mem[0x4000:0x4000 + len(data)] = data      # word w -> 0x4000 + w
        self.out(0x05, saved & 0x0F)        # all four halves back to the DSP (bits 7-4 = 0)

    # -- 2./3. reset, clock, DAC, start (spec sections 2 and 3)
    def start(self, volume=24):
        self.out(0x80, 0x00)                      # DAC_Enable(mode 0, 44.1 kHz)
        self.out(0x81, 0x08)
        self.out(0x42, self.inp(0x42) | 0x03)
        self.out(0x8B, self.inp(0x8B) & ~0x10)
        self.out(0x80, 0x28)
        self.out(0x8B, 0x18)
        self.out(0x9E, volume)                    # PA_Enable({1, volume})
        self.out(0x80, self.inp(0x80) & ~0x02)
        self.out(0xDD, self.inp(0xDD) | 0xC2)
        self.out(0x8B, 0x05)                      # c_mp3decINIT: out(0x8B, 5)
        self.out(0x42, (self.inp(0x42) & 0xE3) | 0x0B)   # RestDsp: DSP clock 24 MIPS step
        self.out(0x42, self.inp(0x42) | 0x40)     # PLL1 on
        self.s.ticks += 2 * 12000                 # 2 ms
        self.out(0x2E, 0x7B)                      # reset (bit7=0), clock on (bit1), IRQ2DSP idle
        self.out(0x2F, 0x03)
        self.out(0x27, self.inp(0x27) | 0x01)     # DSP interrupt enable
        self.out(0x2E, self.inp(0x2E) | 0x80)     # StartDSP: run

    def stop(self):
        self.out(0x27, self.inp(0x27) & 0xFE)     # STOPDSP + DisableDspInt
        self.out(0x26, 0x01)
        self.out(0x2E, self.inp(0x2E) & 0x7F)
        self.out(0x2E, self.inp(0x2E) & 0x7D)
        self.out(0x42, self.inp(0x42) & 0xBF)

    # -- 4. request service (spec section 4)
    def run(self, f, mode, max_ticks=3_000_000_000):
        s = self.s
        size = len(f)
        pos = 0
        first_ee = True
        buf = None
        res = dict(total=None, now=None, frames=0, done=False)
        end = s.ticks + max_ticks
        while s.ticks < end:
            s.ticks += 2000
            s.dsp.step()
            if not self.inp(0x26) & 0x01:
                continue
            self.inp(0x35)
            req = self.inp(0x35)
            self.out(0x26, 0x01)                  # ack
            saved05 = self.inp(0x05)
            if req == 0x11:
                self.out(0x34, mode)              # 0 play, 1 resume, 0xFF scan
                self.out(0x35, 0x22)
            elif req == 0xEE:
                step = self.inp(0x32) | self.inp(0x33) << 8
                self.inp(0x34)                    # 0x11 forward / 0x22 backward
                if first_ee:
                    first_ee = False
                    self.wr(0x24, 0x5C07, self.inp(0x9E))          # volume
                    self.wr(0x24, 0x5C03, size & 0xff)             # file length, 32 bit
                    self.wr(0x24, 0x5C04, size >> 24 & 0xff)
                    self.wr(0x25, 0x5C03, size >> 8 & 0xff)
                    self.wr(0x26, 0x5C03, size >> 16 & 0xff)
                    self.out(0x05, saved05)
                if step not in (0, 1):
                    pos += (step - 1) * 512               # inferred seek semantics (spec 5.3)
                if buf is None:
                    buf = f[pos:pos + 512]
                    pos += len(buf)
                if not buf:
                    self.out(0x30, 0x03)                  # no more data
                else:
                    s.mem[BUF:BUF + 512] = buf.ljust(512, b'\0')
                    self.out(0x06, BUF & 0xff)
                    self.out(0x07, BUF >> 8)
                    for i, v in enumerate(BITSTREAM_DESC):
                        self.out(0x08 + i, v)
                    self.out(0x13, 0x01)
                    while self.inp(0x13) & 0x01:
                        pass
                    self.out(0x30, 0x00)
                self.out(0x33, 0x01)
                self.out(0x35, 0xDD)
                buf = f[pos:pos + 512]                    # prefetch the next sector
                pos += len(buf)
            elif req == 0xCC:
                info = [self.inp(p) for p in range(0x30, 0x35)]
                sec, mn, hr = self.rd(0x24, 0x5C01), self.rd(0x24, 0x5C02), self.rd(0x24, 0x5D52)
                self.out(0x05, saved05)
                res['total'] = (hr, mn, sec)
                res['info'] = info
                if mode == 0xFF:
                    res['done'] = True
                    self.stop()
                    return res
                code = info[4]
                if (self.inp(0x81) ^ code) & 0x0F:        # c_mp3deChangDAC3fs
                    self.out(0x80, self.inp(0x80) & 0xDF)
                    self.out(0x81, code)
                    if code & 0x80:
                        self.out(0x42, self.inp(0x42) | 0x03)
                        s.ticks += 20 * 12000
                    else:
                        self.out(0x42, self.inp(0x42) & 0xFC)
                self.out(0x80, self.inp(0x80) | 0x20)
                self.out(0x8B, self.inp(0x8B) & 0xEF)
                self.out(0x8B, self.inp(0x8B) | 0x10)
                self.out(0x80, self.inp(0x80) & 0xFD)
                self.out(0x35, 0x99)
            elif req == 0xBB:
                t = [self.rd(0x24, 0x5D57 + i) for i in range(5)]
                self.wr(0x24, 0x5C07, 24)
                self.out(0x05, 0x00)
                res['now'] = (t[0], t[1], t[2], t[3] | t[4] << 8)
                res['frames'] += 1
                self.out(0x33, 0x00)
                self.out(0x34, 0x01)
                self.out(0x35, 0x00)
                self.out(0x05, saved05)
            elif req == 0xAA:
                res['done'] = True
                self.stop()
                return res
            else:
                self.log.append('unhandled request %02X' % req)
        return res


def main():
    fwp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'my-ximpro.fw')
    mp3s = sys.argv[2:] or [os.path.join(HERE, '..', 'testdisk', n)
                            for n in ('Sweep mono 22k.mp3', 'TONE440.MP3')]
    fw = FwImage(fwp)
    img = fw.file('MP3DEC.DSP')
    msgs = []
    ok = True

    def new_soc():
        soc = ATJ2085(fw, log=lambda *a: None, trace_syscalls=False)
        sink = Sink()
        soc.dsp = DspHLE(soc, sink, pacing='emu', log=msgs.append)
        return soc, sink

    # negative test: corrupted program memory must not be recognised
    soc, sink = new_soc()
    h = Host(soc)
    h.load_adef(img, corrupt=True)
    h.start()
    print('corrupted PM : running=%s program=%r' % (soc.dsp.running, soc.dsp.program))
    ok &= not soc.dsp.running

    for path in mp3s:
        f = open(path, 'rb').read()
        name = os.path.basename(path)
        # scan pass (duration)
        soc, sink = new_soc()
        h = Host(soc)
        h.load_adef(img)
        h.start()
        print('%s: program=%r identified by %s, InitDSPCode hook name=%r' % (
            name, soc.dsp.program, soc.dsp.identified, soc.dsp.loaded_name))
        ok &= soc.dsp.running and soc.dsp.identified == 'fingerprint' and soc.dsp.loaded_name is None
        r = h.run(f, 0xFF)
        print('  scan: total %s  info ports 30-34 = %s' % (r.get('total'), r.get('info')))
        ok &= r['done'] and r['total'] is not None
        # play pass
        soc, sink = new_soc()
        h = Host(soc)
        h.load_adef(img)
        h.start()
        t0 = soc.ticks
        r = h.run(f, 0x00)
        pcm = sink.pcm()
        secs = (soc.ticks - t0) / soc.CPU_HZ
        print('  play: done=%s  %d BB reports, last time %s, %d frames PCM @ %s Hz, %.2f s emulated,'
              ' DAC port81=%02X port42=%02X' % (r['done'], r['frames'], r['now'], len(pcm),
                                                 sink.rate, secs, soc.ports[0x81], soc.ports[0x42]))
        import io
        import mp3
        d = mp3.Decoder(io.BytesIO(f))
        raw = b''
        while True:
            x = d.read(4608)
            if not x:
                break
            raw += x
        ref = np.frombuffer(raw, '<i2').reshape(-1, d.get_channels() or 1)
        if ref.shape[1] == 1:
            ref = np.repeat(ref, 2, axis=1)
        same = len(ref) == len(pcm) and np.array_equal(ref, pcm)
        print('  PCM identical to a direct pymp3 decode: %s (%d vs %d frames)' % (same, len(pcm), len(ref)))
        ok &= r['done'] and same
        if h.log:
            print('  host:', h.log)
    warn = [m for m in msgs if 'WARNING' in m]
    print('model warnings: %d' % len(warn))
    for m in warn:
        print('  ', m)
    ok &= not warn
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
