;; FTL write path (modules/common/ftlw.inc, API in ftlw.h) with the busy-wait hook ftlw_idle()
;; (recio.c: keeps the ADC FIFO drained and the DSP served while the NAND programs / erases)
	.module ftlw
FTLW_IDLE = 1
	.include "../common/ftlw.inc"
