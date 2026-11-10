;; newbrec target ximpro: NAND primitives (Samsung 2 KB page, CE1, ATJ2085 sequencer). Own code.
;; Sequences as documented in lisong_rebuild/analysis/nand_ftl.md 1.x. Interrupts are never enabled in
;; the boot record, so there is no IFF handling. The window 0x8000 is the NAND while CE1 is selected;
;; EMHI/EMLO are saved and restored (the LCD keeps EMHI = CE3 otherwise).
;;
;; All routines work on the global request block `nreq` (C: nand_req_t, 9 bytes):
;;   +0 col_lo +1 col_hi +2 row0 +3 row1 +4 row2 (address cycles)  +5 buf (word)  +7 len (word)
;; Results: A = 0 ok, 4 program/erase failed, 5 busy timeout, 6 write protected.
	.module nand
	.include "regs.inc"
	.globl	_nreq
	.area	_DATA
sv:	.ds	2
spr:	.ds	2
	.area	_CODE

sel:	in	a, (P_EMLO)
	ld	(sv), a
	in	a, (P_EMHI)
	ld	(sv+1), a
	ld	a, #EM_DATA
	out	(P_EMLO), a
	ld	a, #EMHI_CE1_NAND
	out	(P_EMHI), a
	ld	a, #NAND_CEMODE_SAMSUNG
	out	(P_NAND_CEMODE), a
	ld	a, #NAND_EN_CE1
	out	(P_NAND_EN), a
	ld	a, #NAND_STATUS		; known state
	out	(P_NAND_CMD), a
	ret

;; release, A and C preserved
rel:	ld	b, a
	ld	a, #NAND_STOP
	out	(P_NAND_CMD), a
	xor	a, a
	out	(P_NAND_EN), a
	out	(P_NAND_CEMODE), a
	ld	a, (sv+1)
	out	(P_EMHI), a
	ld	a, (sv)
	out	(P_EMLO), a
	ld	a, b
	ret

;; command B, then C address bytes from (HL)
cyc:	ld	a, #EM_DATA
	out	(P_EMLO), a
	ld	a, #EM_CLE
	out	(P_EMLO), a
	ld	a, b
	ld	(EXT_WINDOW), a
	ld	a, #EM_ALE
	out	(P_EMLO), a
	ld	a, c
	or	a, a
	jr	z, 2$
	ld	b, c
1$:	ld	a, (hl)
	ld	(EXT_WINDOW), a
	inc	hl
	djnz	1$
2$:	ld	a, #EM_DATA
	out	(P_EMLO), a
	ret

;; wait ready, status -> C (0 ok, 4 fail, 5 timeout, 6 write protected)
wait:	ld	b, #0x60
1$:	djnz	1$
	ld	a, #NAND_STATUS
	out	(P_NAND_CMD), a
	ld	de, #0x7000
2$:	ld	a, (EXT_WINDOW)
	ld	b, a
	and	a, #NAND_ST_READY
	jr	nz, 3$
	dec	de
	ld	a, d
	or	a, e
	jr	nz, 2$
	ld	a, #NAND_RESET
	out	(P_NAND_CMD), a
	ld	c, #5
	ret
3$:	ld	c, #4
	ld	a, b
	and	a, #NAND_ST_FAIL
	ret	nz
	ld	c, #6
	ld	a, b
	and	a, #NAND_ST_NWP
	ret	z
	ld	c, #0
	ret

;; uint8_t nand_id(uint8_t *out4)    HL = out (4 bytes: maker, device, 2 more)
_nand_id::
	push	hl
	call	sel
	ld	a, #NAND_ID		; CLE 0x90 + one address cycle (0)
	out	(P_NAND_CMD), a
	ld	b, #0x60
1$:	djnz	1$
	pop	hl
	ld	b, #4
2$:	ld	a, (EXT_WINDOW)
	ld	(hl), a
	inc	hl
	djnz	2$
	xor	a, a
	jp	rel

;; uint8_t nand_read(void): len bytes (1..0x840) at column / row into buf (any RAM address)
_nand_read::
	call	sel
	ld	hl, #_nreq
	ld	bc, #(NAND_READ1 << 8) | 5
	call	cyc
	ld	a, #NAND_READ2		; read confirm: tR starts
	out	(P_NAND_CMD), a
	ld	b, #0			; tR <= 25 us: 256 x 13 T = 136 us at 24.576 MHz
1$:	djnz	1$
	ld	de, (_nreq+5)
	ld	bc, (_nreq+7)
	ld	hl, #EXT_WINDOW
	ldir
	xor	a, a
	jp	rel

;; uint8_t nand_erase(void): the block whose first page row is in row0..row2 (single-block erase)
_nand_erase::
	call	sel
	ld	hl, #_nreq+2
	ld	bc, #(NAND_ERASE1 << 8) | 3
	call	cyc
	ld	a, #NAND_ERASE2
	out	(P_NAND_CMD), a
	call	wait
	ld	a, c
	jp	rel

;; uint8_t nand_prog(void): program one 512-byte sector at column col (= 512 * sector in the page) of
;; page `row` from buf (PIO), with the hardware ECC engine counting the data cycles, and the 16 spare
;; bytes of that sector (column 0x800 + 16 * sector): FF x8, ECC x4, FF FF 55 FF  (the BREC format)
_nand_prog::
	call	sel
	ld	hl, #_nreq
	ld	bc, #(NAND_PROG1 << 8) | 5
	call	cyc
	ld	a, #ECC_START
	out	(P_ECC_CTL), a
	ld	hl, (_nreq+5)
	ld	bc, #512
1$:	ld	a, (hl)
	ld	(EXT_WINDOW), a
	inc	hl
	dec	bc
	ld	a, b
	or	a, c
	jr	nz, 1$
2$:	in	a, (P_ECC_CTL)
	and	a, #ECC_BUSY
	jr	nz, 2$
	ld	a, (_nreq+1)		; column high = 2 * sector -> spare column = 0x800 + 8 * that
	add	a, a
	add	a, a
	add	a, a
	ld	(spr), a
	ld	a, #(NAND_SPARE >> 8)
	ld	(spr+1), a
	ld	hl, #spr
	ld	bc, #(NAND_RNDIN << 8) | 2
	call	cyc
	ld	a, #0xFF
	ld	b, #8
3$:	ld	(EXT_WINDOW), a
	djnz	3$
	in	a, (P_ECC_SP8)
	ld	(EXT_WINDOW), a
	in	a, (P_ECC_SP9)
	ld	(EXT_WINDOW), a
	in	a, (P_ECC_SP10)
	ld	(EXT_WINDOW), a
	in	a, (P_ECC_SP11)
	ld	(EXT_WINDOW), a
	ld	a, #0xFF
	ld	(EXT_WINDOW), a
	ld	(EXT_WINDOW), a
	ld	a, #0x55
	ld	(EXT_WINDOW), a
	ld	a, #0xFF
	ld	(EXT_WINDOW), a
	ld	a, #NAND_PROG2
	out	(P_NAND_CMD), a
	call	wait
	ld	a, c
	jp	rel
