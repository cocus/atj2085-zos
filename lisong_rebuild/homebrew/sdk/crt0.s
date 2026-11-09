;; ACTOS 3.x homebrew AP start-up (bank-less AP, loaded by the OS at 0x0800).
;; The OS enters an AP like an IAR banked function: SP -> [bank word][return],
;; param in DE; the result goes back in HL and the exit must restore the caller's
;; bank (same sequence as IAR's ?BANK_FAST_LEAVE in every stock AP).
	.module crt0
	.globl	_main
	.globl	s__INITIALIZER, l__INITIALIZER, s__INITIALIZED, s__DATA, l__DATA

	.area	_CODE
ap_entry::
	;; remember exactly how the OS entered us (used by the launcher to chain-load an app
	;; so that the app returns straight to the OS). Kept in _CODE (RAM) so the C runtime
	;; init below does not clear it.
	ld	(_ap_entry_sp), sp
	ld	(_ap_entry_bc), bc
	ld	(_ap_entry_de), de
	ld	(_ap_entry_ix), ix
	ld	(_ap_entry_iy), iy
	push	bc
	push	de
	push	ix
	push	iy
	;; C runtime init: copy initialised data, clear zero-initialised data
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
2$:	pop	iy
	pop	ix
	pop	de
	push	de
	push	ix
	push	iy
	ex	de, hl			; sdcccall(1): uint16_t main(uint16_t param) -> param in HL
	call	_main			; result in DE
	ex	de, hl			; IAR ABI: result in HL
	pop	iy
	pop	ix
	pop	de
	pop	bc
	;; ?BANK_FAST_LEAVE equivalent: restore the caller's bank, keep HL
	ex	de, hl
	ex	(sp), hl		; HL = bank word, (SP) = saved DE
	ex	de, hl			; DE = bank word, HL = result
	push	hl
	call	if_bank_resident
	jr	z, 3$
	ld	hl, #0x1008		; BankSwitch(E = bank)
	rst	0x10
3$:	pop	hl
	pop	de
	ret

;; Z if bank E is resident (OS bank table pointer at 0x0098, indexed by bank>>5)
if_bank_resident:
	push	de
	ld	a, e
	rlca
	rlca
	rlca
	and	a, #0x07
	push	hl
	push	bc
	ld	hl, (0x0098)
	ld	b, #0
	ld	c, a
	add	hl, bc
	ld	a, (hl)
	cp	a, e
	pop	bc
	pop	hl
	pop	de
	ret

_ap_entry_sp::	.dw	0
_ap_entry_bc::	.dw	0
_ap_entry_de::	.dw	0
_ap_entry_ix::	.dw	0
_ap_entry_iy::	.dw	0

	;; area order: everything follows _CODE contiguously
	.area	_HOME
	.area	_INITIALIZER
	.area	_GSINIT
	.area	_GSFINAL
	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area	_BSS
	.area	_HEAP
