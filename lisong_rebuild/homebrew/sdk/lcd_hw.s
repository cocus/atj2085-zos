;; Raw LCD access, identical to the stock UI driver (UI_6B33*.DRV):
;; data and commands are written to memory 0x8000 with port 0x02 = 0x18 (CE3 = LCD),
;; port 0x00 |= 0xC0 (3 external wait states), port 0xF4 bit 2 = A0 (0 cmd / 1 data).
;; Interrupts are disabled with the OS nesting counter at 0x0037 like the driver does.
	.module lcd_hw
	.area	_DATA
save02:	.ds	1
save00:	.ds	1
	.area	_CODE

_lcd_lock::
	di
	ld	a, (0x0037)
	inc	a
	ld	(0x0037), a
	in	a, (0x02)
	ld	(save02), a
	ld	a, #0x18
	out	(0x02), a
	in	a, (0x00)
	ld	(save00), a
	or	a, #0xc0
	out	(0x00), a
	ret

_lcd_unlock::
	ld	a, (save00)
	out	(0x00), a
	ld	a, (save02)
	out	(0x02), a
	ld	a, (0x0037)
	dec	a
	ld	(0x0037), a
	ret	nz
	ei
	ret

;; void lcd_cmd(uint8_t c)    (A = c)
_lcd_cmd::
	ld	l, a
	in	a, (0xf4)
	res	2, a
	out	(0xf4), a
	ld	a, l
	ld	(0x8000), a
	ret

;; void lcd_data(uint8_t d)   (A = d)
_lcd_data::
	ld	l, a
	in	a, (0xf4)
	set	2, a
	out	(0xf4), a
	ld	a, l
	ld	(0x8000), a
	ret

;; void lcd_fill_words(uint16_t color, uint16_t count)   HL = color, DE = count
;; (A0 must already be set to data)
_lcd_fill_words::
	ld	a, d
	or	a, e
	ret	z
1$:	ld	a, h
	ld	(0x8000), a
	ld	a, l
	ld	(0x8000), a
	dec	de
	ld	a, d
	or	a, e
	jr	nz, 1$
	ret
