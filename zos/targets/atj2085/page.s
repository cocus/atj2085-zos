;; zos paging: 16 KB pages of on-chip RAM in the 0x4000-0x7FFF window, selected by port 0x05.
;; Device-measured (MEMORY PROBE): 0xC0/C1/C2 = DSP program RAM lanes, 0x34/35/36 = DSP data RAM
;; lanes, each an independent, writable, executable 16 KB page; the stock OS lives in 0xF7 (ZRAM2).
;; All of this code runs from ZRAM1, and the interrupt handler never touches the window.
	.module page
	.include "regs.inc"
	.area	_CODE

;; uint8_t page_get(void)
_page_get::
	in	a, (P_PAGE)
	ret

;; void page_set(uint8_t v)                  A
_page_set::
	out	(P_PAGE), a
	ret

;; uint16_t mod_call(uint8_t page, uint16_t fn)   A = page value, DE = function
;; calls fn() with the window switched to `page`, restores the previous page, returns fn's DE
_mod_call::
	ld	c, a
	in	a, (P_PAGE)
	push	af
	ld	a, c
	out	(P_PAGE), a
	ex	de, hl
	call	1$
	pop	af
	out	(P_PAGE), a
	ret
1$:	jp	(hl)

;; void page_write(uint8_t page, const xfer_t *x)   A = page, DE = {u16 dst; const void *src; u16 len}
;; copies len bytes from ZRAM1 (src) to the page (dst in 0x4000-0x7FFF)
_page_write::
	ld	c, a
	in	a, (P_PAGE)
	push	af
	ld	a, c
	out	(P_PAGE), a
	ex	de, hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)			; DE = dst
	inc	hl
	ld	c, (hl)
	inc	hl
	ld	b, (hl)			; BC = src
	inc	hl
	push	bc
	ld	c, (hl)
	inc	hl
	ld	b, (hl)			; BC = len
	pop	hl			; HL = src
	ld	a, b
	or	a, c
	jr	z, 1$
	ldir
1$:	pop	af
	out	(P_PAGE), a
	ret

;; void page_read(uint8_t page, const xfer_t *x)    A = page, DE = {u16 dst (ZRAM1); u16 src (page); u16 len}
_page_read::
	ld	c, a
	in	a, (P_PAGE)
	push	af
	ld	a, c
	out	(P_PAGE), a
	ex	de, hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)			; DE = dst
	inc	hl
	ld	c, (hl)
	inc	hl
	ld	b, (hl)			; BC = src
	inc	hl
	push	bc
	ld	c, (hl)
	inc	hl
	ld	b, (hl)
	pop	hl
	ld	a, b
	or	a, c
	jr	z, 1$
	ldir
1$:	pop	af
	out	(P_PAGE), a
	ret
