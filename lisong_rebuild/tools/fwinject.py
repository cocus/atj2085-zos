"""Put a firmware image into the firmware area of an s1clone NAND dump - as an atj2085emu NAND overlay.

usage: fwinject.py DUMP OUT.ovl FW [--copies A|AB] [--merge OVL ...]
  FW        AFI (.fw as s1res writes it) or a raw FWIMAGE.FW
  --copies  A  = only the primary copy at firmware byte 0 (what s1res writes; BREC then finds the
                 backup copy at byte 0x800000 different and re-copies A -> B on the first boot,
                 BREC 0x0606-0x069A), AB = both copies (the state after that first boot). Default AB.
  --merge   add the pages of other overlays (e.g. nandinject.py's user-disk files); pages of FW win

The firmware area = logical units 0..0x3F of zone 0 through the FTL mapping table (ftlread.py:
byte X -> unit X >> 18, sector (X >> 9) & 511). Only sectors whose content changes are written:
a copy of the physical page with the 512-byte column replaced and the spare ECC (+8..+11 of the
sector's 16-byte spare group) recomputed, exactly like nandinject.py. The dump is only read.
"""
import os, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'atj2085emu'))
from ftlread import Ftl, FW_UNITS        # noqa: E402
from emu.nand import ecc512, RAW         # noqa: E402

COPY_B = 0x800000                        # BREC 0x062F: second copy at firmware sector 0x4000


def fwimage(path):
    d = open(path, 'rb').read()
    if d[:4] == b'AFI\0':
        for i in range(0x20, 0x200, 0x20):
            e = d[i:i + 0x20]
            if e[:11] == b'FWIMAGE FW ':
                off, ln = struct.unpack_from('<II', e, 0x10)
                return d[off:off + ln]
        raise ValueError('no FWIMAGE.FW in ' + path)
    if d[:4] == b'\x55\xaa\xf0\x0f':
        return d
    raise ValueError('not an AFI / FWIMAGE: ' + path)


def read_overlay(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'NANDOVL1', path
    o = 8
    n, = struct.unpack_from('<I', d, o); o += 4
    erased = list(struct.unpack_from('<%dI' % n, d, o)); o += 4 * n
    n, = struct.unpack_from('<I', d, o); o += 4
    pages = {}
    for _ in range(n):
        p, = struct.unpack_from('<I', d, o); o += 4
        pages[p] = bytearray(d[o:o + RAW]); o += RAW
    return erased, pages


def main():
    a = sys.argv[1:]
    copies = 'AB'
    if '--copies' in a:
        i = a.index('--copies'); copies = a[i + 1].upper(); del a[i:i + 2]
    merges = []
    while '--merge' in a:
        i = a.index('--merge'); merges.append(a[i + 1]); del a[i:i + 2]
    dump, out, fwp = a
    ftl = Ftl(dump, os.path.join(HERE, '..', 'work', 'nandcache'))
    new = fwimage(fwp)
    assert len(new) <= COPY_B, 'image larger than one copy'
    erased, pages = [], {}
    for m in merges:
        e, p = read_overlay(m)
        erased += e
        pages.update(p)
        print('merged %s: %d pages, %d erased blocks' % (m, len(p), len(e)))
    fwpages = set()
    nsec = 0
    for base in ([0] + ([COPY_B] if copies == 'AB' else [])):
        for s in range(-(-len(new) // 512)):
            data = new[s * 512:(s + 1) * 512].ljust(512, b'\0')
            X = base + s * 512
            if ftl.read_fw(X, 512) == data:
                continue
            loc = ftl.sector_loc(X >> 18, (X >> 9) & 511)
            assert loc is not None, 'firmware unit %d not mapped' % (X >> 18)
            page, col = loc
            pg = pages.get(page)
            if pg is None or page not in fwpages:
                pg = pages[page] = bytearray(ftl.c.read_raw(page)) if pg is None else pg
            pg[col:col + 512] = data
            g = 0x800 + (col // 512) * 16
            pg[g + 8:g + 12] = ecc512(bytes(data))
            fwpages.add(page)
            nsec += 1
    with open(out, 'wb') as f:
        f.write(b'NANDOVL1' + struct.pack('<I', len(erased)) + b''.join(struct.pack('<I', b) for b in erased))
        f.write(struct.pack('<I', len(pages)))
        for pg in sorted(pages):
            assert len(pages[pg]) == RAW
            f.write(struct.pack('<I', pg) + bytes(pages[pg]))
    print('%s: FWIMAGE %d bytes, copies %s: %d sectors changed in %d pages -> %s (%d pages total)' % (
        os.path.basename(fwp), len(new), copies, nsec, len(fwpages), out, len(pages)))


if __name__ == '__main__':
    main()
