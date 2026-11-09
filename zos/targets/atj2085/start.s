;; zos start-up (after the API jump table), "takeover" mode: zos needs nothing from the stock OS
;; while it runs (own IRQ handler, keys, LCD, NAND/FTL, FAT), so it owns all of ZRAM1 above the
;; stock stack: its variables may cover the stock resident kernel (0x2E00-0x32FF) and the UD
;; driver (0x3300-). The only stock data used is read by hal_init/hal_blk_init (RAM 0x008A
;; flash size, 0x3C80 FTL table descriptors), which lie outside zos' memory (build.py checks).
;; Leaving zos = watchdog reset -> the untouched stock firmware boots again.
	.module start
	.include "regs.inc"
	.globl	_zos_main
	.globl	_zos_gsinit
	.globl	s__INITIALIZER, l__INITIALIZER, s__INITIALIZED, s__DATA, l__DATA
	.area	_CODE
_zos_start::
	di				; no stock IRQ handler from here on (its code may be overwritten)
	ld	bc, #l__INITIALIZER
	ld	a, b
	or	a, c
	jr	z, 1$
	ld	de, #s__INITIALIZED
	ld	hl, #s__INITIALIZER
	ldir
1$:	ld	bc, #l__DATA
	ld	a, b
	or	a, c
	jr	z, 2$
	ld	hl, #s__DATA
	ld	(hl), #0
	dec	bc
	ld	a, b
	or	a, c
	jr	z, 2$
	ld	d, h
	ld	e, l
	inc	de
	ldir
2$:	call	_zos_gsinit
	call	_zos_main		; hal_init installs zos' IRQ handler and enables interrupts
	;; fall through: reboot
_zos_reboot::
	di
	ld	a, #WDT_REBOOT	; watchdog on, 176 ms, clear
	out	(P_WDT), a
3$:	jr	3$			; wait for the watchdog reset
