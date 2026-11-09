;; Chain-loader, v2. The launcher has already read the whole app image into DSP RAM page LPAGE
;; (window 0x4000) and closed the file, so nothing here calls the OS: the stock FS driver keeps
;; a cluster cache at 0x2C00.. (FS_FAT16) and must not see a half-overwritten launcher anyway.
;;
;; _page_put(A = page, DE = &{u16 dst; u16 src; u16 len}): DI, copy ZRAM1 -> page, restore page, EI
;; _page_enter(A = page, DE = address in page): DI, map the page, JP there (no return)
;;
;; The stub (copied to STUB_CODE inside the page, position independent) runs with the page mapped:
;;   copy image 0x4000 -> 0x0800, restore the OS registers/SP, then a 4-byte trampoline written
;;   below the OS stack does: OUT (0x05),A (A = OS page) ; EI ; RET (-> app entry, SP = OS SP).
;; Parameters (in the page) at STUB_PAR = 0x7EF0:
;;   +0 len  +2 entry  +4 SP  +6 BC  +8 DE  +10 IX  +12 IY  +14 OS page (port 0x05)
	.module stub
	.area	_CODE

_page_put::
	ld	c, a
	di
	in	a, (0x05)
	push	af
	ld	a, c
	out	(0x05), a
	ex	de, hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	inc	hl
	ld	c, (hl)
	inc	hl
	ld	b, (hl)
	inc	hl
	push	bc
	ld	c, (hl)
	inc	hl
	ld	b, (hl)
	pop	hl
	ld	a, b
	or	a, c
	jr	z, 1$
	ldir
1$:	pop	af
	out	(0x05), a
	ei
	ret

_os_page::
	in	a, (0x05)
	ret

_page_enter::
	di
	out	(0x05), a
	ex	de, hl
	jp	(hl)

_stub_start::
	ld	hl, #0x4000
	ld	de, #0x0800
	ld	bc, (0x7ef0)
	ldir
	ld	sp, (0x7ef4)
	ld	hl, (0x7ef2)		; [SP-2] = entry
	push	hl
	ld	hl, #0xC9FB		; [SP-4] = EI, [SP-3] = RET
	push	hl
	ld	hl, #0x05D3		; [SP-6] = OUT (0x05),A
	push	hl
	ld	hl, #0
	add	hl, sp			; HL = trampoline
	inc	sp
	inc	sp
	inc	sp
	inc	sp			; SP -> entry word
	ld	bc, (0x7ef6)
	ld	de, (0x7ef8)
	ld	ix, (0x7efa)
	ld	iy, (0x7efc)
	ld	a, (0x7efe)
	jp	(hl)
_stub_end::
