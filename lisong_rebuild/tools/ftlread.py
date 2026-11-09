"""ftlread.py - reference implementation of the stock ATJ2085 "641" FTL read path (LISONG LSD_501_04,
BRECF641 + UD_SF641.DRV) over an s1clone NAND dump. Spec: analysis/nand_ftl.md.

usage:
  python ftlread.py DUMP info                     geometry, zone tables, consistency checks
  python ftlread.py DUMP export-disk OUT.img      user FAT volume (UD sector 0x200 .. end) as raw image
  python ftlread.py DUMP export-fw OUT.bin [--compare FWIMAGE.FW|AFI.fw]
                                                  firmware region (logical units 0..0x3F = 16 MB)
  python ftlread.py DUMP export-vm OUT.bin        VM/settings area (UD sectors 0..511)
  python ftlread.py DUMP ls [PATH]                list a directory of the FAT volume (default: root)
  python ftlread.py DUMP tree [MAXDEPTH]          recursive listing
  python ftlread.py DUMP get PATH OUT             extract one file
  python ftlread.py DUMP fatcheck                 FAT consistency check (chains, cross-links, sizes)
  python ftlread.py DUMP ecc [NPAGES]             verify the spare-area ECC of the mapped sectors
options: --cache DIR (index cache, default ../work/nandcache)
         --overlay OVL  apply an atj2085emu NAND overlay (NANDOVL1: erased blocks + programmed
                        pages, emu.nand.save_overlay / nandinject.py) on top of the dump

Mapping (all evidence in nand_ftl.md):
  "unit" = 2 physical blocks (2 x 64 pages x 2 KB = 256 KB = 512 sectors), blocks 2n / 2n+1 of a zone
  global logical unit U: zone z = U // LUZ, L = U % LUZ (LUZ = 0x7B0 for K9K8G08: BREC 0x16FB)
  physical unit P = table_z[L] (zone-relative); zone z covers physical units z*2048 .. z*2048+2047
  sector s (0..511) of a unit: superpage sp = s >> 2, block = 2*(z*2048+P) + (sp & 1),
                               page = sp >> 1, column = (s & 3) * 512, spare bytes at 0x800 + (s&3)*16
  firmware byte offset X -> U = X >> 18 (BREC 0x3CAF); UD sector S -> U = 0x40 + (S >> 9) (UD_SF641 0x362C)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from s1clone import S1Clone, RAW  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# geometry by READ-ID byte 1 for Samsung-type chips (BREC 0x15A3..0x1768; values for D3 / EC D3 51 95)
GEOM = {
    # id1: (logical units per zone (0x3CAA), units per zone (0x3CA9 = 4 -> 1024, 8 -> 2048))
    0xD3: (0x7B0, 2048),
    0xDC: (0x7B0, 2048),
    0xD5: (0x7D0, 2048),
}
FW_UNITS = 0x40          # firmware/system area = logical units 0..0x3F (16 MB)
UD_FAT_BASE = 0x200      # FAT volume starts at UD sector 0x200 (SYSBNKB 0x4872)


def load_overlay(path):
    """atj2085emu overlay file -> (erased block set, {page: raw bytes})"""
    d = open(path, 'rb').read()
    if d[:8] != b'NANDOVL1':
        raise ValueError('not a NAND overlay file')
    o = 8
    n, = struct.unpack_from('<I', d, o); o += 4
    erased = set(struct.unpack_from('<%dI' % n, d, o)); o += 4 * n
    n, = struct.unpack_from('<I', d, o); o += 4
    pages = {}
    for _ in range(n):
        pg, = struct.unpack_from('<I', d, o); o += 4
        pages[pg] = bytes(d[o:o + RAW]); o += RAW
    return erased, pages


class Ftl:
    def __init__(self, dump_path, cache=None, overlay=None):
        self.c = S1Clone(dump_path, cache)
        if overlay:
            erased, pages = load_overlay(overlay)
            raw, ppb = self.c.read_raw, self.c.block_pages
            self.c.read_raw = lambda pg: pages[pg] if pg in pages else (
                b'\xff' * RAW if pg // ppb in erased else raw(pg))
        idb = self.c.id_bytes
        if idb[1] not in GEOM:
            raise ValueError('unsupported flash id %s' % idb.hex())
        self.luz, self.upz = GEOM[idb[1]]
        self.bpz = self.upz * 2                        # physical blocks per zone
        self.zones = self.c.blocks // self.bpz
        self.ppb = self.c.block_pages
        self.tables = [self._load_table(z) for z in range(self.zones)]
        self.units = self.luz * self.zones            # global logical units
        self._pcache = {}

    # ---- mapping table: block with spare byte 1 == 0xBB in the zone, newest 2-page copy
    def _find_table_block(self, z):
        found = []
        for b in range(z * self.bpz, (z + 1) * self.bpz):
            s = self.c.read_raw(b * self.ppb)[0x800:0x810]
            if s[0] == 0xFF and s[1] == 0xBB:
                found.append(b)
        return found

    def _load_table(self, z):
        blocks = self._find_table_block(z)
        if not blocks:
            raise ValueError('zone %d: no table block (spare byte1 = 0xBB)' % z)
        best = None
        for b in blocks:
            # copies are written as consecutive page pairs (pages 0-1, 2-3, ...); newest = last valid
            for p in range(0, self.ppb, 2):
                d0 = self.c.read_raw(b * self.ppb + p)
                d1 = self.c.read_raw(b * self.ppb + p + 1)
                if d0[0x800 + 1] != 0xBB:
                    break
                t = struct.unpack('<2048H', d0[:0x800] + d1[:0x800])
                if t[2047] == 0x55AA:
                    best = (b, p, t)
        if best is None:
            raise ValueError('zone %d: no valid table copy' % z)
        self.__dict__.setdefault('table_loc', {})[z] = best[:2]
        return best[2]

    def unit_phys(self, U):
        z, L = divmod(U, self.luz)
        P = self.tables[z][L]
        if P == 0xFFFF:
            return None
        return z, P

    def sector_loc(self, U, s):
        """-> (page, column) of sector s (0..511) of logical unit U, or None if unmapped."""
        r = self.unit_phys(U)
        if r is None:
            return None
        z, P = r
        sp = s >> 2
        blk = (z * self.upz + P) * 2 + (sp & 1)
        return blk * self.ppb + (sp >> 1), (s & 3) * 512

    def _page(self, page):
        d = self._pcache.get(page)
        if d is None:
            if len(self._pcache) > 256:
                self._pcache.clear()
            d = self.c.read_raw(page)
            self._pcache[page] = d
        return d

    def read_unit_sector(self, U, s):
        loc = self.sector_loc(U, s)
        if loc is None:
            return b'\xff' * 512
        page, col = loc
        return self._page(page)[col:col + 512]

    def read_ud(self, S):
        """UD (user disk driver) logical sector S."""
        return self.read_unit_sector(FW_UNITS + (S >> 9), S & 511)

    def ud_sectors(self):
        return (self.units - FW_UNITS) * 512

    def read_fw(self, off, n):
        out = bytearray()
        while n > 0:
            U, o = divmod(off, 0x40000)
            s, so = divmod(o, 512)
            d = self.read_unit_sector(U, s)[so:so + n]
            out += d
            off += len(d)
            n -= len(d)
        return bytes(out)


# ---------------------------------------------------------------------------------------- FAT
class Fat:
    def __init__(self, read_sector, nsect):
        self.rs = read_sector
        b = read_sector(0)
        self.part_off = 0
        if b[0] not in (0xEB, 0xE9) and b[510:512] == b'\x55\xaa':
            # MBR: first partition entry
            self.part_off = struct.unpack_from('<I', b, 0x1C6)[0]
            b = read_sector(self.part_off)
        self.bs = b
        self.bps, self.spc, self.rsv, self.nfat, self.nroot, ts16, self.media, fs16 = \
            struct.unpack_from('<HBHBHHBH', b, 11)
        ts32, = struct.unpack_from('<I', b, 32)
        self.total = ts16 or ts32
        if fs16:
            self.fsz = fs16
            self.root_clus = None
        else:
            self.fsz, = struct.unpack_from('<I', b, 36)
            self.root_clus, = struct.unpack_from('<I', b, 44)
        if self.bps != 512:
            raise ValueError('bytes/sector %d' % self.bps)
        self.fat0 = self.part_off + self.rsv
        self.root0 = self.fat0 + self.nfat * self.fsz
        self.rootsecs = (self.nroot * 32 + 511) // 512
        self.data0 = self.root0 + self.rootsecs
        self.nclus = (self.total - (self.data0 - self.part_off)) // self.spc
        self.type = 12 if self.nclus < 4085 else 16 if self.nclus < 65525 else 32
        self.label = b[43:54] if self.type != 32 else b[71:82]
        self._fat = None

    def fat(self):
        if self._fat is None:
            raw = b''.join(self.rs(self.fat0 + i) for i in range(self.fsz))
            if self.type == 16:
                self._fat = struct.unpack('<%dH' % (len(raw) // 2), raw)
            elif self.type == 32:
                self._fat = [x & 0x0FFFFFFF for x in struct.unpack('<%dI' % (len(raw) // 4), raw)]
            else:
                f = []
                for n in range(len(raw) * 2 // 3):
                    v = raw[n * 3 // 2] | raw[n * 3 // 2 + 1] << 8 if n * 3 // 2 + 1 < len(raw) else 0
                    f.append(v >> 4 if n & 1 else v & 0xFFF)
                self._fat = f
        return self._fat

    def eoc(self, c):
        return c >= {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.type]

    def chain(self, c):
        f = self.fat()
        out = []
        seen = set()
        while 2 <= c < len(f) and not self.eoc(c):
            if c in seen:
                raise ValueError('loop in chain at %d' % c)
            seen.add(c)
            out.append(c)
            c = f[c]
        return out

    def clus_sectors(self, c):
        s = self.data0 + (c - 2) * self.spc
        return range(s, s + self.spc)

    def read_chain(self, c, size=None):
        out = bytearray()
        for cl in self.chain(c):
            for s in self.clus_sectors(cl):
                out += self.rs(s)
            if size is not None and len(out) >= size:
                break
        return bytes(out[:size]) if size is not None else bytes(out)

    def dir_entries(self, clus):
        if clus is None or clus == 0:
            if self.type == 32:
                raw = self.read_chain(self.root_clus)
            else:
                raw = b''.join(self.rs(self.root0 + i) for i in range(self.rootsecs))
        else:
            raw = self.read_chain(clus)
        lfn = {}
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}
                continue
            attr = e[11]
            if attr == 0x0F:
                seq = e[0] & 0x1F
                part = e[1:11] + e[14:26] + e[28:32]
                lfn[seq] = part
                continue
            name = e[0:8].decode('latin1').rstrip()
            ext = e[8:11].decode('latin1').rstrip()
            short = name + ('.' + ext if ext else '')
            long = None
            if lfn:
                raw_l = b''.join(lfn[k] for k in sorted(lfn))
                long = raw_l.decode('utf-16le', 'replace').split('\x00')[0].rstrip('￿')
            lfn = {}
            if attr & 0x08:
                continue
            c = struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16 if self.type == 32 else 0)
            size, = struct.unpack_from('<I', e, 28)
            yield dict(short=short, name=long or short, attr=attr, clus=c, size=size)

    def lookup(self, path):
        clus = None
        ent = None
        for part in [p for p in path.replace('\\', '/').split('/') if p]:
            for e in self.dir_entries(clus):
                if e['name'].upper() == part.upper() or e['short'].upper() == part.upper():
                    ent = e
                    clus = e['clus']
                    break
            else:
                raise FileNotFoundError(path)
        return ent

    def walk(self, clus=None, path='', depth=0, maxdepth=99):
        for e in self.dir_entries(clus):
            if e['short'] in ('.', '..'):
                continue
            p = path + '/' + e['name']
            yield p, e
            if e['attr'] & 0x10 and depth < maxdepth:
                yield from self.walk(e['clus'], p, depth + 1, maxdepth)


def fatcheck(fat):
    f = fat.fat()
    used = {}
    problems = []
    nfiles = ndirs = 0
    total = 0
    for p, e in fat.walk():
        if e['attr'] & 0x10:
            ndirs += 1
        else:
            nfiles += 1
            total += e['size']
        if e['clus'] == 0:
            if e['size'] and not e['attr'] & 0x10:
                problems.append('%s: size %d but no cluster' % (p, e['size']))
            continue
        try:
            ch = fat.chain(e['clus'])
        except ValueError as x:
            problems.append('%s: %s' % (p, x))
            continue
        for c in ch:
            if c in used:
                problems.append('%s: cluster %d cross-linked with %s' % (p, c, used[c]))
            used[c] = p
        if not e['attr'] & 0x10:
            need = (e['size'] + fat.spc * 512 - 1) // (fat.spc * 512)
            if len(ch) != need:
                problems.append('%s: chain %d clusters, size needs %d' % (p, len(ch), need))
    lost = sum(1 for c in range(2, fat.nclus + 2) if c < len(f) and f[c] != 0 and c not in used)
    # FAT copies identical?
    copies_equal = all(fat.rs(fat.fat0 + i) == fat.rs(fat.fat0 + fat.fsz + i) for i in range(fat.fsz)) \
        if fat.nfat > 1 else True
    return dict(files=nfiles, dirs=ndirs, bytes=total, used_clusters=len(used), lost_clusters=lost,
                fat_copies_equal=copies_equal, problems=problems)


# ---------------------------------------------------------------------------------------- ECC
def ecc512(data):
    odd = 0
    par = 0
    for i, v in enumerate(data):
        for j in range(8):
            if v >> j & 1:
                odd ^= (i << 3) | j
                par ^= 1
    even = odd ^ (0xFFF if par else 0)
    return bytes(((odd >> 3) & 0xFF, ((odd & 7) << 5) | ((odd >> 11) & 7),
                  (even >> 3) & 0xFF, ((even & 7) << 5) | ((even >> 11) & 7) | 0x10))


def main():
    args = sys.argv[1:]
    cache = os.path.join(HERE, '..', 'work', 'nandcache')
    if '--cache' in args:
        i = args.index('--cache'); cache = args[i + 1]; del args[i:i + 2]
    ovl = None
    if '--overlay' in args:
        i = args.index('--overlay'); ovl = args[i + 1]; del args[i:i + 2]
    if len(args) < 2:
        print(__doc__); return 1
    ftl = Ftl(args[0], cache, ovl)
    cmd = args[1]
    ud = lambda s: ftl.read_ud(UD_FAT_BASE + s)
    if cmd == 'info':
        c = ftl.c
        print('flash id %s, %d pages x %d B, %d pages/block, %d blocks' % (
            c.id_bytes.hex(), c.pages, RAW, c.block_pages, c.blocks))
        print('zones %d x %d units (%d blocks); logical units/zone 0x%X; global logical units 0x%X' % (
            ftl.zones, ftl.upz, ftl.bpz, ftl.luz, ftl.units))
        for z in range(ftl.zones):
            b, p = ftl.table_loc[z]
            t = ftl.tables[z]
            print('zone %d: table block 0x%04X pages %d-%d; temp unit [0x7FD]=0x%04X, [0x7FE]=0x%04X, sig 0x%04X' % (
                z, b, p, p + 1, t[0x7FD], t[0x7FE], t[0x7FF]))
            free = [x for x in t[ftl.luz:0x7FD] if x != 0xFFFF]
            print('        free-unit list (entries 0x%X..0x7FC): %d units' % (ftl.luz, len(free)))
            # cross-check spare logical numbers of mapped units
            bad = 0
            for L in range(ftl.luz):
                P = t[L]
                if P == 0xFFFF:
                    continue
                s = c.read_raw((z * ftl.bpz + 2 * P) * ftl.ppb)[0x800:0x804]
                if s[0] == 0xFF and s[1] == 0xFF and (s[2] | s[3] << 8) not in (L, 0xFFFF):
                    bad += 1
            print('        spare logical number mismatches: %d' % bad)
        print('UD sectors: %d (%d MB); FAT volume = UD sector 0x200..' % (ftl.ud_sectors(), ftl.ud_sectors() // 2048))
        fat = Fat(ud, ftl.ud_sectors() - UD_FAT_BASE)
        print('FAT%d: label %r, %d sectors, %d sect/clus, %d FATs x %d sectors, root entries %d, clusters %d, part_off %d' % (
            fat.type, fat.label, fat.total, fat.spc, fat.nfat, fat.fsz, fat.nroot, fat.nclus, fat.part_off))
    elif cmd == 'export-disk':
        n = ftl.ud_sectors() - UD_FAT_BASE
        fat = Fat(ud, n)
        n = min(n, fat.part_off + fat.total)
        with open(args[2], 'wb') as f:
            for s in range(n):
                f.write(ud(s))
        print('wrote %d sectors (%d MB) to %s' % (n, n // 2048, args[2]))
    elif cmd == 'export-fw':
        d = ftl.read_fw(0, FW_UNITS * 0x40000)
        open(args[2], 'wb').write(d)
        print('wrote 16 MB firmware region to', args[2])
        if '--compare' in args:
            ref = open(args[args.index('--compare') + 1], 'rb').read()
            if ref[:4] == b'AFI\0':
                for i in range(0x20, 0x200, 32):
                    e = ref[i:i + 32]
                    if e[:7] == b'FWIMAGE':
                        off, ln = struct.unpack_from('<II', e, 0x10)
                        ref = ref[off:off + ln]
            diff = [i for i in range(len(ref)) if d[i] != ref[i]] if d[:len(ref)] != ref else []
            print('compare with reference FWIMAGE (%d bytes): %s' % (
                len(ref), 'IDENTICAL' if not diff else '%d bytes differ, first at 0x%X' % (len(diff), diff[0])))
    elif cmd == 'export-vm':
        open(args[2], 'wb').write(b''.join(ftl.read_ud(s) for s in range(512)))
        print('wrote VM area (UD sectors 0..511) to', args[2])
    elif cmd in ('ls', 'tree'):
        fat = Fat(ud, 0)
        if cmd == 'ls':
            clus = None
            if len(args) > 2:
                clus = fat.lookup(args[2])['clus']
            for e in fat.dir_entries(clus):
                print('%s %10d  %-12s %s' % ('D' if e['attr'] & 0x10 else '-', e['size'], e['short'], e['name']))
        else:
            md = int(args[2]) if len(args) > 2 else 99
            for p, e in fat.walk(maxdepth=md):
                print('%s %10d  %s' % ('D' if e['attr'] & 0x10 else '-', e['size'], p))
    elif cmd == 'get':
        fat = Fat(ud, 0)
        e = fat.lookup(args[2])
        open(args[3], 'wb').write(fat.read_chain(e['clus'], e['size']))
        print('wrote %d bytes to %s' % (e['size'], args[3]))
    elif cmd == 'fatcheck':
        fat = Fat(ud, 0)
        r = fatcheck(fat)
        for k, v in r.items():
            if k != 'problems':
                print('%s: %s' % (k, v))
        print('problems: %d' % len(r['problems']))
        for p in r['problems'][:50]:
            print('  ' + p)
    elif cmd == 'ecc':
        n = int(args[2]) if len(args) > 2 else 2000
        ok = bad = noecc = 0
        import random
        rnd = random.Random(1)
        for _ in range(n):
            U = rnd.randrange(ftl.units)
            s = rnd.randrange(512)
            loc = ftl.sector_loc(U, s)
            if loc is None:
                continue
            page, col = loc
            raw = ftl.c.read_raw(page)
            sp = raw[0x800 + (s & 3) * 16 + 8:0x800 + (s & 3) * 16 + 12]
            if sp == b'\xff' * 4:
                noecc += 1
            elif ecc512(raw[col:col + 512]) == sp:
                ok += 1
            else:
                bad += 1
        print('ECC: %d ok, %d mismatch, %d unprogrammed (FF)' % (ok, bad, noecc))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
