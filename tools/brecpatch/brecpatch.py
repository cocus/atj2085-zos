#!/usr/bin/env python3
"""brecpatch - change the power-on screen that the ATJ2085 boot record (BREC) draws.

The boot record (16 KB, NAND blocks 0 and 1, `BRECF641.BIN` in the AFI .fw) initialises the LCD
first thing and paints a full-screen fill (stock: blue, RGB565 0x001F) plus a 20 x 36 RGB565
picture (stock: an hourglass) before it mounts the NAND and loads the OS. This tool:
  * finds the paint routine by its instruction pattern (not by a fixed address), checks the
    window geometry (20 x 36, 1440 bytes) and the bitmap location, and refuses anything else;
  * sets the fill colour (default black) and replaces the picture (default: a built-in "ZOS"
    emblem drawn in code, light on black; or --image FILE.png/.bmp, scaled to 20 x 36);
  * keeps 0x55AA at 0x3FFC and recomputes the boot ROM's 16-bit sum at 0x3FFE, then re-verifies;
  * writes a preview PNG of the boot screen as the user sees it (128 x 128 visible part of the\r
    132 x 132 GRAM, glass mounted rotated 180 degrees; the picture is stored rotated accordingly).

usage:
  brecpatch.py IN [-o OUT.BIN] [--fill RRGGBB] [--image FILE] [--preview P.png] [--fw-out OUT.fw]
  brecpatch.py IN --check                     report what is found (nothing written)
  brecpatch.py IN --restore-check ORIG        compare IN with ORIG: only fill/bitmap/sum may differ
IN / ORIG: a raw 16384-byte BREC, an AFI .fw (its BREC*.BIN part) or an s1clone NAND dump
(block 0 pages 0-7). --fw-out needs IN to be a .fw (the patched BREC replaces its BREC part).
NOTE: s1res / normal ADFU firmware updates do NOT write the boot record. Install the patched
BREC with the zos FIRMWARE module (BREC UPDATE) or a ROM-recovery tool (s1clone).

Pure Python 3 standard library; Pillow only if --image is a PNG (or a BMP this tool cannot read).
No stock bytes are embedded: the tool only recognises instruction patterns and patches the
image it is given.
"""
import argparse, os, struct, sys, zlib

BREC_LEN = 0x4000
SIG_OFF, SUM_OFF, SIG = 0x3FFC, 0x3FFE, 0x55AA
W, H = 20, 36
BMP_LEN = W * H * 2
LCD = 132

# fill loop: ld bc,0x4410 ; ld de,COLOUR ; 1: ld a,d ; ld (0x8000),a ; ld a,e ; ld (0x8000),a ;
#            dec bc ; ld a,b ; or c ; jr nz,1b                       (None = the colour bytes)
FILL_PAT = [0x01, 0x10, 0x44, 0x11, None, None, 0x7A, 0x32, 0x00, 0x80, 0x7B, 0x32, 0x00, 0x80,
            0x0B, 0x78, 0xB1, 0x20, 0xF3]
FILL_IMM = 4
# window + stream: ld c,0x15 ; call cmd ; ld c,X0 ; call dat ; ld c,X1 ; call dat ; ld c,0x75 ;
# call cmd ; ld c,Y0 ; call dat ; ld c,Y1 ; call dat ; ld c,0x5C ; call cmd ; call datmode ;
# ld hl,BITMAP ; ld de,0x8000 ; ld bc,LEN ; ldir                  (None = any byte, read below)
WIN_PAT = [0x0E, 0x15, 0xCD, None, None, 0x0E, None, 0xCD, None, None, 0x0E, None, 0xCD, None, None,
           0x0E, 0x75, 0xCD, None, None, 0x0E, None, 0xCD, None, None, 0x0E, None, 0xCD, None, None,
           0x0E, 0x5C, 0xCD, None, None, 0xCD, None, None,
           0x21, None, None, 0x11, 0x00, 0x80, 0x01, None, None, 0xED, 0xB0]
OFF_X0, OFF_X1, OFF_Y0, OFF_Y1, OFF_HL, OFF_BC = 6, 11, 21, 26, 39, 45


class BrecError(Exception):
    pass


# ------------------------------------------------------------------------------------- checks
def brec_sum(b):
    return sum(struct.unpack_from('<8191H', b, 0)) & 0xFFFF          # words 0x0000..0x3FFC


def sum_ok(b):
    return (len(b) == BREC_LEN and struct.unpack_from('<H', b, SIG_OFF)[0] == SIG
            and struct.unpack_from('<H', b, SUM_OFF)[0] == brec_sum(b))


def fix_sum(b):
    struct.pack_into('<H', b, SIG_OFF, SIG)
    struct.pack_into('<H', b, SUM_OFF, brec_sum(b))


def find(b, pat):
    hits = []
    n = len(pat)
    first = pat[0]
    i = b.find(bytes([first]))
    while i >= 0 and i + n <= len(b):
        if all(p is None or b[i + k] == p for k, p in enumerate(pat)):
            hits.append(i)
        i = b.find(bytes([first]), i + 1)
    return hits


def analyse(b):
    """-> dict(fill_at, colour, bitmap, win) or BrecError."""
    if len(b) != BREC_LEN:
        raise BrecError('size %d, want %d' % (len(b), BREC_LEN))
    if not sum_ok(b):
        raise BrecError('0x55AA / sum at 0x3FFC..0x3FFF wrong: not a valid boot record')
    f = find(b, FILL_PAT)
    w = find(b, WIN_PAT)
    if len(f) != 1:
        raise BrecError('fill loop pattern found %d times (want exactly 1)' % len(f))
    if len(w) != 1:
        raise BrecError('window/bitmap pattern found %d times (want exactly 1)' % len(w))
    fa, wa = f[0], w[0]
    if not 0 < wa - fa < 0x40:
        raise BrecError('fill loop (0x%04X) and bitmap stream (0x%04X) are not one routine' % (fa, wa))
    x0, x1, y0, y1 = b[wa + OFF_X0], b[wa + OFF_X1], b[wa + OFF_Y0], b[wa + OFF_Y1]
    bm = struct.unpack_from('<H', b, wa + OFF_HL)[0]
    ln = struct.unpack_from('<H', b, wa + OFF_BC)[0]
    if (x1 - x0 + 1, y1 - y0 + 1) != (W, H) or ln != BMP_LEN:
        raise BrecError('window %dx%d / %d bytes, want %dx%d / %d' % (x1 - x0 + 1, y1 - y0 + 1, ln, W, H, BMP_LEN))
    if x1 >= LCD or y1 >= LCD:
        raise BrecError('window outside the 132x132 LCD')
    if bm + BMP_LEN > SIG_OFF:
        raise BrecError('bitmap 0x%04X outside the image' % bm)
    if bm <= wa + len(WIN_PAT) and bm + BMP_LEN > fa:
        raise BrecError('bitmap 0x%04X overlaps the paint code' % bm)
    return dict(fill_at=fa + FILL_IMM, colour=struct.unpack_from('<H', b, fa + FILL_IMM)[0],
                bitmap=bm, win=(x0, x1, y0, y1), code=fa)


# ------------------------------------------------------------------------------------- inputs
def s1clone_brec(d, block=0):
    """Data bytes of pages 0-7 of a block from an s1clone dump (wiRe's container: header 16408
    bytes, then per page u16 chksum, u16 size, run-length-encoded 2048+64 bytes)."""
    page_raw = struct.unpack_from('<I', d, 0x14)[0]
    ppb = struct.unpack_from('<I', d, 0x10)[0]
    pos = 8 + 16 + 16384
    out = []
    want = range(block * ppb, block * ppb + 8)
    for p in range(block * ppb + 8):
        n = struct.unpack_from('<H', d, pos + 2)[0]
        src = d[pos + 4:pos + 4 + n]
        pos += 4 + n
        if p not in want:
            continue
        o = bytearray()
        i, last = 0, None
        while i < len(src):
            c = src[i]; i += 1
            o.append(c)
            if c == last:
                rep, sh = 0, 0
                while True:
                    v = src[i]; i += 1
                    rep |= (v & 0x7F) << sh
                    sh += 7
                    if not v & 0x80:
                        break
                o += bytes([c]) * rep
                last = None
            else:
                last = c
        if len(o) != page_raw:
            raise BrecError('s1clone page %d decodes to %d bytes' % (p, len(o)))
        out.append(bytes(o[:2048]))
    return b''.join(out)


def afi_brec_entry(d):
    for i in range(0x20, 0x200, 0x20):
        e = d[i:i + 0x20]
        if e[:4] == b'\0\0\0\0':
            break
        if e[:4] == b'BREC' and e[8:11] == b'BIN':
            off, ln = struct.unpack_from('<II', e, 0x10)
            return i, off, ln
    raise BrecError('no BREC*.BIN part in this AFI .fw')


def load(path):
    d = open(path, 'rb').read()
    if d[:4] == b'AFI\0':
        _, off, ln = afi_brec_entry(d)
        if ln != BREC_LEN:
            raise BrecError('AFI BREC part is %d bytes' % ln)
        return d[off:off + ln], 'AFI .fw'
    if d[:8] == b's1clone\0':
        b0 = s1clone_brec(d, 0)
        b1 = s1clone_brec(d, 1)
        return b0, 's1clone dump block 0 (block 1 copy %s)' % ('identical' if b0 == b1 else 'DIFFERENT')
    if len(d) == BREC_LEN:
        return d, 'raw BREC'
    raise BrecError('%s: not a 16 KB BREC, AFI .fw or s1clone dump' % path)


# ------------------------------------------------------------------------------------- pictures
def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def parse_colour(s):
    s = s.lstrip('#')
    if len(s) != 6:
        raise BrecError('colour %r: want RRGGBB' % s)
    v = int(s, 16)
    return v >> 16, v >> 8 & 0xFF, v & 0xFF


def emblem(bg):
    """Built-in 20x36 picture: the letters Z, O, S stacked, 2-px strokes, light blue-white fading
    to cyan downwards, on the fill colour, with a dotted 'please wait' rule under them."""
    px = [[bg] * W for _ in range(H)]

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            px[y][x] = c
    Z = ['############', '############', '        ### ', '       ###  ', '     ####   ', '   ####     ',
         '  ###       ', ' ###        ', '############', '############']
    O = [' ########## ', '############', '###      ###', '##        ##', '##        ##', '##        ##',
         '##        ##', '###      ###', '############', ' ########## ']
    S = [' ###########', '############', '###         ', '##########  ', ' ########## ', '  ##########',
         '         ###', '         ###', '############', '########### ']
    y = 1
    for k, g in enumerate((Z, O, S)):
        for r, row in enumerate(g):
            t = (y + r) / (H - 1)
            c = (int(235 - 90 * t), int(245 - 20 * t), 255)
            for i, ch in enumerate(row):
                if ch == '#':
                    put(4 + i, y + r, c)
        y += 11
    for x in range(3, 17, 3):                     # "busy" dots
        put(x, 34, (120, 200, 255))
        put(x + 1, 34, (120, 200, 255))
    return px


def read_bmp(d):
    if d[:2] != b'BM':
        raise BrecError('not a BMP')
    off = struct.unpack_from('<I', d, 10)[0]
    w, h, planes, bpp, comp = struct.unpack_from('<iiHHI', d, 18)
    if comp not in (0, 3) or bpp not in (24, 32):
        raise ValueError('BMP format')
    flip = h > 0
    h = abs(h)
    stride = (w * bpp // 8 + 3) & ~3
    px = []
    for y in range(h):
        row = d[off + (h - 1 - y if flip else y) * stride:]
        px.append([(row[x * bpp // 8 + 2], row[x * bpp // 8 + 1], row[x * bpp // 8]) for x in range(w)])
    return px


def load_image(path, bg):
    d = open(path, 'rb').read()
    px = None
    try:
        px = read_bmp(d)
    except (ValueError, BrecError, struct.error):
        pass
    if px is None or len(px) != H or len(px[0]) != W:
        try:
            from PIL import Image
        except ImportError:
            raise BrecError('%s: needs Pillow (pip install pillow), or give a 20x36 24/32-bit BMP' % path)
        im = Image.open(path).convert('RGBA')
        if im.size != (W, H):
            im.thumbnail((W, H), Image.LANCZOS)
            canvas = Image.new('RGBA', (W, H), bg + (255,))
            canvas.paste(im, ((W - im.size[0]) // 2, (H - im.size[1]) // 2), im)
            im = canvas
        else:
            under = Image.new('RGBA', (W, H), bg + (255,))
            im = Image.alpha_composite(under, im)
        px = [[im.getpixel((x, y))[:3] for x in range(W)] for y in range(H)]
    return px


def encode(px):
    """Picture as the user sees it -> bitmap bytes. The glass is mounted rotated 180 degrees and
    BREC's DATCTL scans GRAM straight (BC 00 ..), so the stored pixel order is reversed (the stock
    hourglass is stored upside down too)."""
    out = bytearray()
    for row in reversed(px):
        for c in reversed(row):
            out += struct.pack('>H', rgb565(*c))
    return bytes(out)


# ------------------------------------------------------------------------------------- preview
def png(path, rows):
    raw = b''.join(b'\0' + bytes(v for p in r for v in p) for r in rows)
    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    hdr = struct.pack('>IIBBBBB', len(rows[0]), len(rows), 8, 2, 0, 0, 0)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', hdr) + chunk(b'IDAT', zlib.compress(raw, 9))
                           + chunk(b'IEND', b''))


def preview(b, info, path, scale=3):
    def c565(v):
        r, g, bb = v >> 11, v >> 5 & 63, v & 31
        return (r << 3 | r >> 2, g << 2 | g >> 4, bb << 3 | bb >> 2)
    fill = c565(info['colour'])
    scr = [[fill] * LCD for _ in range(LCD)]
    x0, _, y0, _ = info['win']
    for i in range(W * H):
        v = b[info['bitmap'] + 2 * i] << 8 | b[info['bitmap'] + 2 * i + 1]
        scr[y0 + i // W][x0 + i % W] = c565(v)
    # what the user sees: GRAM rows 0..127, columns 4..131, rotated 180 degrees (atj2085emu lcd.py)
    scr = [row[4:][::-1] for row in scr[:128]][::-1]
    rows = []
    for r in scr:
        big = [p for p in r for _ in range(scale)]
        rows += [big] * scale
    png(path, rows)


# ------------------------------------------------------------------------------------- main
def report(b, src, name):
    info = analyse(b)
    x0, x1, y0, y1 = info['win']
    print('%s (%s)' % (name, src))
    print('  0x55AA + sum %04X: OK' % brec_sum(b))
    print('  paint routine at 0x%04X: fill colour RGB565 0x%04X at 0x%04X%s' % (
        info['code'], info['colour'], info['fill_at'], ' (stock blue)' if info['colour'] == 0x001F else ''))
    print('  picture: %dx%d at LCD cols 0x%02X-0x%02X rows 0x%02X-0x%02X, bitmap at 0x%04X-0x%04X' % (
        W, H, x0, x1, y0, y1, info['bitmap'], info['bitmap'] + BMP_LEN - 1))
    return info


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input')
    ap.add_argument('-o', '--out')
    ap.add_argument('--fill', default='000000', help='background RRGGBB (default black)')
    ap.add_argument('--image', help='20x36 picture (PNG/BMP; other sizes are fitted)')
    ap.add_argument('--preview', help='write a preview PNG of the boot screen as seen on the 128x128 glass (x3)')
    ap.add_argument('--fw-out', help='also write a copy of the input .fw with the patched BREC part')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--restore-check', metavar='ORIG')
    a = ap.parse_args(argv)
    try:
        b, src = load(a.input)
        info = report(b, src, a.input)
        if a.check:
            if a.preview:
                preview(b, info, a.preview)
                print('preview ' + a.preview)
            return 0
        if a.restore_check:
            o, osrc = load(a.restore_check)
            oi = report(o, osrc, a.restore_check)
            if b == o:
                print('IDENTICAL to the original')
                return 0
            allowed = set(range(info['fill_at'], info['fill_at'] + 2)) | set(range(info['bitmap'], info['bitmap'] + BMP_LEN)) \
                | {SUM_OFF, SUM_OFF + 1}
            diff = [i for i in range(BREC_LEN) if b[i] != o[i]]
            bad = [i for i in diff if i not in allowed]
            if oi['bitmap'] != info['bitmap'] or oi['fill_at'] != info['fill_at'] or bad:
                print('DIFFERS outside fill/bitmap/sum: %d bytes, first at 0x%04X' % (len(bad), bad[0] if bad else 0))
                return 1
            print('differs only in fill colour / picture / sum (%d bytes): a patched copy of the original' % len(diff))
            return 0
        bg = parse_colour(a.fill)
        px = load_image(a.image, bg) if a.image else emblem(bg)
        nb = bytearray(b)
        struct.pack_into('<H', nb, info['fill_at'], rgb565(*bg))   # ld de,nn: LE immediate
        nb[info['bitmap']:info['bitmap'] + BMP_LEN] = encode(px)
        fix_sum(nb)
        nb = bytes(nb)
        ni = analyse(nb)                                   # re-verify the result
        assert ni['bitmap'] == info['bitmap'] and ni['fill_at'] == info['fill_at']
        diff = [i for i in range(BREC_LEN) if nb[i] != b[i]]
        print('patched: fill 0x%04X, picture %s, %d bytes changed, new sum %04X: verified' % (
            ni['colour'], a.image or 'built-in ZOS emblem', len(diff), brec_sum(nb)))
        out = a.out or os.path.splitext(a.input)[0] + '-patched.BIN'
        open(out, 'wb').write(nb)
        print('wrote ' + out)
        if a.preview:
            preview(nb, ni, a.preview)
            print('preview ' + a.preview)
        if a.fw_out:
            d = bytearray(open(a.input, 'rb').read())
            if d[:4] != b'AFI\0':
                raise BrecError('--fw-out needs an AFI .fw as input')
            ent, off, ln = afi_brec_entry(d)
            d[off:off + ln] = nb
            cs = lambda x: sum(struct.unpack('<%dI' % (len(x) // 4), x)) & 0xFFFFFFFF
            struct.pack_into('<I', d, ent + 0x1C, cs(nb))
            struct.pack_into('<I', d, 0xFFC, cs(bytes(d[:0xFFC])))
            open(a.fw_out, 'wb').write(d)
            print('wrote %s (BREC part replaced; NOTE: s1res/normal ADFU does not write BREC)' % a.fw_out)
        return 0
    except (BrecError, OSError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
