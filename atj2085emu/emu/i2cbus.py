"""Optional FM tuner models on the board's bit-banged I2C bus (GPIO C: port 0xF4 bit0 SCL, bit1 SDA;
port 0xF3 bit1 = SDA output enable). Enabled with ATJ2085(tuner='tea'|'rda'|'none') / --tuner; without it
the GPIO C pins stay plain latches (old behaviour: SDA reads back the master's last bit, so every
address seems to ACK and FM shows a TEA5767).

  'tea' : TEA5767 at 0x60 (5-byte write / 5-byte read frames, PLL with high-side injection)
  'none': the bus with its pull-ups and no tuner (every address NACKs)
  'rda' : RDA5807M (the "RRD-102" module): 0x10 sequential access (writes from reg 02h, reads from
          0Ah), 0x11 random access (register index, then data; reads after a repeated START),
          0x60 TEA5767-compatible mode; chip ID 0x5804; tune / seek with STC/SF; RSSI/ST/FM_TRUE;
          a scripted RDS stream (groups 0A + 2A every ~88 ms, with block errors) on the stations
          that have RDS.

The bus model decodes START/STOP and bits from the pin writes; the slave drives SDA (reads as the
port value while the master's SDA output is disabled). Time = soc.ticks (T-states, CPU_HZ/s).
"""

# (frequency in 100 kHz units, level 0..15, PI, PS, RadioText or None for no RDS)
STATIONS = [
    (915, 13, 0xC201, 'ZOS TEST', 'HELLO FROM THE RDS TEST STREAM'),
    (987, 11, None, None, None),
    (1015, 9, 0xC2A7, 'Radio-2 ', 'Ca\x82n lowercase + accents \x86k'),
    (1051, 8, None, None, None),
]


def station_at(f10):
    """(level, station tuple or None) heard at f10: full level on the channel, -5 per 100 kHz."""
    best, st = 0, None
    for s in STATIONS:
        lv = s[1] - 5 * abs(s[0] - f10)
        if lv > best:
            best, st = lv, (s if s[0] == f10 else None)
    return max(best, 1), st


class I2CBus:
    def __init__(self, soc, kind, log=print):
        self.soc, self.log = soc, log
        self.scl = self.sda = 1
        self.slave_low = False
        self.state = 'idle'      # idle / addr / wr / rd
        self.bit = 0
        self.sh = 0
        self.dev = None
        self.out = 0
        self.mack = True
        self.devs = {}
        if kind == 'tea':
            self.devs[0x60] = Tea5767(soc)
        elif kind == 'rda':
            r = Rda5807(soc, log)
            self.devs.update({0x10: r.seq, 0x11: r.rnd, 0x60: r.tea})
        elif kind != "none":                 # none: pulled-up bus, nothing ACKs
            raise ValueError(kind)
        self.rda = self.devs[0x10].chip if 0x10 in self.devs else None

    # pins --------------------------------------------------------------------------------------
    def _line_sda(self):
        p = self.soc.ports
        if p[0xf3] & 0x02:
            return (p[0xf4] >> 1) & 1
        return 0 if self.slave_low else 1

    def port_in(self, v):
        """value read from port 0xF4: SDA shows the line while the master's output is off"""
        if not (self.soc.ports[0xf3] & 0x02):
            v = (v & ~0x02) | (self._line_sda() << 1)
        return v

    def pins_changed(self):
        p = self.soc.ports
        scl = p[0xf4] & 1 if p[0xf3] & 1 else 1
        sda = self._line_sda()
        if scl and self.scl and sda != self.sda:
            if not sda:
                self._start()
            else:
                self._stop()
        elif scl and not self.scl:
            self._rise(sda)
        elif not scl and self.scl:
            self._fall()
        self.scl, self.sda = scl, self._line_sda()

    # protocol ----------------------------------------------------------------------------------
    def _start(self):
        if self.dev is not None and self.state != 'idle':
            self.dev.restart()
        self.state, self.bit, self.sh, self.slave_low = 'addr', 0, 0, False

    def _stop(self):
        if self.dev is not None:
            self.dev.stop()
        self.dev = None
        self.state, self.slave_low = 'idle', False

    def _rise(self, sda):
        if self.state in ('addr', 'wr'):
            if self.bit < 8:
                self.sh = ((self.sh << 1) | sda) & 0xff
            self.bit += 1
        elif self.state == 'rd':
            if self.bit == 8:
                self.mack = sda == 0
            self.bit += 1

    def _fall(self):
        if self.state in ('addr', 'wr'):
            if self.bit == 8:                                 # byte in: the slave ACKs or not
                b = self.sh
                if self.state == 'addr':
                    d = self.devs.get(b >> 1)
                    ack = d is not None and d.begin(b & 1)
                    if ack:
                        self.dev, self.rw = d, b & 1
                else:
                    ack = self.dev.write(b)
                self.slave_low = bool(ack)
                if not ack:
                    self.state = 'nack'
            elif self.bit == 9:                               # ACK slot done
                self.slave_low = False
                self.bit, self.sh = 0, 0
                if self.state == 'addr' and self.rw:
                    self.state = 'rd'
                    self._load()
                else:
                    self.state = 'wr'
        elif self.state == 'rd':
            if self.bit < 8:
                self.slave_low = not ((self.out >> (7 - self.bit)) & 1)
            elif self.bit == 8:
                self.slave_low = False                        # master's ACK slot
            else:
                if self.mack:
                    self._load()
                else:
                    self.state = 'nack'
                    self.slave_low = False
        elif self.state == 'nack':
            self.slave_low = False

    def _load(self):
        self.out = self.dev.read() & 0xff
        self.bit = 0
        self.slave_low = not (self.out & 0x80)


class Tea5767:
    """TEA5767: write W0..W4 (PLL 14 bits, HLSI in W2 bit4, standby W3 bit6), read R0..R4."""
    def __init__(self, soc, chip=None):
        self.soc = soc
        self.chip = chip            # RDA in TEA-compatible mode shares the tuning
        self.w = [0] * 5
        self.f10 = 0
        self.n = 0
        self.rn = 0

    def begin(self, rd):
        self.n = self.rn = 0
        return True

    def restart(self):
        pass

    def write(self, b):
        if self.n < 5:
            self.w[self.n] = b
        self.n += 1
        if self.n == 2 or self.n == 5:
            pll = ((self.w[0] & 0x3f) << 8) | self.w[1]
            khz = pll * 32768 / 4 / 1000.0
            khz = khz - 225 if self.w[2] & 0x10 else khz + 225
            self.f10 = int(round(khz / 100.0))
            if self.chip is not None:
                self.chip.tune_tea(self.f10)
        return True

    def read(self):
        f10 = self.chip.freq() if self.chip is not None else self.f10
        lv, st = station_at(f10)
        on = st is not None
        r = [0x80 | (self.w[0] & 0x3f), self.w[1], (0x80 if on and lv >= 10 else 0) | (0x37 if on else 0x50),
             lv << 4, 0]
        v = r[self.rn] if self.rn < 5 else 0xff
        self.rn += 1
        return v

    def stop(self):
        pass


class _Port:
    def __init__(self, chip, mode):
        self.chip, self.mode = chip, mode
        self.idx = 0
        self.hi = None
        self.first = False

    def begin(self, rd):
        self.hi = None
        if self.mode == "seq":
            self.idx = 0x0a if rd else 0x02
            self.hi = None
        elif not rd:
            self.first = True
        else:
            self.hi = None
        return True

    def restart(self):
        pass

    def write(self, b):
        if self.mode == 'rnd' and self.first:
            self.first = False
            self.idx = b & 0x1f
            self.hi = None
            return True
        if self.hi is None:
            self.hi = b
        else:
            self.chip.wreg(self.idx & 0x0f, (self.hi << 8) | b)
            self.idx += 1
            self.hi = None
        return True

    def read(self):
        r = self.idx & 0x0f
        v = self.chip.rreg(r)
        if self.hi is None:
            self.hi = 1
            return v >> 8
        self.hi = None
        self.idx += 1
        self.chip.read_word(r)
        return v & 0xff

    def stop(self):
        pass


class Rda5807:
    STEP_T = 0.015              # seconds per 100 kHz seek step
    TUNE_T = 0.010
    GROUP_T = 0.0877            # RDS: 11.4 groups/s

    def __init__(self, soc, log=print):
        self.soc, self.log = soc, log
        self.reg = [0] * 16
        self.reg[0] = 0x5804
        self.reg[2] = 0x0000
        self.reg[5] = 0x880f
        self.chan = 0
        self.busy = None        # ('tune', t_done) / ('seek', t_next, start_chan, up)
        self.stc = self.sf = 0
        self.rds_t = 0
        self.rds_n = 0
        self.rdsr = 0
        self.blk = [0, 0, 0, 0]
        self.bler = 0
        self.groups = 0
        self.seq, self.rnd = _Port(self, 'seq'), _Port(self, 'rnd')
        self.tea = Tea5767(soc, self)
        self.writes = []

    def _t(self, s):
        return int(s * self.soc.CPU_HZ)

    def freq(self):
        return 870 + self.chan

    def tune_tea(self, f10):
        self.chan = max(0, min(210, f10 - 870))

    def wreg(self, r, v):
        self.writes.append((self.soc.ticks, r, v))
        now = self.soc.ticks
        if r == 2:
            if v & 0x0002:                                  # SOFT_RESET
                self.reg[3] = self.reg[4] = 0
                self.reg[5] = 0x880f
                self.busy = None
            if (v & 0x0100) and not (self.reg[2] & 0x0100):  # SEEK 0 -> 1
                self.stc = self.sf = 0
                self.busy = ('seek', now + self._t(self.STEP_T), self.chan, bool(v & 0x0200))
            elif not (v & 0x0100) and self.busy and self.busy[0] == 'seek':
                self.busy = None                            # seek stopped by the host
            self.reg[2] = v
            return
        if r == 3:
            self.reg[3] = v
            if v & 0x0010 and self.reg[2] & 1:              # TUNE
                self.stc = self.sf = 0
                self.chan = min(210, v >> 6)
                self.busy = ('tune', now + self._t(self.TUNE_T))
                self._rds_reset()
            return
        self.reg[r] = v

    def _rds_reset(self):
        self.rdsr = 0
        self.rds_n = 0
        self.rds_t = self.soc.ticks + self._t(0.3)          # sync takes a while

    def update(self):
        now = self.soc.ticks
        b = self.busy
        if b and b[0] == 'tune' and now >= b[1]:
            self.busy = None
            self.stc = 1
            self.reg[3] &= ~0x0010
        elif b and b[0] == 'seek':
            _, t, start, up = b
            th = (self.reg[5] >> 8) & 0x0f
            while self.busy and now >= t:
                self.chan = (self.chan + (1 if up else -1)) % 211
                lv, st = station_at(self.freq())
                if st is not None and lv * 4 >= th * 4:
                    self.busy, self.stc = None, 1
                elif self.chan == start:
                    self.busy, self.stc, self.sf = None, 1, 1
                else:
                    t += self._t(self.STEP_T)
                    self.busy = ('seek', t, start, up)
            if not self.busy:
                self.reg[2] &= ~0x0100
                self._rds_reset()
        # RDS
        en = (self.reg[2] & 0x0009) == 0x0009 and self.busy is None
        lv, st = station_at(self.freq())
        if en and st is not None and st[2] is not None and now >= self.rds_t:
            self.rds_t = now + self._t(self.GROUP_T)
            self._next_group(st)

    def _next_group(self, st):
        _, _, pi, ps, rt = st
        n = self.rds_n
        self.rds_n += 1
        rtb = (rt + '\r').encode('latin-1')
        nseg = (len(rtb) + 3) // 4
        if n % 2 == 0:                                      # 0A: PS segment
            seg = (n // 2) % 4
            b = (0 << 12) | (seg & 3)
            c = 0xe0cd
            d = (ord(ps[seg * 2]) << 8) | ord(ps[seg * 2 + 1])
        else:                                               # 2A: RadioText segment, A/B flag 0
            seg = (n // 2) % nseg
            ch = rtb[seg * 4:seg * 4 + 4].ljust(4, b' ')
            b = (2 << 12) | seg
            c = (ch[0] << 8) | ch[1]
            d = (ch[2] << 8) | ch[3]
        bler = 0
        self.groups += 1
        if self.groups % 7 == 0:                            # uncorrectable block B
            b ^= 0x5a5a
            bler = 0x3
        elif self.groups % 11 == 0:                         # silent error in block D
            d ^= 0x2104
        elif self.groups % 5 == 0:                          # corrected errors in A
            bler = 0x4
        self.blk = [pi, b, c, d]
        self.bler = bler
        self.rdsr = 1

    def rreg(self, r):
        self.update()
        if r == 0x0a:
            lv, st = station_at(self.freq())
            return ((self.rdsr << 15) | (self.stc << 14) | (self.sf << 13) |
                    ((1 << 12) if self.rdsr else 0) |
                    ((1 << 10) if lv >= 10 and st is not None else 0) | (self.chan & 0x3ff))
        if r == 0x0b:
            lv, st = station_at(self.freq())
            rssi = min(127, lv * 4 + 3)
            return (rssi << 9) | ((1 << 8) if st is not None else 0) | 0x80 | self.bler
        if 0x0c <= r <= 0x0f:
            return self.blk[r - 0x0c]
        return self.reg[r]

    def read_word(self, r):
        if r == 0x0f:
            self.rdsr = 0                                    # group consumed
