"""User disk ("NAND" user partition) backing store for the RST 18h storage HLE.

The firmware's UD_*.DRV exposes a flat space of 512-byte *logical* sectors (its FTL maps
them onto NAND pages). FS_FAT16/32.DRV reads/writes those sectors with RST 18h. This module
provides that logical sector space from a raw image file:

  * an existing raw FAT image (`--disk user.img`): used as is (writes go to the file);
  * a host folder (`--disk some_folder`): a FAT16 image is built in memory from the
    folder's files (8.3 names + VFAT long names), writes stay in memory unless
    `--disk-save` is given.

Layout of the logical space is decided in soc.py (see README "Storage model").
"""
import os
import struct

SECTOR = 512


class UserDisk:
    def __init__(self, data=None, path=None, writeback=False):
        self.path = path
        self.writeback = writeback
        if data is None:
            data = open(path, 'rb').read()
        self.data = bytearray(data)
        if len(self.data) % SECTOR:
            self.data += bytes(SECTOR - len(self.data) % SECTOR)
        self.dirty = False
        self.reads = 0
        self.writes = 0

    @property
    def sectors(self):
        return len(self.data) // SECTOR

    def read(self, sec):
        self.reads += 1
        o = sec * SECTOR
        if o + SECTOR > len(self.data):
            return b'\xff' * SECTOR
        return bytes(self.data[o:o + SECTOR])

    def write(self, sec, buf):
        self.writes += 1
        o = sec * SECTOR
        if o + SECTOR > len(self.data):
            return
        self.data[o:o + SECTOR] = buf[:SECTOR]
        self.dirty = True

    def save(self, path=None):
        path = path or self.path
        if path and self.dirty:
            with open(path, 'wb') as f:
                f.write(self.data)
            self.dirty = False

    @classmethod
    def open(cls, spec, size_mb=64, save_path=None):
        if os.path.isdir(spec):
            img = build_fat16(spec, size_mb=size_mb)
            d = cls(data=img, path=save_path)
            return d
        return cls(path=spec)


# ----------------------------------------------------------------------------- FAT16 builder
def _short_name(name, used):
    base, ext = os.path.splitext(name)
    ext = ext[1:]
    ok = lambda s: ''.join(c for c in s.upper() if c.isalnum() or c in '$%\'-_@~`!(){}^#&')
    b, e = ok(base), ok(ext)[:3]
    need_lfn = (b != base or e != ext or len(base) > 8 or len(ext) > 3)
    if not need_lfn and (b + '.' + e) not in used:
        sn = b.ljust(8) + e.ljust(3)
    else:
        need_lfn = True
        for i in range(1, 1000):
            t = '~%d' % i
            cand = b[:8 - len(t)] + t
            if cand + '.' + e not in used:
                b = cand
                break
        sn = b.ljust(8) + e.ljust(3)
    used.add(sn[:8].rstrip() + '.' + sn[8:].rstrip())
    return sn.encode('ascii'), need_lfn


def _lfn_entries(name, short):
    cs = 0
    for c in short:
        cs = (((cs & 1) << 7) + (cs >> 1) + c) & 0xff
    u = name.encode('utf-16-le') + b'\x00\x00'
    chars = [u[i:i + 2] for i in range(0, len(u), 2)]
    if len(name) % 13 == 0:
        chars = chars[:-1]
    while len(chars) % 13:
        chars.append(b'\xff\xff')
    n = len(chars) // 13
    ents = []
    for i in range(n):
        ch = chars[i * 13:(i + 1) * 13]
        seq = (i + 1) | (0x40 if i == n - 1 else 0)
        e = bytes([seq]) + b''.join(ch[0:5]) + bytes([0x0f, 0, cs]) + b''.join(ch[5:11]) + b'\0\0' + b''.join(ch[11:13])
        ents.append(e)
    return list(reversed(ents))


def build_fat16(folder, size_mb=64, label=b'XIMPRO     '):
    """Build a FAT16 image ("superfloppy": BPB in sector 0, no MBR) from a host folder."""
    total = size_mb * 1024 * 1024 // SECTOR
    spc = 4 if size_mb <= 128 else (8 if size_mb <= 256 else 16)   # sectors per cluster
    reserved, nfats, rootents = 1, 2, 512
    rootsecs = rootents * 32 // SECTOR
    # FAT size: solve iteratively
    fatsz = 1
    while True:
        data = total - reserved - nfats * fatsz - rootsecs
        clusters = data // spc
        need = (clusters + 2) * 2
        nf = (need + SECTOR - 1) // SECTOR
        if nf <= fatsz:
            break
        fatsz = nf
    img = bytearray(total * SECTOR)
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SECTOR, spc, reserved, nfats, rootents,
                     total if total < 0x10000 else 0, 0xf8, fatsz, 63, 255, 0,
                     total if total >= 0x10000 else 0)
    struct.pack_into('<BBBI', bs, 36, 0x80, 0, 0x29, 0x12345678)
    bs[43:54] = label
    bs[54:62] = b'FAT16   '
    bs[510:512] = b'\x55\xaa'
    img[0:SECTOR] = bs
    fat = [0xfff8, 0xffff]
    fat_off = reserved * SECTOR
    root_off = (reserved + nfats * fatsz) * SECTOR
    data_off = root_off + rootsecs * SECTOR
    csize = spc * SECTOR

    def alloc(nbytes):
        n = max(1, (nbytes + csize - 1) // csize)
        first = len(fat)
        for i in range(n):
            fat.append(first + i + 1 if i < n - 1 else 0xffff)
        if len(fat) > clusters + 2:
            raise ValueError('disk image too small for folder contents (use --disk-mb)')
        return first

    def cl_off(c):
        return data_off + (c - 2) * csize

    def dirent(short, attr, clus, size):
        return short + bytes([attr, 0, 0]) + struct.pack('<HHHHHHHI', 0, 0x4f21, 0x4f21, 0, 0, 0x4f21, clus, size)

    def write_dir(path, parent_clus, my_clus):
        ents = [dirent(label, 0x08, 0, 0)] if my_clus is None else [
            dirent(b'.          ', 0x10, my_clus, 0), dirent(b'..         ', 0x10, parent_clus or 0, 0)]
        used = set()
        subdirs = []
        for name in sorted(os.listdir(path), key=str.lower):
            full = os.path.join(path, name)
            short, lfn = _short_name(name, used)
            if lfn:
                ents.extend(_lfn_entries(name, short))
            if os.path.isdir(full):
                idx = len(ents)
                ents.append(None)
                subdirs.append((idx, short, full))
            else:
                blob = open(full, 'rb').read()
                c = alloc(len(blob)) if blob else 0
                if blob:
                    img[cl_off(c):cl_off(c) + len(blob)] = blob
                ents.append(dirent(short, 0x20, c, len(blob)))
        return ents, subdirs

    def place(ents, my_clus):
        raw = b''.join(ents)
        if my_clus is None:
            if len(raw) > rootsecs * SECTOR:
                raise ValueError('too many root entries')
            img[root_off:root_off + len(raw)] = raw
        else:
            img[cl_off(my_clus):cl_off(my_clus) + len(raw)] = raw

    def do_dir(path, parent, my_clus):
        ents, subdirs = write_dir(path, parent, my_clus)
        for idx, short, full in subdirs:
            n_entries = 2 + 2 * len(os.listdir(full)) + 8 * len(os.listdir(full))
            c = alloc(max(csize, n_entries * 32))
            ents[idx] = dirent(short, 0x10, c, 0)
            do_dir(full, my_clus, c)
        place(ents, my_clus)

    do_dir(folder, None, None)
    fatb = bytearray(fatsz * SECTOR)
    for i, v in enumerate(fat):
        struct.pack_into('<H', fatb, i * 2, v)
    for k in range(nfats):
        img[fat_off + k * fatsz * SECTOR:fat_off + (k + 1) * fatsz * SECTOR] = fatb
    return bytes(img)
