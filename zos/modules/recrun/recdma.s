;; void rec_dma(const uint8_t *desc13)    HL = DMA1 SA0, SA1, SA2, SA3, ISA, DA0..DA3, IDA, BCL, BCH,
;; MODE (13 bytes, ZRAM1). Like the stock ADENCM driver (RECORD.AP 0C:2AD3, with port 05 = 0x24
;; around it, 0C:29A0 / 0C:29EA) the window gives the DSP's IDM low half to the MCU during the
;; transfer: that hides this module's ZRAM2 code, so the routine lives in _CODE2 (ZRAM1).
;; Interrupt-safe (no RAM but the stack; port 05 restored).
	.module recdma
	.include "regs.inc"
	.area	_CODE2
_rec_dma::
	in	a, (P_PAGE)
	ld	e, a
	ld	a, #PAGE_IDMLO_L
	out	(P_PAGE), a
	ld	bc, #(13 << 8) | P_DMA1_SA0
1$:	ld	a, (hl)
	out	(c), a
	inc	hl
	inc	c
	djnz	1$
	ld	a, #DMA_CMD_START
	out	(P_DMA1_CMD), a		; the CPU halts until done
2$:	in	a, (P_DMA1_CMD)
	rrca				; DMA_CMD_START = bit 0
	jr	c, 2$
	ld	a, e
	out	(P_PAGE), a
	ret

;; the module's second code section (_CODE2, ZRAM1): where/how long, for main.c's loader
	.area	_CODE
	.globl	s__CODE2, l__CODE2
_code2_addr::	.dw	s__CODE2
_code2_len::	.dw	l__CODE2
