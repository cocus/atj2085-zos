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

Optional, opt-in (README "Faster boot"): --fast-boot skips the per-file firmware sums on normal
boots (full checks stay whenever copy A and B differ or A's header/directory is bad), --fast-lcd
shortens the two waits before backlight-on, --fast = both, --keep-screen leaves the picture alone.

usage:
  brecpatch.py IN [-o OUT.BIN] [--fill RRGGBB] [--image FILE] [--keep-screen] [--fast-boot]
               [--fast-lcd] [--fast] [--preview P.png] [--fw-out OUT.fw]
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


# ------------------------------------------------------------------------------------- fast boot
# BREC's firmware check (BREC 0x0606 in BRECF641) verifies FWIMAGE copy A fully (magic, header u16
# sum, directory u32 sum, then the u32 sum of every non-.$$$/.RES file = ~half of the boot time),
# then copy B "light" (magic only) and compares A's and B's header ids. --fast-boot keeps all of that
# except the per-file sums on the NORMAL path: they are skipped only while a run-time flag is set,
# and the flag is cleared (= stock full checks) as soon as anything is off:
#   * A and B headers differ (first boot after a flash, interrupted update): A is re-verified FULLY
#     before the stock "erase settings + copy A -> B"; if A's files are bad, the stock "verify B
#     fully + copy B -> A" path runs instead (this extra check is not in the stock BREC: there A
#     was already fully verified at that point);
#   * A fails its header/directory check: B is verified fully (stock) before B -> A.
# Lost: a corrupted file BODY in copy A while A's header/dir are intact and equal to B's is no
# longer detected (stock: B is verified fully and copied over A); the player then runs the
# damaged file. The 0x55AA/sum of BREC itself and everything in the ROM are unchanged.
#
# verify entry:  xor a ; ld (LIGHT),a ; ld de,0 ; call VERIFY ; and a ; jr nz,A_BAD ; inc a ; ld (LIGHT),a
FB_MAIN = [0xAF, 0x32, None, None, 0x11, 0x00, 0x00, 0xCD, None, None, 0xA7, 0x20, None, 0x3C, 0x32, None, None]
# A bad:   ld de,0x4000 ; call VERIFY ; and a ; jp nz,DONE ; ld hl,0x4000 ; ld de,0 ; ld bc,0x4000 ;
#          call COPY ; jp MAIN
FB_ABAD = [0x11, 0x00, 0x40, 0xCD, None, None, 0xA7, 0xC2, None, None, 0x21, 0x00, 0x40, 0x11, 0x00, 0x00,
           0x01, 0x00, 0x40, 0xCD, None, None, 0xC3, None, None]
# A != B:  call ERASEVM ; ld hl,0 ; ld de,0x4000 ; ld bc,0x4000 ; call COPY ; jp MAIN
FB_DIFF = [0xCD, None, None, 0x21, 0x00, 0x00, 0x11, 0x00, 0x40, 0x01, 0x00, 0x40, 0xCD, None, None, 0xC3, None, None]
# per directory entry: pop hl ; call FILESUM ; and a ; jr nz,+6 ; pop bc ; djnz loop
FB_FILE = [0xE1, 0xCD, None, None, 0xA7, 0x20, 0x06, 0xC1, 0x10]
FILESUM_PROLOGUE = bytes([0xC5, 0xD5, 0xE5, 0xDD, 0xE5])          # push bc/de/hl/ix
STUB_LEN = 0x30
# LCD (paint routine end): ld c,200 ; call DELAY ; ld c,100 ; call DELAY ; in a,(EE) ; set 1,a ; out (EE),a
LCD_TAIL = [0x0E, None, 0xCD, None, None, 0x0E, None, 0xCD, None, None, 0xDB, 0xEE, 0xCB, 0xCF, 0xD3, 0xEE]


def w16(v):
    return [v & 0xFF, v >> 8]


def fast_stub(base, light, verify, filesum, erasevm, abad):
    flag = base
    s1, s3, s2 = base + 1, base + 10, base + 18
    code = [1]                                                     # flag (1 = skip file sums)
    code += [0x3A] + w16(flag) + [0xB7, 0xCA] + w16(filesum) + [0xAF, 0xC9]   # s1: per-file sum
    code += [0xAF, 0x32] + w16(flag) + [0x11, 0x00, 0x40, 0xC9]               # s3: A bad -> full
    code += [0xAF, 0x32] + w16(flag) + [0x32] + w16(light) + [0x11, 0x00, 0x00, 0xCD] + w16(verify) \
        + [0xA7, 0xCA] + w16(erasevm) + [0xE1, 0xC3] + w16(abad)              # s2: A != B
    assert len(code) <= STUB_LEN and s1 == base + 1 and len(code) > 18
    return bytes(code), s1, s2, s3


def find_fastboot(b, info):
    """-> dict with the patch sites and state 'stock' / 'patched'; BrecError if not recognised."""
    m, f, d = find(b, FB_MAIN), find(b, FB_FILE), find(b, FB_DIFF)
    if len(m) != 1 or len(f) != 1:
        raise BrecError('firmware-check entry / per-file sum call not found exactly once (%d, %d)' % (len(m), len(f)))
    x = m[0]
    light, verify = struct.unpack_from('<H', b, x + 2)[0], struct.unpack_from('<H', b, x + 8)[0]
    if struct.unpack_from('<H', b, x + 15)[0] != light:
        raise BrecError('firmware-check flag mismatch')
    abad = x + 13 + struct.unpack_from('b', b, x + 12)[0]
    callf = f[0] + 1
    target = struct.unpack_from('<H', b, callf + 1)[0]
    base = info['bitmap'] + BMP_LEN + 0x1F                         # free bytes after the bitmap
    diffs = [p for p in d if struct.unpack_from('<H', b, p + 16)[0] == x]
    if len(diffs) != 1:
        raise BrecError('A != B copy path not found exactly once')
    diff = diffs[0]
    st = dict(main=x, light=light, verify=verify, abad=abad, callf=callf, diff=diff, base=base)
    if target == base + 1:                                         # already patched by us
        filesum = struct.unpack_from('<H', b, base + 6)[0]
        erasevm = struct.unpack_from('<H', b, base + 18 + 15)[0]
        code = fast_stub(base, light, verify, filesum, erasevm, abad)[0]
        if b[base:base + len(code)] != code or b[abad] != 0xCD:
            raise BrecError('fast-boot patch present but damaged')
        st.update(state='patched', filesum=filesum, erasevm=erasevm)
        return st
    ab = b[abad:abad + len(FB_ABAD)]
    if not all(p is None or ab[k] == p for k, p in enumerate(FB_ABAD)) or \
            struct.unpack_from('<H', ab, 4)[0] != verify or struct.unpack_from('<H', ab, 23)[0] != x:
        raise BrecError('A-bad path at 0x%04X not recognised' % abad)
    if b[target:target + 5] != FILESUM_PROLOGUE:
        raise BrecError('per-file sum routine 0x%04X not recognised' % target)
    if not (base + STUB_LEN <= 0x3800 and b[base - 0x1F:base + STUB_LEN] == bytes(STUB_LEN + 0x1F)):
        raise BrecError('no free (zero) bytes after the bitmap for the fast-boot stub')
    st.update(state='stock', filesum=target, erasevm=struct.unpack_from('<H', b, diff + 1)[0])
    return st


def patch_fastboot(nb, st):
    if st['state'] == 'patched':
        return 0
    code, s1, s2, s3 = fast_stub(st['base'], st['light'], st['verify'], st['filesum'], st['erasevm'], st['abad'])
    nb[st['base']:st['base'] + len(code)] = code
    nb[st['callf']:st['callf'] + 3] = bytes([0xCD] + w16(s1))      # per-file sum -> s1
    nb[st['diff']:st['diff'] + 3] = bytes([0xCD] + w16(s2))        # A != B -> s2 (full A check first)
    nb[st['abad']:st['abad'] + 3] = bytes([0xCD] + w16(s3))        # A bad -> s3 (full B check)
    return 1


def find_fastlcd(b, info):
    hits = [p for p in find(b, LCD_TAIL) if info['code'] < p < info['code'] + 0x80]
    if len(hits) != 1:
        raise BrecError('backlight-on delay pair not found exactly once')
    p = hits[0]
    if struct.unpack_from('<H', b, p + 3) != struct.unpack_from('<H', b, p + 8):
        raise BrecError('backlight-on delays call different routines')
    return dict(at=p, waits=(b[p + 1], b[p + 6]))


def fastboot_sites(b, info):
    """bytes a --fast-boot / --fast-lcd patch may change (for --restore-check)"""
    s = set()
    try:
        st = find_fastboot(b, info)
        s |= set(range(st['base'], st['base'] + STUB_LEN))
        for k in ('callf', 'diff', 'abad'):
            s |= set(range(st[k], st[k] + 3))
    except BrecError:
        pass
    try:
        p = find_fastlcd(b, info)['at']
        s |= {p + 1, p + 6}
    except BrecError:
        pass
    return s


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
    try:
        st = find_fastboot(b, info)
        print('  firmware check at 0x%04X: per-file sums %s' % (
            st['main'], 'SKIPPED on the normal path (fast boot)' if st['state'] == 'patched' else 'stock (every boot)'))
    except BrecError as e:
        print('  firmware check: not recognised (%s): --fast-boot unavailable' % e)
    try:
        fl = find_fastlcd(b, info)
        print('  backlight-on waits at 0x%04X: %d + %d units%s' % (fl['at'], fl['waits'][0], fl['waits'][1],
              ' (stock)' if fl['waits'] == (200, 100) else ''))
    except BrecError as e:
        print('  backlight-on waits: not recognised (%s)' % e)
    return info


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input')
    ap.add_argument('-o', '--out')
    ap.add_argument('--fill', default='000000', help='background RRGGBB (default black)')
    ap.add_argument('--image', help='20x36 picture (PNG/BMP; other sizes are fitted)')
    ap.add_argument('--preview', help='write a preview PNG of the boot screen as seen on the 128x128 glass (x3)')
    ap.add_argument('--fw-out', help='also write a copy of the input .fw with the patched BREC part')
    ap.add_argument('--keep-screen', action='store_true', help='do not change fill colour / picture')
    ap.add_argument('--fast-boot', action='store_true',
                    help='skip the per-file firmware sums on normal boots (see README: what is lost)')
    ap.add_argument('--fast-lcd', action='store_true',
                    help='shorten the two waits before backlight-on (200+100 -> 1+1 units)')
    ap.add_argument('--fast', action='store_true', help='= --fast-boot --fast-lcd')
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
                | {SUM_OFF, SUM_OFF + 1} | fastboot_sites(o, oi)
            diff = [i for i in range(BREC_LEN) if b[i] != o[i]]
            bad = [i for i in diff if i not in allowed]
            if oi['bitmap'] != info['bitmap'] or oi['fill_at'] != info['fill_at'] or bad:
                print('DIFFERS outside fill/bitmap/fast-boot sites/sum: %d bytes, first at 0x%04X' % (len(bad), bad[0] if bad else 0))
                return 1
            print('differs only in fill colour / picture / fast-boot sites / sum (%d bytes): a patched copy of the original' % len(diff))
            return 0
        nb = bytearray(b)
        what = []
        if a.fast:
            a.fast_boot = a.fast_lcd = True
        if a.fast_boot:                                    # before the picture: checks the free bytes
            if patch_fastboot(nb, find_fastboot(b, info)):
                what.append('fast boot')
        if not a.keep_screen:
            bg = parse_colour(a.fill)
            px = load_image(a.image, bg) if a.image else emblem(bg)
            struct.pack_into('<H', nb, info['fill_at'], rgb565(*bg))   # ld de,nn: LE immediate
            nb[info['bitmap']:info['bitmap'] + BMP_LEN] = encode(px)
            what.append('fill 0x%04X + picture %s' % (rgb565(*bg), a.image or 'built-in ZOS emblem'))
        if a.fast_lcd:
            fl = find_fastlcd(b, info)
            nb[fl['at'] + 1] = 1
            nb[fl['at'] + 6] = 1
            what.append('backlight-on waits %d+%d -> 1+1' % fl['waits'])
        fix_sum(nb)
        nb = bytes(nb)
        ni = analyse(nb)                                   # re-verify the result
        assert ni['bitmap'] == info['bitmap'] and ni['fill_at'] == info['fill_at']
        if a.fast_boot:
            assert find_fastboot(nb, ni)['state'] == 'patched'
        if a.fast_lcd:
            assert find_fastlcd(nb, ni)['waits'] == (1, 1)
        assert nb[:8] == b[:8] and nb[0x200:0x215] == b[0x200:0x215]   # what zos FIRMWARE compares
        diff = [i for i in range(BREC_LEN) if nb[i] != b[i]]
        print('patched: %s; %d bytes changed, new sum %04X: verified' % (
            ', '.join(what) or 'nothing', len(diff), brec_sum(nb)))
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
