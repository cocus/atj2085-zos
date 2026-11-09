"""ACTOS .RES resource files (UI30.RES, LOGO.RES, END.RES).

Format (from wiRe's s1res, s1mp3_github/common/s1def.h):
  head  : u32 id='RES\\x19', u16 num_entries, 5 x u16 unknown          (16 bytes)
  entry : u32 fofs, u16 size, u8 type, char name[9]                      (16 bytes each)
  type 1 ICON   : u16 w, u16 h, pixels. colour ("OLED") = w*h RGB565 big-endian,
                  rows stored bottom-up; mono = w*((h+7)/8) bytes, column bytes, MSB=top
       2 STRING, 3 MSTRING (one string per language), 4 LOGO (mono 128x64 / 128x32)
  colour icons may exceed 65535 bytes: real size = w*h*2+4 (u16 size field truncates)

usage: restool.py dump UI30.RES outdir      -> index.json + PNG per image + .txt strings

"""
import struct, sys, os, json, zlib

ID = 0x19534552


def png(path, w, h, rgb_rows):
    raw = b''.join(b'\0' + bytes(r) for r in rgb_rows)
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def parse(d):
    rid, n = struct.unpack_from('<IH', d, 0)
    assert rid == ID, 'not a RES file'
    ents = []
    for i in range(n):
        fofs, size, typ = struct.unpack_from('<IHB', d, 16 + 16 * i)
        name = d[16 + 16 * i + 7: 16 + 16 * i + 16].split(b'\0')[0].decode('latin1')
        ents.append(dict(idx=i, name=name, type=typ, fofs=fofs, size=size))
    return ents


def icon_info(d, e):
    w, h = struct.unpack_from('<HH', d, e['fofs'])
    oled = w * h * 2 + 4
    mono = w * ((h + 7) // 8) + 4
    if oled > 0xFFFF and mono > e['size']:
        return w, h, 'rgb565', oled
    if oled <= e['size']:
        return w, h, 'rgb565', oled
    return w, h, 'mono', mono


def render(d, off, w, h, fmt):
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            if fmt == 'rgb565':
                o = off + 2 * (x + (h - 1 - y) * w)
                c = (d[o] << 8) | d[o + 1] if o + 1 < len(d) else 0
                r += bytes(((c >> 11) << 3 & 0xff, (c >> 5) << 2 & 0xff, (c << 3) & 0xff))
            else:
                o = off + x + (y // 8) * w
                bit = d[o] & (0x80 >> (y % 8)) if o < len(d) else 0
                r += b'\0\0\0' if bit else b'\xff\xff\xff'
        rows.append(r)
    return rows


def dump(path, od):
    d = open(path, 'rb').read()
    os.makedirs(od, exist_ok=True)
    ents = parse(d)
    for e in ents:
        if not e['name']:
            continue
        safe = f"{e['idx']:04d}_{e['name']}"
        try:
            if e['type'] == 1:
                w, h, fmt, real = icon_info(d, e)
                e.update(w=w, h=h, fmt=fmt, real_size=real)
                if 0 < w <= 2048 and 0 < h <= 2048:
                    png(os.path.join(od, safe + '.png'), w, h, render(d, e['fofs'] + 4, w, h, fmt))
            elif e['type'] == 4:
                h = 64 if e['size'] >= 128 * 64 // 8 else 32
                png(os.path.join(od, safe + '.png'), 128, h, render(d, e['fofs'], 128, h, 'mono'))
            elif e['type'] in (2, 3):
                blob = d[e['fofs']:e['fofs'] + e['size']]
                open(os.path.join(od, safe + '.txt'), 'w', encoding='utf-8').write(
                    '\n'.join(s.decode('gbk', 'replace') for s in blob.split(b'\0') if s))
        except Exception as ex:
            e['error'] = str(ex)
    json.dump(dict(file=os.path.basename(path), size=len(d), entries=ents), open(os.path.join(od, 'index.json'), 'w'), indent=0)
    from collections import Counter
    print(os.path.basename(path), len(ents), 'entries', dict(Counter(e['type'] for e in ents)))


if __name__ == '__main__':
    if sys.argv[1] == 'dump':
        dump(sys.argv[2], sys.argv[3])
