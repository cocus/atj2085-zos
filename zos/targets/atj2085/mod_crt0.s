;; zos module start-up: header at PAGE_BASE (0x4000) + C runtime init. See core/module.h.
;; size/end/api/name are patched by build.py.
	.module mod_crt0
	.globl	_mod_main
	.globl	s__INITIALIZER, l__INITIALIZER, s__INITIALIZED, s__DATA, l__DATA
	.area	_CODE
	.ascii	"ZM"
	.db	1			; version
	.db	0			; api count (patched)
	.dw	0			; size (patched)
	.dw	0			; end (patched)
	.dw	mod_init
	.dw	_mod_main
	.ascii	"MODULE"		; name[16] (patched)
	.db	0,0,0,0,0,0,0,0,0,0
mod_init:
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
2$:	jp	gsinit			; SDCC's static-initializer code (_GSINIT), ends with RET (_GSFINAL)

	;; area order: code + initializer data in the file, then RAM-only areas
	.area	_HOME
	.area	_INITIALIZER
	.area	_GSINIT
gsinit:					; mod_crt0 is linked first: this is the start of _GSINIT
	.area	_GSFINAL
	ret
	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area	_BSS
	.area	_HEAP
