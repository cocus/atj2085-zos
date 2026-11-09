;; LCD pixel streaming (SSD1789A-compatible controller on CE3, memory 0x8000, A0 = port F4 bit 2).
;; zos keeps the bus selected (port 02 = 0x18) for the whole session; A0 is left at 1 (data)
;; after every command, so pixel/parameter bytes are plain stores to 0x8000.
	.module lcd
	.include "regs.inc"
	.area	_CODE

;; void lcd_cmd(uint8_t c)       A = command
_lcd_cmd::
	ld	l, a
	in	a, (P_GPC_D)
	res	2, a
	out	(P_GPC_D), a
	ld	a, l
	ld	(LCD_DATA), a
	in	a, (P_GPC_D)
	set	2, a
	out	(P_GPC_D), a
	ret

;; void hal_lcd_fill(uint16_t color, uint16_t count)    HL = colour, DE = count
_hal_lcd_fill::
	ld	b, h
	ld	c, l
	ld	hl, #LCD_DATA
	ld	a, e				; remainder count & 7
	and	a, #7
	jr	z, 2$
1$:	ld	(hl), b
	ld	(hl), c
	dec	a
	jr	nz, 1$
2$:	srl	d				; DE /= 8
	rr	e
	srl	d
	rr	e
	srl	d
	rr	e
	ld	a, d
	or	a, e
	ret	z
3$:	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	ld	(hl), b
	ld	(hl), c
	dec	de
	ld	a, d
	or	a, e
	jr	nz, 3$
	ret

;; void hal_lcd_pixel(uint16_t color)                  HL = colour
_hal_lcd_pixel::
	ld	a, h
	ld	(LCD_DATA), a
	ld	a, l
	ld	(LCD_DATA), a
	ret

;; void hal_lcd_blit(const uint8_t *src, uint16_t count)   HL = big-endian RGB565, DE = pixels
_hal_lcd_blit::
	ld	a, d
	or	a, e
	ret	z
	ld	bc, #LCD_DATA
1$:	ld	a, (hl)
	ld	(bc), a
	inc	hl
	ld	a, (hl)
	ld	(bc), a
	inc	hl
	dec	de
	ld	a, d
	or	a, e
	jr	nz, 1$
	ret

;; ---- text: colours set once, then 6x8 glyphs streamed straight from the 5x7 font -------------
	.area	_DATA
fgc:	.ds	2
bgc:	.ds	2
	.area	_CODE

;; void hal_lcd_colors(uint16_t fg, uint16_t bg)        HL = fg, DE = bg
_hal_lcd_colors::
	ld	(fgc), hl
	ld	(bgc), de
	ret

;; void hal_lcd_glyph6x8(const uint8_t *cols)          HL = 5 column bytes (LSB = top row)
;; streams 48 pixels row by row into the current 6x8 window
_hal_lcd_glyph6x8::
	ld	c, #1				; row mask
	ld	de, #LCD_DATA
1$:	push	hl
	ld	b, #5
2$:	ld	a, (hl)
	inc	hl
	and	a, c
	jr	z, 3$
	ld	a, (fgc + 1)
	ld	(de), a
	ld	a, (fgc)
	ld	(de), a
	djnz	2$
	jr	4$
3$:	ld	a, (bgc + 1)
	ld	(de), a
	ld	a, (bgc)
	ld	(de), a
	djnz	2$
4$:	ld	a, (bgc + 1)			; 6th column = spacing
	ld	(de), a
	ld	a, (bgc)
	ld	(de), a
	pop	hl
	sla	c
	jr	nc, 1$
	ret
