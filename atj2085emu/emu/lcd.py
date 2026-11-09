"""Colour LCD controller model (Epson S1D15G00 / Solomon SSD1779 style command set).

Protocol as used by UI_6B33S.DRV (resident code, loaded at 0x4000):
  * select: port 0x02 (EM_PAGE_HI) := 0x18 (CE3), port 0x00 |= 0xC0 (wait states)  [drv 0x45DC]
  * RS line = GPIO C bit 2 (port 0xF4): 0 = command [drv 0x45D5], 1 = data [drv 0x45CE]
  * bus cycle = any memory access to 0x8000..0xFFFF while CE3 is selected
    (the driver also uses `ld de,#0x8000 ; ldir`, so A0..A14 are don't-care)
Commands seen in the driver: 0x15 CASET(x0,x1), 0x75 PASET(y0,y1), 0x5C RAMWR,
0x5D RAMRD, 0xBC DATCTL(p1,p2,p3). Everything else is logged and ignored.
Pixel format: 2 bytes per pixel (driver loop at 0x4568 writes 2 bytes/pixel);
interpreted as RGB565 big-endian, the same encoding as UI30.RES icons.
"""

W = H = 132   # S1D15G00 GRAM is 132x132
# Visible glass: 128x128. Derived from the driver: in modes with page-inverse
# (DATCTL P1 bit0) it adds +4 to page addresses (drv 0x44C8), so visible rows are
# GRAM rows 0..127 and visible columns 4..131; the glass is mounted rotated 180
# degrees (only then do bottom-up RES bitmaps drawn with P1=2 and top-down
# data drawn with P1=3 both come out upright). Bit meanings of P1 (bit0 page
# inverse, bit1 column inverse, bit2 page-first scan) are the S1D15G00 ones.
VIS_W = VIS_H = 128
VIS_X0, VIS_Y0 = 4, 0


class Lcd:
    def __init__(self, log=None):
        self.log = log or (lambda *a: None)
        self.fb = bytearray(W * H * 2)     # RGB565 BE, row-major, GRAM coordinates
        self.cmd = None
        self.params = []
        self.x0, self.x1, self.y0, self.y1 = 0, W - 1, 0, H - 1
        self.x = self.y = 0
        self.datctl = [0, 0, 0]
        self.byte_hi = None
        self.writing = False
        self.reading = False
        self.dirty = True
        self.cmd_hist = {}
        self.on = False
        self.pixels_written = 0

    # --- bus ---------------------------------------------------------------
    def write(self, rs, v):
        if not rs:
            self._command(v)
        else:
            self._data(v)

    def read(self, rs):
        if rs and self.reading:
            # return GRAM content (dummy first read is not modelled)
            i = (self.y * W + self.x) * 2
            if self.byte_hi is None:
                self.byte_hi = 1
                return self.fb[i] if i < len(self.fb) else 0
            self.byte_hi = None
            v = self.fb[i + 1] if i + 1 < len(self.fb) else 0
            self._advance()
            return v
        return 0

    def _command(self, c):
        self.cmd = c
        self.params = []
        self.byte_hi = None
        self.writing = c == 0x5C
        self.reading = c == 0x5D
        self.cmd_hist[c] = self.cmd_hist.get(c, 0) + 1
        if c == 0x5C or c == 0x5D:
            self.x, self.y = self.x0, self.y0
        elif c == 0xAF:
            self.on = True
        elif c == 0xAE:
            self.on = False
        elif c not in (0x15, 0x75, 0xBC):
            self.log('lcd cmd %02x' % c)

    def _data(self, v):
        c = self.cmd
        if c == 0x5C:
            if self.byte_hi is None:
                self.byte_hi = v
                return
            x, y = self.x, self.y
            p1 = self.datctl[0]
            if p1 & 2:          # column address inverse
                x = W - 1 - x
            if p1 & 1:          # page address inverse
                y = H - 1 - y
            if 0 <= x < W and 0 <= y < H:
                i = (y * W + x) * 2
                self.fb[i] = self.byte_hi
                self.fb[i + 1] = v
            self.byte_hi = None
            self.pixels_written += 1
            self.dirty = True
            self._advance()
            return
        self.params.append(v)
        if c == 0x15 and len(self.params) == 2:
            self.x0, self.x1 = self.params
        elif c == 0x75 and len(self.params) == 2:
            self.y0, self.y1 = self.params
        elif c == 0xBC and len(self.params) <= 3:
            self.datctl[len(self.params) - 1] = v

    def _advance(self):
        if self.datctl[0] & 4:      # page (y) direction first
            self.y += 1
            if self.y > self.y1:
                self.y = self.y0
                self.x += 1
                if self.x > self.x1:
                    self.x = self.x0
        else:
            self.x += 1
            if self.x > self.x1:
                self.x = self.x0
                self.y += 1
                if self.y > self.y1:
                    self.y = self.y0

    # --- export ------------------------------------------------------------
    def to_rgb(self, raw=False):
        """-> (w, h, bytes of 24-bit RGB) of what the user sees (128x128, rotated 180),
        or of the raw 132x132 GRAM when raw=True."""
        if raw:
            w, h = W, H
            coords = [((y * W) + x) * 2 for y in range(H) for x in range(W)]
        else:
            w, h = VIS_W, VIS_H
            coords = [(((VIS_Y0 + VIS_H - 1 - sy) * W) + (VIS_X0 + VIS_W - 1 - sx)) * 2
                      for sy in range(VIS_H) for sx in range(VIS_W)]
        out = bytearray(w * h * 3)
        fb = self.fb
        o = 0
        if True:
            for i in coords:
                v = (fb[i] << 8) | fb[i + 1]
                r = (v >> 11) & 0x1f
                g = (v >> 5) & 0x3f
                b = v & 0x1f
                out[o] = (r << 3) | (r >> 2)
                out[o + 1] = (g << 2) | (g >> 4)
                out[o + 2] = (b << 3) | (b >> 2)
                o += 3
        return w, h, bytes(out)
