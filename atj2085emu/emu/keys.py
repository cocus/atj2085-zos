"""Button model for KY_HARD.DRV (resident scan routine at 0x0600, run from a kernel timer).

Scan algorithm (KY_HARD.DRV file 0x200.. = 0x0600.., verified by disassembly):
  phase 1: F0 &= 0x99 (bits 1,2,5,6 -> inputs), F1 |= 0x66, short delay,
           C = in(F2) & 0x66; a set bit means "pressed" (active high):
              bit1 -> code [0x070B]=0x04, bit2 -> [0x070A]=0x01,
              bit5 -> [0x070F]=0x0D, bit6 -> [0x070C]=0x05
  phase 2 (only if C == 0): F0 |= 0x40, F2 |= 0x40 (drive GPIOB6 high),
           B = in(F2) & 0x02 -> code [0x0709]=0x06
Key codes come from the 'SYSKEYSET' table at 0x0709 (06 01 04 05 03 02 0D ...).
Names CONFIRMED ON THE DEVICE (coordinator, 2026-11-05): MENU = 0x01 (select/enter),
LEFT (<<) = 0x04, RIGHT (>>) = 0x05, PLAY/PAUSE = 0x06, release = 0x30. In the main menu
RIGHT goes MUSICA->VIDEO, LEFT goes MUSICA->JUEGOS, MENU enters. Code 0x0D (GPIOB5) is in
the scan table but no physical key produces it ('K0D' here, for experiments only).
Key-up is posted as 0x30 (RST 08 A=0x10 = PutSysMsg).
"""

# name -> (phase, F2 bit)        firmware key code
KEYS = {
    'MENU':  (1, 0x04),    # 0x01  menu / select (GPIOB2)
    'LEFT':  (1, 0x02),    # 0x04  << (GPIOB1)
    'RIGHT': (1, 0x40),    # 0x05  >> (GPIOB6)
    'PLAY':  (2, 0x02),    # 0x06  play/pause (phase 2: B6 driven, B1 read)
    'K0D':   (1, 0x20),    # 0x0D  no physical key (GPIOB5)
}


class KeyModel:
    def __init__(self):
        self.down = set()
        self.release_at = {}

    def press(self, soc, key, ms=150):
        key = key.upper()
        if key not in KEYS:
            raise KeyError(key)
        self.down.add(key)
        if ms:
            self.release_at[key] = soc.ticks + ms * (soc.CPU_HZ // 1000)

    def hold(self, key):
        self.down.add(key.upper())

    def release(self, key):
        self.down.discard(key.upper())
        self.release_at.pop(key.upper(), None)

    def tick(self, soc):
        for k, t in list(self.release_at.items()):
            if soc.ticks >= t:
                self.down.discard(k)
                del self.release_at[k]

    def port_in(self, soc, p):
        v = soc.ports[p]
        if p != 0xf2:
            return v
        oe = soc.ports[0xf0]
        if oe & 0x40 and soc.ports[0xf2] & 0x40:          # phase 2
            v &= ~0x02
            if any(KEYS[k][0] == 2 for k in self.down):
                v |= 0x02
            return v
        mask = 0
        for k in self.down:
            ph, bit = KEYS[k]
            if ph == 1:
                mask |= bit
        return (v & ~0x66 & 0x7f) | mask   # bit7 (card/USB detect?) reads 0

    def port_out(self, soc, p, v):
        pass
