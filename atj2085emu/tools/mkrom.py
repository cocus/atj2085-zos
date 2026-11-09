"""Create rom/comlib.s28 (the mask-ROM C runtime library the emulator maps at 0x8800-0x8E77)
from YOUR player's boot ROM.

The ROM image is not distributed with the emulator. Every s1clone NAND dump carries the chip's
8 KB boot ROM (BROM) in its header (offset 0x18, read from 0x8000 with ports 01h = 02h = 0), so
anyone who made a backup of their player with s1clone already has it.

usage:  python tools/mkrom.py DUMP.s1clone        (or a raw 8 KB brom.bin)
        -> rom/comlib.s28  (S-records, addresses 0x8800-0x8E77)

rom/comlib.map (optional) only adds symbol names to traces; the emulator works without it.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'rom', 'comlib.s28')
LO, HI = 0x8800, 0x8E78          # runtime-library range used by the firmware


def read_brom(path):
    d = open(path, 'rb').read(0x18 + 0x4000)
    if d[:8] == b's1clone\0':
        return d[0x18:0x18 + 0x2000]
    if len(d) >= 0x2000:
        return d[:0x2000]
    sys.exit('not an s1clone dump or an 8 KB BROM image: ' + path)


def srec(typ, addr, data=b''):
    body = bytes([len(data) + 3, addr >> 8, addr & 0xFF]) + data
    return 'S%d%s%02X' % (typ, body.hex().upper(), ~sum(body) & 0xFF)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    brom = read_brom(sys.argv[1])
    lines = [srec(0, 0, b'brom')]
    for a in range(LO, HI, 16):
        lines.append(srec(1, a, brom[a - 0x8000:min(a + 16, HI) - 0x8000]))
    lines.append(srec(9, 0))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write('\n'.join(lines) + '\n')
    print('wrote', os.path.normpath(OUT), '(%d bytes of ROM)' % (HI - LO))


if __name__ == '__main__':
    main()
