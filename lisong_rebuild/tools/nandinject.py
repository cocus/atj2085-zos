"""Put files into the user disk of an s1clone NAND dump - as an atj2085emu NAND overlay.

usage: nandinject.py DUMP OUT.ovl FILE|FOLDER [...]   (a FOLDER = one-level subfolder of the root)

Adds (or replaces) 8.3 files in the root directory of the FAT16 user disk, using only clusters
whose sectors are mapped by the FTL. Every changed UD sector is written into a copy of its
physical NAND page (column (s&3)*512, spare ECC bytes +8..+11 recomputed) and saved in the
emulator's overlay format (NANDOVL1), for `headless.py --nand DUMP --nand-overlay OUT.ovl`.
The dump itself is only read.
"""
import os, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'atj2085emu'))
from ftlread import Ftl, FW_UNITS, UD_FAT_BASE
from emu.nand import ecc512, RAW


class Disk:
    """FAT volume sectors (LBA 0 = UD 0x200) with copy-on-write"""
    def __init__(self, ftl):
        self.ftl, self.w = ftl, {}

    def loc(self, lba):
        S = UD_FAT_BASE + lba
        return self.ftl.sector_loc(FW_UNITS + (S >> 9), S & 511)

    def read(self, lba):
        if lba in self.w:
            return self.w[lba]
        return bytearray(self.ftl.read_ud(UD_FAT_BASE + lba))

    def write(self, lba, data):
        assert len(data) == 512 and self.loc(lba) is not None
        self.w[lba] = bytearray(data)


def name83(fn):
    b, _, e = os.path.basename(fn).upper().partition('.')
    assert 0 < len(b) <= 8 and len(e) <= 3, fn
    return (b.ljust(8) + e.ljust(3)).encode()


def main():
    dump, out, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    ftl = Ftl(dump, os.path.join(HERE, '..', 'work', 'nandcache'))
    d = Disk(ftl)
    bs = d.read(0)
    bps, spc, rsvd, nf, rootent, tot16, _, fsz = struct.unpack_from('<HBHBHHBH', bs, 11)
    assert bps == 512 and fsz, 'FAT16 expected'
    tot = tot16 or struct.unpack_from('<I', bs, 32)[0]
    root = rsvd + nf * fsz
    data0 = root + (rootent * 32 + 511) // 512
    nclus = (tot - data0) // spc
    fat = bytearray()
    for i in range(fsz):
        fat += d.read(rsvd + i)
    ent = lambda c: struct.unpack_from('<H', fat, 2 * c)[0]
    clus_ok = lambda c: all(d.loc(data0 + (c - 2) * spc + i) is not None for i in range(spc))
    rootsec = [d.read(root + i) for i in range((rootent * 32 + 511) // 512)]

    def free_chain(c):
        while 2 <= c < 0xFFF8:
            n = ent(c); struct.pack_into('<H', fat, 2 * c, 0); c = n

    nxt = [2]

    def alloc(data):
        """clusters for data (at least one), FAT chain written, data written; -> first cluster"""
        need = max(1, -(-len(data) // (spc * 512)))
        chain = []
        while len(chain) < need:
            assert nxt[0] < nclus + 2, 'disk full'
            if ent(nxt[0]) == 0 and clus_ok(nxt[0]):
                chain.append(nxt[0])
            nxt[0] += 1
        for i, c in enumerate(chain):
            struct.pack_into('<H', fat, 2 * c, chain[i + 1] if i + 1 < len(chain) else 0xFFFF)
            blob = data[i * spc * 512:(i + 1) * spc * 512].ljust(spc * 512, bytes(1))
            for k in range(spc):
                d.write(data0 + (c - 2) * spc + k, blob[k * 512:(k + 1) * 512])
        return chain[0]

    def dirent(nm, attr, clus, size):
        e = bytearray(32)
        e[0:11] = nm; e[11] = attr
        struct.pack_into('<HI', e, 26, clus, size)
        return e

    def root_slot(nm):
        slot = None
        for si, sec in enumerate(rootsec):                   # existing entry / free slot
            for o in range(0, 512, 32):
                if sec[o:o + 11] == nm:
                    free_chain(struct.unpack_from('<H', sec, o + 26)[0])
                    return (si, o)
                if slot is None and sec[o] in (0, 0xE5):
                    slot = (si, o)
        assert slot, 'root directory full'
        return slot

    for fn in files:
        nm = name83(fn.rstrip('/' + chr(92)))
        si, o = root_slot(nm)
        if os.path.isdir(fn):                                # one-level subfolder with its files
            ents = []
            subs = sorted(f for f in os.listdir(fn) if os.path.isfile(os.path.join(fn, f)))
            for f in subs:
                data = open(os.path.join(fn, f), 'rb').read()
                ents.append(dirent(name83(f), 0x20, alloc(data), len(data)))
            dc = alloc(b'')                                   # the directory's own cluster
            body = dirent(b'.          ', 0x10, dc, 0) + dirent(b'..         ', 0x10, 0, 0) + b''.join(ents)
            assert len(body) <= spc * 512, 'subfolder too big for one cluster'
            body = body.ljust(spc * 512, bytes(1))
            for k in range(spc):
                d.write(data0 + (dc - 2) * spc + k, body[k * 512:(k + 1) * 512])
            rootsec[si][o:o + 32] = dirent(nm, 0x10, dc, 0)
            print('%-12s <DIR> %d files, cluster %d' % (nm.decode(), len(subs), dc))
            continue
        data = open(fn, 'rb').read()
        rootsec[si][o:o + 32] = dirent(nm, 0x20, alloc(data), len(data))
        print('%-12s %7d bytes' % (nm.decode(), len(data)))
    for i, s in enumerate(rootsec):
        d.write(root + i, s)
    for f in range(nf):
        for i in range(fsz):
            d.write(rsvd + f * fsz + i, fat[i * 512:(i + 1) * 512])
    # changed sectors -> overlay pages
    pages = {}
    for lba, data in d.w.items():
        if data == bytearray(ftl.read_ud(UD_FAT_BASE + lba)):
            continue
        page, col = d.loc(lba)
        p = pages.setdefault(page, bytearray(ftl.c.read_raw(page)))
        p[col:col + 512] = data
        g = 0x800 + (col // 512) * 16
        p[g + 8:g + 12] = ecc512(bytes(data))
    with open(out, 'wb') as f:
        f.write(b'NANDOVL1' + struct.pack('<I', 0) + struct.pack('<I', len(pages)))
        for pg in sorted(pages):
            assert len(pages[pg]) == RAW
            f.write(struct.pack('<I', pg) + bytes(pages[pg]))
    print('%d sectors changed, %d pages -> %s' % (len(d.w), len(pages), out))


if __name__ == '__main__':
    main()
