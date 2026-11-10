;; FTL write path (modules/common/ftlw.inc, API in ftlw.h) with the FWIMAGE copy A entry
;; ftlw_fwwrite() (firmware logical units 0..0x1F only) and the raw boot-record path
;; ftlw_brec_erase/prog (physical blocks 0 and 1 only)
	.module ftlw
FTLW_FW = 1
FTLW_BREC = 1
	.include "../common/ftlw.inc"
