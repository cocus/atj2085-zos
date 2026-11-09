;; USB module helpers in assembler.
;;
;; usbz_tpl: TEMPLATE of a ZRAM1 routine. It switches the 0x4000 window to ZRAM2 (the USB buffer
;; RAM), which hides this module's own code (it runs from its DSP RAM page at 0x4000), so it is
;; never called here: usbdev.c copies it into ZRAM1 (zr->stub, HAL_MODRAM block) and calls the copy.
;; Position independent: no jumps, only absolute calls into the core (API jump table).
;;   uint8_t blkz(const blkz_t *a)       HL = a (ZRAM1) = { u32 lba; u16 dst }
;; reads user-disk block `lba` with the core's hal_blk_read straight into ZRAM2 at CPU address
;; `dst` (0x4000-0x4FFF; the caller has given the bank(s) to the MCU via P_CLKCTL2). Returns
;; hal_blk_read's result (0 = failed). hal_blk_read: SDCC __sdcccall(1), lba in HL:DE (HL = high
;; word), buf on the stack, popped by the callee (checked in the generated core code), result in A.
	.module usbz
	.include "regs.inc"
	.globl	_hal_blk_read
	.area	_CODE
_usbz_tpl::
	in	a, (P_PAGE)
	push	af			; caller's window (this module's page)
	ld	a, #PAGE_ZRAM2
	out	(P_PAGE), a
	ld	e, (hl)
	inc	hl
	ld	d, (hl)			; DE = lba bits 15-0
	inc	hl
	ld	c, (hl)
	inc	hl
	ld	b, (hl)			; BC = lba bits 31-16
	inc	hl
	ld	a, (hl)
	inc	hl
	ld	h, (hl)
	ld	l, a			; HL = dst
	push	hl			; buf512 (stack argument)
	ld	l, c
	ld	h, b
	call	_hal_blk_read		; absolute (API table), A = result
	ld	c, a
	pop	af
	out	(P_PAGE), a		; module page back
	ld	a, c
	ret
_usbz_tpl_end::

;; uint8_t port_in(uint8_t port)           A = port -> A
_port_in::
	ld	c, a
	in	a, (c)
	ret

;; void port_out(uint8_t port, uint8_t v)  A = port, L = v
_port_out::
	ld	c, a
	out	(c), l
	ret
