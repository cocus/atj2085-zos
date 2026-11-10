;; newbrec target ximpro: LCD bus helpers. The panel sits on chip select 3 (port 02 = 0x18, the window
;; 0x8000 is then the LCD bus); A0 (command / data) is GPIO C bit 2 (port F4). Own code.
	.module lcdraw
	.include "regs.inc"
	.area	_DATA
fgc:	.ds	2
bgc:	.ds	2
	.area	_CODE

;; void lcd_cmd(uint8_t c)       A = command byte (A0 low for the write, high afterwards)
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

;; void lcd_data(uint8_t d)      A = data byte
_lcd_data::
	ld	(LCD_DATA), a
	ret

;; void lcd_fill(uint16_t color, uint16_t count)     HL = colour, DE = pixel count
_lcd_fill::
	ld	b, h
	ld	c, l
	ld	hl, #LCD_DATA
	ld	a, d
	or	a, e
	ret	z
1$:	ld	(hl), b
	ld	(hl), c
	dec	de
	ld	a, d
	or	a, e
	jr	nz, 1$
	ret

;; void lcd_colors(uint16_t fg, uint16_t bg)       HL = fg, DE = bg
_lcd_colors::
	ld	(fgc), hl
	ld	(bgc), de
	ret

;; void lcd_glyph(const uint8_t *cols)    HL = 5 column bytes (LSB = top): 6x8 pixels row by row
_lcd_glyph::
	ld	c, #1
	ld	de, #LCD_DATA
1$:	push	hl
	ld	b, #5
2$:	ld	a, (hl)
	inc	hl
	and	a, c
	jr	z, 3$
	ld	a, (fgc+1)
	ld	(de), a
	ld	a, (fgc)
	ld	(de), a
	djnz	2$
	jr	4$
3$:	ld	a, (bgc+1)
	ld	(de), a
	ld	a, (bgc)
	ld	(de), a
	djnz	2$
4$:	ld	a, (bgc+1)
	ld	(de), a
	ld	a, (bgc)
	ld	(de), a
	pop	hl
	sla	c
	jr	nc, 1$
	ret
