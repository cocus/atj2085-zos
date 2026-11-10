;; newbrec target ximpro: image header, entry points the mask ROM / the FIRMWARE module expect, start-up.
;; Own code. The 16 KB image is loaded by the BROM to RAM 0x0000 and started at 0x0000 (interrupts off).
;;
;; Family compatibility (zos FIRMWARE module "BREC UPDATE" accepts a boot record when image bytes
;; 0x0000-0x0007 and 0x0200-0x0214 equal those of the installed one): the header `jp 0x0200 ; "BREC"`
;; and an entry sequence with the same instructions (`ld sp,0x200 ; in/or/out port DF ; call 0x1B12 ;
;; call 0x0216 ; ld sp,0x400 ; jp 0x600`). The two calls land on small jump stubs placed at those
;; addresses: 0x1B12 = our main (never returns), 0x0216 = the payload launcher (M2; M1: reset).
	.module head
	.include "regs.inc"
	.globl	_main, _tgt_reset
	.globl	s__DATA, l__DATA

	.area	_HEAD (ABS)
	.org	0x0000
	jp	0x0200
	.db	0x00
	.ascii	"BREC"
	.ascii	"F641"			; boot record family
	.ascii	"00"
	.db	0x20
	.ascii	"NEWBREC M1"		; our tag (stock: release date bytes)

;; unused vectors: any stray RST / interrupt ends in a watchdog reset; NMI returns
	.org	0x0038
	jp	_tgt_reset
	.org	0x0066
	retn

	.area	_ENTRY (ABS)
	.org	0x0200
	ld	sp, #0x0200
	in	a, (P_PWRCTL)
	or	a, #0x01
	out	(P_PWRCTL), a
	call	0x1B12
	call	0x0216
	ld	sp, #0x0400
	jp	0x0600
	.db	0x76
	.org	0x0216
	jp	_tgt_reset		; M2: payload launcher

	.area	_STUB (ABS)
	.org	0x1B12
	jp	_brec_main

	.area	_CODE
_brec_main::
	di
	ld	sp, #0x0200
	ld	hl, #s__DATA
	ld	bc, #l__DATA
	ld	a, b
	or	a, c
	jr	z, 1$
	ld	(hl), #0
	dec	bc
	ld	a, b
	or	a, c
	jr	z, 1$
	ld	d, h
	ld	e, l
	inc	de
	ldir
1$:	call	_main
	jp	_tgt_reset
