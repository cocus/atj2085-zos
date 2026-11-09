# Launcher icons

SHELL draws one 16x16 monochrome icon per carousel item and per module in the GAMES / TOOLS lists
(`icon16()` in `modules/common/draw.c`, scaled x2 in the carousel, in the skin's colours).

- One file per icon: `icons/<NAME>.bmp`, exactly **16 x 16 pixels**.
  `NAME` is a module name (`SNAKE.bmp` for `SNAKE.ZM`, 8 characters max) or a SHELL category
  (`MUSIC`, `FM`, `GAMES`, `TOOLS`, `USB`, `SETTINGS`, `REBOOT`). Modules without their own icon
  get `DEFAULT.bmp`.
- Format: Windows BMP, **uncompressed**, 1, 4 or 8-bit (palette), 24 or 32-bit; bottom-up or
  top-down. What Paint, GIMP, Paint.NET, ... save by default works; RLE compression does not.
- Which pixels are drawn: the **dark** ones. A pixel is set when `R + G + B < 384` (average below
  50% grey). Draw in black (or any dark colour) on a white background; white, light colours and
  magenta `#FF00FF` (the Rockbox "transparent" colour) are background. The colours themselves are
  not kept: the shell paints the set pixels in the skin's text colour.
- The files in this folder are 24-bit, black on white.

`python build.py` turns them into `out/atj2085/icons.h` (`gen_icons()`: 32 bytes per icon, one bit
per pixel, row by row, MSB = leftmost pixel), compiled into SHELL.ZM. A new or changed icon needs a
rebuild (and a deploy of SHELL.ZM), nothing else.
