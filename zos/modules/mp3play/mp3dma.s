;; Bitstream DMA (mp3_driver_spec.md 5.2), exactly as the stock driver issues it: with port 05 =
;; PAGE_IDMLO_L (IDM low half handed to the MCU) - with all halves DSP-owned the DMA does not reach
;; DSP RAM (device). Remapping the window hides this module's own code (ZRAM2), so the routine is
;; only a template here: mp3.c copies it into ZRAM1 (module variables) and calls the copy.
;; Position independent (relative jumps only). 64 bytes (mp3.c dma_ram).
;;   void dma(const uint8_t *buf512)      HL = buf (ZRAM1)
	.module mp3dma
	.include "regs.inc"
	.area	_CODE
_mp3_dma_tpl::
	ld	a, l
	out	(P_DMA1_SA0), a
	ld	a, h
	out	(P_DMA1_SA1), a
	in	a, (P_PAGE)
	ld	c, a
	ld	a, #PAGE_IDMLO_L	; as the stock codec, while the DSP waits for our reply
	out	(P_PAGE), a
	xor	a, a
	out	(P_DMA1_SA2), a
	out	(P_DMA1_SA3), a		; DMA_A3_ZRAM1
	out	(P_DMA1_ISA), a
	ld	a, #0xb3		; DA = DSP word 0x1CB3 (bitstream buffer, counting down)
	out	(P_DMA1_DA0), a
	ld	a, #0x1c
	out	(P_DMA1_DA1), a
	xor	a, a
	out	(P_DMA1_DA2), a
	ld	a, #DMA_A3_DSPRAM
	out	(P_DMA1_DA3), a
	ld	a, #DMA_LANE_IDM_H
	out	(P_DMA1_IDA), a
	ld	a, #0xff		; count - 1 = 0x01FF (512 bytes)
	out	(P_DMA1_BCL), a
	ld	a, #0x01
	out	(P_DMA1_BCH), a
	ld	a, #DMA_MODE_DSP | DMA_MODE_DSTDN
	out	(P_DMA1_MODE), a
	ld	a, #DMA_CMD_START
	out	(P_DMA1_CMD), a		; the CPU halts until done
1$:	in	a, (P_DMA1_CMD)
	rrca				; DMA_CMD_START = bit 0
	jr	c, 1$
	ld	a, c
	out	(P_PAGE), a		; window back (module code visible again)
	ret
_mp3_dma_tpl_end::

;; the module's second code section (_CODE2, ZRAM1): where/how long, for main.c's loader
	.globl	s__CODE2, l__CODE2
_code2_addr::	.dw	s__CODE2
_code2_len::	.dw	l__CODE2
