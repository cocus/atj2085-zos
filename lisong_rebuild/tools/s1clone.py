"""s1clone.py - read wiRe's s1clone NAND dump container (s1mp3_github/common/CloneImage.h).

Container format (little endian), established from CloneImage.h / CloneDevice.cpp (read path):
  header (16408 bytes):
    0x0000 u64  signature "s1clone\\0"
    0x0008 u32  flash_id      (READ ID bytes 0..3 as read from the chip, byte0 = maker)
    0x000C u32  flash_size    (number of pages)
    0x0010 u32  block_size    (pages per block)
    0x0014 u32  page_size     (0x840 = 2048 data + 64 spare)
    0x0018 u8   brom[16384]   (16 KB read from 0x8000 with ports 01h = 02h = 0: the chip's internal BROM)
  then one section per physical page, page 0..flash_size-1, every page present (no skipping):
    u16 chksum  (rotate-right-add checksum of the decoded 0x840 bytes)
    u16 size    (number of encoded bytes that follow)
    encoded bytes: copy bytes; whenever a byte equals the byte just before it, a varint
                   (7 bits per byte, LSB first, bit 7 = continue) follows = extra repeat count;
                   after a run the "last byte" is reset (so the next byte never pairs with the run)
  Each decoded page = 2048 data bytes + 64 spare bytes, exactly as returned by the chip.

The index (file offset of every section) is cached in a side file under the given cache dir
(never next to / inside the dump). The dump is opened read-only.
"""
import os, re, struct, sys

HDR_FMT = '<8sIIII'
HDR_SIZE = 8 + 16 + 16384
RAW = 0x840
_pair = re.compile(rb'(.)\1', re.S)


def checksum(b):
    cs = 0
    for x in b:
        cs = ((0x8000 if cs & 1 else 0) + (cs >> 1) + x) & 0xFFFF
    return cs


def decode(src, want=RAW):
    """Decode one section (bytes) -> bytes."""
    out = bytearray()
    pos = 0
    n = len(src)
    while pos < n:
        m = _pair.search(src, pos)
        if m is None:
            out += src[pos:]
            break
        s = m.start()
        out += src[pos:s + 2]          # up to and including the pair
        b = src[s]
        p = s + 2
        rep = 0
        shift = 0
        while p < n:
            c = src[p]; p += 1
            rep |= (c & 0x7F) << shift
            shift += 7
            if not c & 0x80:
                break
        out += bytes([b]) * rep
        pos = p
    return bytes(out)


class S1Clone:
    def __init__(self, path, cache_dir=None):
        self.path = path
        self.f = open(path, 'rb')
        h = self.f.read(HDR_SIZE)
        sig, self.flash_id, self.pages, self.block_pages, self.page_size = struct.unpack_from(HDR_FMT, h)
        if sig != b's1clone\0':
            raise ValueError('not an s1clone file')
        if self.page_size != RAW:
            raise ValueError('page size %#x' % self.page_size)
        self.brom = h[24:24 + 16384]
        self.id_bytes = struct.pack('<I', self.flash_id)
        self.blocks = self.pages // self.block_pages
        self.offs = self._index(cache_dir)

    def _index(self, cache_dir):
        st = os.stat(self.path)
        cache = None
        if cache_dir:
            os.makedirs(cache_dir, exist_ok=True)
            cache = os.path.join(cache_dir, '%s.%d.idx' % (os.path.basename(self.path), st.st_size))
            if os.path.exists(cache):
                import array
                a = array.array('Q')
                with open(cache, 'rb') as f:
                    a.frombytes(f.read())
                if len(a) == self.pages:
                    return a
        import array
        a = array.array('Q')
        self.f.seek(HDR_SIZE)
        pos = HDR_SIZE
        buf = b''
        bpos = 0
        CH = 1 << 24
        for _ in range(self.pages):
            if bpos + 4 > len(buf):
                self.f.seek(pos)
                buf = self.f.read(CH); bpos = 0
            size = buf[bpos + 2] | buf[bpos + 3] << 8
            a.append(pos)
            pos += 4 + size
            bpos += 4 + size
            if bpos > len(buf):
                buf = b''; bpos = 0
        if pos != st.st_size:
            print('s1clone: warning: index ends at %d, file size %d' % (pos, st.st_size), file=sys.stderr)
        if cache:
            with open(cache, 'wb') as f:
                f.write(a.tobytes())
        return a

    def read_raw(self, page, verify=False):
        """Return the 0x840-byte raw page (data + spare)."""
        self.f.seek(self.offs[page])
        cs, size = struct.unpack('<HH', self.f.read(4))
        d = decode(self.f.read(size))
        if len(d) != RAW:
            raise ValueError('page %d decodes to %d bytes' % (page, len(d)))
        if verify and checksum(d) != cs:
            raise ValueError('page %d checksum mismatch' % page)
        return d

    def read_spare(self, page):
        return self.read_raw(page)[0x800:]


if __name__ == '__main__':
    c = S1Clone(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    print('id', c.id_bytes.hex(), 'pages', c.pages, 'ppb', c.block_pages, 'blocks', c.blocks)
    for p in (0, 1, 64, 128):
        d = c.read_raw(p, verify=True)
        print(p, d[:16].hex(), d[0x800:0x840].hex())
