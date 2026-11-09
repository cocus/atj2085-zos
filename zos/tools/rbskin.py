"""Rockbox theme -> zos skin (128x128 RGB565 LCD).

usage: rbskin.py THEME_DIR OUT_DIR [--preview DIR]
  THEME_DIR: a skin folder holding rockbox/ (the theme's .rockbox tree, e.g. zos/skins/ilike)
  OUT_DIR:   the SKIN folder copied to the player's disk root (SKIN\\)

Output files (8.3 names, read by modules/common/skin.c):
  COLORS.ZT   16-bit little-endian RGB565 words: bg, fg, dim, sel_top, sel_bottom, sel_text, bar
  *.ZI        image: 'Z' 'I' width height frames, then frames x height x width pixels,
              big-endian RGB565 = the LCD's own byte order (a draw is a plain copy to the LCD)
Images are scaled from the theme's 220x176 layout with an area-averaging filter.
No external libraries (BMP reader + PNG writer for previews are below).
"""
import os, sys, re, struct, zlib

SRC_W, SRC_H = 220, 176          # Rockbox target the theme was made for (Sansa Fuze)
DST = 128


# ---------------------------------------------------------------- BMP in, scaling, RGB565 out
def read_bmp(path):
    """-> (w, h, rows) with rows[y][x] = (r, g, b), top row first. 24/32-bit uncompressed."""
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h, _, bpp, comp = struct.unpack_from('<iiHHI', d, 18)
    if comp not in (0, 3) or bpp not in (24, 32):
        raise ValueError(f'{path}: unsupported BMP ({bpp} bpp, compression {comp})')
    flip = h > 0
    h = abs(h)
    stride = (w * bpp // 8 + 3) & ~3
    rows = []
    for y in range(h):
        o = off + (h - 1 - y if flip else y) * stride
        rows.append([(d[o + x * bpp // 8 + 2], d[o + x * bpp // 8 + 1], d[o + x * bpp // 8])
                     for x in range(w)])
    return w, h, rows


MAGENTA = (255, 0, 255)          # Rockbox transparent colour


def key(rows, col):
    """replace the transparent colour (before scaling, so it does not bleed)"""
    return [[col if p == MAGENTA else p for p in r] for r in rows]


def crop(rows, x, y, w, h):
    return [r[x:x + w] for r in rows[y:y + h]]


def scale(rows, nw, nh):
    """area-average downscale (or nearest upscale) to nw x nh"""
    h, w = len(rows), len(rows[0])
    out = []
    for j in range(nh):
        y0, y1 = j * h / nh, (j + 1) * h / nh
        line = []
        for i in range(nw):
            x0, x1 = i * w / nw, (i + 1) * w / nw
            acc = [0.0, 0.0, 0.0]
            tot = 0.0
            for y in range(int(y0), min(h, int(y1 + 0.9999)) or 1):
                wy = min(y + 1, y1) - max(y, y0)
                if wy <= 0:
                    continue
                for x in range(int(x0), min(w, int(x1 + 0.9999)) or 1):
                    wx = min(x + 1, x1) - max(x, x0)
                    if wx <= 0:
                        continue
                    p = rows[y][x]
                    for k in range(3):
                        acc[k] += p[k] * wx * wy
                    tot += wx * wy
            line.append(tuple(int(a / tot + 0.5) for a in acc) if tot else rows[min(h - 1, int(y0))][min(w - 1, int(x0))])
        out.append(line)
    return out


def rgb565(p):
    r, g, b = p
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def hexcol(s):
    v = int(s, 16)
    return (v >> 16) & 255, (v >> 8) & 255, v & 255


def zi(frames):
    """frames: list of rows-lists (same size) -> .ZI bytes"""
    h, w = len(frames[0]), len(frames[0][0])
    assert w <= 255 and h <= 255 and len(frames) <= 255
    b = bytearray(b'ZI' + bytes([w, h, len(frames)]))
    for f in frames:
        for row in f:
            for p in row:
                b += struct.pack('>H', rgb565(p))
    return bytes(b)


# ---------------------------------------------------------------- PNG preview (no PIL)
def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in rows)
    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


# ---------------------------------------------------------------- theme
def read_cfg(rb):
    cfgs = [f for f in os.listdir(os.path.join(rb, 'themes')) if f.endswith('.cfg')]
    cfg = {}
    for ln in open(os.path.join(rb, 'themes', cfgs[0]), errors='replace'):
        m = re.match(r'^([a-z ]+):\s*(.*?)\s*$', ln)
        if m:
            cfg[m.group(1)] = m.group(2)
    return cfg


def frames_of(path, n, under):
    w, h, rows = read_bmp(path)
    rows = key(rows, under)
    fh = h // n
    return [crop(rows, 0, i * fh, w, fh) for i in range(n)]


def sx(v):
    return max(1, round(v * DST / SRC_W))


def sy(v):
    return max(1, round(v * DST / SRC_H))


def main():
    theme, out = sys.argv[1], sys.argv[2]
    prev = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None
    rb = os.path.join(theme, 'rockbox')
    os.makedirs(out, exist_ok=True)
    if prev:
        os.makedirs(prev, exist_ok=True)
    cfg = read_cfg(rb)
    # zos overrides next to the theme (zos.cfg, same "key: value" lines), e.g. a darker background
    # for the low-contrast player LCD; "title color" replaces the backdrop's title strip
    zcfg = os.path.join(theme, 'zos.cfg')
    if os.path.exists(zcfg):
        for ln in open(zcfg, errors='replace'):
            m = re.match(r'^([a-z ]+):\s*(.*?)\s*$', ln)
            if m:
                cfg[m.group(1)] = m.group(2)
    bg =hexcol(cfg.get('background color', 'FFFFFF'))
    fg = hexcol(cfg.get('foreground color', '000000'))
    st = hexcol(cfg.get('line selector start color', '5C9AE7'))
    se = hexcol(cfg.get('line selector end color', '3268D2'))
    stx = hexcol(cfg.get('line selector text color', 'FFFFFF'))
    dim = tuple((a + b) // 2 for a, b in zip(fg, bg))
    # backdrop: full screen, and its title bar (top 22 of 176 rows) as a strip
    bpath = os.path.join(rb, cfg.get('backdrop', '/.rockbox/backdrops/Backdrop.bmp').replace('/.rockbox/', ''))
    _, _, brows = read_bmp(bpath)
    back = scale(brows, DST, DST)
    title_h = sy(22)
    bar = back[title_h // 2][DST // 2]
    title = back[:title_h]
    if 'title color' in cfg:                   # flat strip, slightly lighter at the top, dark edge below
        tc = hexcol(cfg['title color'])
        title = [[tuple(min(255, c + (title_h - 1 - y) * 3) for c in tc)] * DST for y in range(title_h - 1)]
        title.append([tuple(c // 2 for c in tc)] * DST)
        bar = tc
    colors = [bg, fg, dim, st, se, stx, bar]
    open(os.path.join(out, 'COLORS.ZT'), 'wb').write(b''.join(struct.pack('<H', rgb565(c)) for c in colors))
    outs = {'BACK': [back], 'TITLE': [title]}
    # selection bar: vertical gradient, one text row high (zos rows are 11 px)
    rowh = 11
    outs['SELBAR'] = [[[tuple(int(a + (b - a) * y / (rowh - 1)) for a, b in zip(st, se))] * DST for y in range(rowh)]]
    wps = os.path.join(rb, 'wps', 'iLike')
    def img(name, fn, n, w, h):
        p = os.path.join(wps, fn)
        if os.path.exists(p):
            outs[name] = [scale(f, w, h) for f in frames_of(p, n, bg)]
    img('PROGBAR', 'Progress Bar.bmp', 1, sx(196), max(4, sy(10)))
    img('VOLBAR', 'Volume Bar.bmp', 1, sx(166), max(4, sy(10)))
    img('BATT', 'Battery Icon.bmp', 25, sx(25) + 1, sy(12))
    img('PLAYMODE', 'Playmode Icon.bmp', 4, 8, 7)
    img('HOLD', 'Hold Icon.bmp', 1, 7, 8)
    icons = os.path.join(rb, 'icons', 'iLike.bmp')
    if os.path.exists(icons):
        w, h, rows = read_bmp(icons)           # Rockbox icon order: 0 audio, 1 folder, 2 playlist ...
        mid = tuple((a + b) // 2 for a, b in zip(st, se))
        for name, under in (('ICONS', bg), ('ICONSEL', mid)):
            r = key(rows, under)
            outs[name] = [scale(crop(r, 0, i * w, w, w), 9, 9) for i in range(h // w)]
    for name, frames in outs.items():
        open(os.path.join(out, name + '.ZI'), 'wb').write(zi(frames))
        if prev:
            strip = [r for f in frames for r in f]
            write_png(os.path.join(prev, name.lower() + '.png'), strip)
    #print(f"skin {os.path.basename(theme.rstrip('/\\\\'))}: {len(outs)} images + COLORS.ZT -> {out}")


if __name__ == '__main__':
    main()
