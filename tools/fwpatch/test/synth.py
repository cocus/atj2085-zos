"""Synthetic-disk tests of the zosboot selector's FAT/FTL logic in atj2085emu (development only).

usage: synth.py PATCHED.fw ZOS.AP [--zos-file NAME]
The emulator runs BREC's real loader and the stub; only BREC's sector transfer (0x3DC9) is
replaced by a model: a 2-zone F641 flash with a scrambled logical->physical table and a crafted
FAT16 volume (cluster size, fragmentation, directory contents chosen per case). The model decodes
the NAND address the stub built with BREC's real address routine (0x3F1B) and checks every
read is a valid table or data sector. Result per case: decision (go_zos/go_stock) and, for zos,
whether RAM 0x0800.. equals the ZOS.AP text.
"""
import os, sys, json, struct
HERE = os.path.dirname(os.path.abspath(__file__))
FWP = os.path.dirname(HERE)
TOP = os.path.abspath(os.path.join(FWP, '..', '..'))
sys.path.insert(0, os.path.join(TOP, 'atj2085emu'))
from emu.fwimage import FwImage            # noqa: E402
from emu.soc import ATJ2085                 # noqa: E402

UPZ, VOLX = 0x7B0, 0x1040000
TBL = {0: 0x050, 1: 0x049}                  # zone-relative table units, copy k = 1
MAP = lambda L: (L * 7 + 3) % UPZ           # logical -> physical (zone-relative), bijective
INV = {MAP(L): L for L in range(UPZ)}


class Vol:
    """sparse FAT16 volume: {sector: bytes}"""
    def __init__(self, spc, rootents=512, fatsz=256, fstype=b'FAT16   ', sig=True):
        self.spc, self.s = spc, {}
        self.rsvd, self.nf, self.fatsz = 4, 2, fatsz
        self.root = self.rsvd + self.nf * fatsz
        self.data = self.root + rootents * 32 // 512
        bs = bytearray(512)
        bs[0:3] = b'\xEB\x3C\x90'
        struct.pack_into('<HBHBHHBHHHII', bs, 11, 512, spc, self.rsvd, self.nf, rootents, 0, 0xF8, fatsz,
                         63, 255, 0, 2000000)
        bs[0x36:0x3E] = fstype
        if sig:
            bs[510:512] = b'\x55\xAA'
        self.s[0] = bs
        self.fat = {}
        self.dirents = []

    def chain(self, clusters, data, eoc=True):
        for i, c in enumerate(clusters):
            self.fat[c] = clusters[i + 1] if i + 1 < len(clusters) else (0xFFFF if eoc else 0)
            blob = data[i * self.spc * 512:(i + 1) * self.spc * 512]
            for k in range(self.spc):
                sec = blob[k * 512:(k + 1) * 512]
                if sec:
                    self.s[self.data + (c - 2) * self.spc + k] = sec.ljust(512, b'\0')

    def ent(self, name, attr=0x20, clus=0, size=0):
        e = bytearray(32)
        e[0:11] = name
        e[11] = attr
        struct.pack_into('<HI', e, 26, clus, size)
        self.dirents.append(bytes(e))

    def build(self):
        for c, v in self.fat.items():
            for f in range(self.nf):
                sec = self.rsvd + f * self.fatsz + (c >> 8)
                b = self.s.setdefault(sec, bytearray(512))
                struct.pack_into('<H', b, 2 * (c & 0xFF), v)
        raw = b''.join(self.dirents)
        for i in range(0, len(raw), 512):
            self.s[self.root + i // 512] = bytearray(raw[i:i + 512].ljust(512, b'\0'))

    def read(self, v):
        return bytes(self.s.get(v, bytes(512)))


def run_case(fw, vol, expect_text):
    log = []
    emu = ATJ2085(FwImage(fw), log=log.append)
    mem = emu.mem
    res = {'reads': 0, 'bad': []}

    def setup():                           # BREC's flash-detect / mount results (no NAND model)
        if bytes(mem[0x0603:0x060B]) == b'ZOSBOOT1':
            struct.pack_into('<H', mem, 0x3CAA, UPZ)
            mem[0x3CA9] = 8
            struct.pack_into('<HH', mem, 0x3C80, 0x1000 | TBL[0], 0x1000 | TBL[1])
            struct.pack_into('<H', mem, 0x008A, 4 * UPZ)
            mem[0x37] = 0x76
        return False
    syms = json.load(open(os.path.join(FWP, 'zosboot.sym')))
    emu.add_hle(syms['main'], setup, passthru=True)

    def xfer():
        m = emu.m
        de, hl, bc = m.de, m.hl, m.bc
        st = bytes(mem[de:de + 8])
        row = st[2] | st[3] << 8 | st[4] << 16
        unit = row >> 7
        sp = ((row >> 6) & 1) | ((row & 0x3F) << 1)
        s = sp * 4 + st[1] // 2
        dest = st[5] | st[6] << 8
        zone, pu = unit >> 11, unit & 0x7FF
        res['reads'] += 1
        if pu == TBL[zone]:                # table entry: sector 16 + 8*(L>>10) + ((L>>8)&3), byte 2*(L&0xFF)
            rel = s - 16
            L = ((((rel >> 3) << 2) | (rel & 3)) << 8) + hl // 2
            if rel < 0 or rel & 4 or bc != 2 or hl & 1:
                res['bad'].append('table read s=%d off=%d n=%d' % (s, hl, bc))
                data = b'\xff\xff'
            else:
                data = struct.pack('<H', MAP(L) if L < UPZ else 0xFFFF)
        else:
            if bc != 512 or hl:
                res['bad'].append('data read n=%d off=%d' % (bc, hl))
            L = INV.get(pu)
            U = L + zone * UPZ
            X = (U << 18) | (s << 9)
            v = (X - VOLX) // 512
            data = vol.read(v)[:bc]
        mem[dest:dest + len(data)] = data
        m.a = 1
        emu._ret()
        return True
    emu.add_hle(0x3DC9, xfer, name='xfer model')
    out = {}
    for n in ('go_zos', 'go_stock', 'reboot'):
        emu.add_hle(syms[n], (lambda n: lambda: (out.setdefault('d', n), False)[1])(n), passthru=True)
    while 'd' not in out and emu.ticks < 60_000_000:
        emu.run(50_000)
    d = out.get('d', 'timeout')
    img = None
    if d == 'go_zos':
        img = bytes(mem[0x0800:0x0800 + len(expect_text)]) == expect_text
    return d, img, res


def main():
    fw, zap = sys.argv[1], sys.argv[2]
    zname = b'ZOS     AP '
    if '--zos-file' in sys.argv:
        b, _, e = sys.argv[sys.argv.index('--zos-file') + 1].upper().partition('.')
        zname = (b.ljust(8) + e.ljust(3)).encode()
    ap = open(zap, 'rb').read()
    text = ap[0x800:0x800 + (ap[12] | ap[13] << 8)]
    nsec = -(-len(ap) // 512)
    lfn = bytes([0x41]) + 'zos'.encode('utf-16-le').ljust(10, b'\xff') + b'\x0f\x00\x00' + b'\xff' * 18
    cases = []

    def case(name, expect, mk):
        cases.append((name, expect, mk))

    def std(spc, clusters, pre=(), size=None, attr=0x20, eoc=True, data=None, name=zname, **kw):
        def mk():
            v = Vol(spc, **kw)
            v.ent(b'NO NAME    ', 0x08)
            for e in pre:
                v.dirents.append(e)
            v.chain(clusters, data or ap, eoc)
            v.ent(name, attr, clusters[0], len(ap) if size is None else size)
            v.build()
            return v
        return mk
    filler = [bytes([0xE5]) + b'OS     AP \x20' + bytes(20)] + [lfn] + \
             [b'FILE%04d   ' % i + b'\x20' + bytes(20) for i in range(20)]
    n1 = nsec
    frag = [900 - 37 * i if i % 2 else 50 + 3 * i for i in range(n1)]
    case('spc1_fragmented_2nd_root_sector', 'go_zos', std(1, frag, pre=filler))
    case('spc2_backwards', 'go_zos', std(2, [500 - i for i in range(-(-n1 // 2))]))
    case('spc16_zone0_hi_and_zone1', 'go_zos', std(16, [40000, 65000]))
    case('spc64_one_cluster', 'go_zos', std(64, [123]))
    case('chain_cut_short', 'go_stock', std(1, frag[:10]))
    case('chain_hits_free', 'go_stock', std(1, frag[:10], eoc=False))
    case('beyond_flash', 'go_stock', std(32, [65000]))
    case('fat12_volume', 'go_stock', std(1, frag, fstype=b'FAT12   '))
    case('no_boot_signature', 'go_stock', std(1, frag, sig=False))
    case('is_directory', 'go_stock', std(1, frag, attr=0x10))
    case('only_deleted_entry', 'go_stock', std(1, frag, name=b'\xe5OS     AP '))
    case('size_over_64k', 'go_stock', std(1, frag, size=70000))
    case('size_short_of_text', 'go_stock', std(1, frag, size=0x800 + len(text) - 1))
    case('cluster_0', 'go_stock', std(1, [0] + frag[1:]))
    ok = True
    for name, expect, mk in cases:
        d, img, res = run_case(fw, mk(), text)
        good = d == expect and (img is not False) and not res['bad'] and (d != 'go_zos' or img)
        ok &= good
        print('%-34s expect %-8s got %-8s image %-5s reads %3d %s%s' % (
            name, expect, d, {None: '-', True: 'ok', False: 'BAD'}[img], res['reads'], 'PASS' if good else 'FAIL',
            ('  ' + '; '.join(res['bad'][:3])) if res['bad'] else ''))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
