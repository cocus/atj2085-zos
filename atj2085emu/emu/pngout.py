"""Minimal PNG writer (stdlib only)."""
import struct
import zlib

from .lcd import W, H


def write_png(path, w, h, rgb, scale=1):
    if scale > 1:
        rows = []
        for y in range(h):
            row = rgb[y * w * 3:(y + 1) * w * 3]
            big = b''.join(row[x * 3:x * 3 + 3] * scale for x in range(w))
            rows.extend([big] * scale)
        w, h = w * scale, h * scale
        raw = b''.join(b'\0' + r for r in rows)
    else:
        raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def save_lcd(lcd, path, scale=3, raw=False):
    w, h, rgb = lcd.to_rgb(raw)
    write_png(path, w, h, rgb, scale)
