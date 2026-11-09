;; FTL write path (modules/common/ftlw.inc, API in ftlw.h) with the FWIMAGE copy A entry
;; ftlw_fwwrite() (firmware logical units 0..0x1F only)
	.module ftlw
FTLW_FW = 1
	.include "../common/ftlw.inc"
