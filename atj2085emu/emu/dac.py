"""Optional model of the MCU-fed DAC (ports 0x88-0x8B): an 8-word FIFO drained at the sample rate.

NOT enabled by default (the plain emulator treats the ports as latches: the FIFO never fills, so a
CPU that polls DACFIFO_FULL never waits and no underruns exist). `soc.dac = DacModel(soc)` turns it on:
  - OUT 0x8A (the H byte) pushes one 16-bit word ((H << 8) | M, signed) if the FIFO has room, else the word
    is lost (`overruns`)
  - IN 0x8B reads the port latch with bit 5 (DACFIFO_FULL) = 1 while 8 words are queued
  - every sample period (CPU_HZ / rate T-states) the DAC takes one L and one R word (two words) out;
    with fewer than two queued it is an underrun: the output is `hold` (the previous sample, the
    usual DAC behaviour: marked with `u` so a test can tell silence from holding)
  - OUT 0x8B with bit 4 (DACFIFO_RUN) clear resets the FIFO
Real hardware numbers (modules/common/snd.c, beep.c, usbaudio/dacfeed.s): 8 kHz, 8 words deep,
L and R word per sample. The T-state time base is soc.CPU_HZ (12 MHz GUESS in soc.py): the real core clock
decides how many instructions fit between two refills, so the numbers are in T-states first.
Time inside a run() slice is exact (soc.now()).
Output: `t` (T-state of each sample period), `l`, `r`, `u` (1 = underrun, sample held) lists; `pushes`:
(T-state, word) of every accepted word; `to_wav(path)` writes the stereo 8 kHz WAV (the held
samples included, so an underrun is audible/visible).
"""
import array
import wave


class DacModel:
    def __init__(self, soc, rate=8000, depth=8, cpu_hz=None):
        self.soc = soc
        self.rate = rate
        self.depth = depth
        self.period = (cpu_hz or soc.CPU_HZ) / rate           # T-states per sample period (stereo frame)
        self.fifo = []                                         # queued words
        self.m = 0                                             # latched M byte
        self.next_t = None                                     # next sample boundary (None: not started)
        self.hold = (0, 0)
        self.t = array.array('q')
        self.l = array.array('h')
        self.r = array.array('h')
        self.u = array.array('b')
        self.pushes = []                                       # (t, word)
        self.overruns = 0
        self.max_level = 0
        self.run = True
        self.log_pushes = True

    # time ---------------------------------------------------------------------------------
    def _advance(self, now):
        if self.next_t is None:
            return
        while self.next_t <= now:
            if len(self.fifo) >= 2:
                self.hold = (self.fifo[0], self.fifo[1])
                del self.fifo[:2]
                u = 0
            else:
                u = 1                                          # (a lone word waits for its partner: the pairing holds)
            self.t.append(int(self.next_t)); self.l.append(self.hold[0]); self.r.append(self.hold[1]); self.u.append(u)
            self.next_t += self.period

    # port hooks (soc.py) ----------------------------------------------------------------------
    def port_in(self, p, latch):
        if p == 0x8B:
            self._advance(self.soc.now())
            return (latch | 0x20) if len(self.fifo) >= self.depth else (latch & ~0x20)
        return latch

    def port_out(self, p, v):
        if p == 0x89:
            self.m = v
        elif p == 0x8A:
            now = self.soc.now()
            self._advance(now)
            if len(self.fifo) >= self.depth:
                self.overruns += 1
                return
            w = ((v << 8) | self.m) & 0xFFFF
            if w & 0x8000:
                w -= 0x10000
            self.fifo.append(w)
            self.max_level = max(self.max_level, len(self.fifo))
            if self.log_pushes:
                self.pushes.append((now, w))
            if self.next_t is None:                            # first word starts the sample clock
                self.next_t = now + self.period
        elif p == 0x8B:
            if not v & 0x10:                                   # FIFO counter in reset
                self._advance(self.soc.now())
                self.fifo.clear()
                self.next_t = None

    def flush(self):
        self._advance(self.soc.now())

    def to_wav(self, path):
        with wave.open(path, 'wb') as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(self.rate)
            buf = array.array('h')
            for a, b in zip(self.l, self.r):
                buf.append(a); buf.append(b)
            w.writeframes(buf.tobytes())
