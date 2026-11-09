;; zos NAND low level (ATJ2085, Samsung-type 2 KB page NAND on CE1), read only.
;; Spec: lisong_rebuild/analysis/nand_ftl.md 1.1-1.4 (BREC 0x3D3A select, 0x3D9A cycles, 0x3E30 read).
;; P_EMHI = chip enable (EMHI_CE1_NAND); P_EMLO = bus phase of writes into EXT_WINDOW
;; (EM_CLE / EM_ALE / EM_DATA); P_NAND_CEMODE/P_NAND_EN enable the sequencer for the CE;
;; P_NAND_CMD = sequencer command. The data address inside the window is irrelevant, so the data
;; phase is a plain LDIR from EXT_WINDOW.
	.module nand
	.include "regs.inc"
	.area	_CODE

;; uint8_t nand_read(const nand_req_t *r)          HL = r -> A = 0
;;   r = { u8 col_lo, col_hi, row0, row1, row2; u16 buf; u16 len }   (len 1..0x840)
;; Interrupts are disabled while the CE is selected (the window is not the LCD then);
;; P_EMLO/P_EMHI are restored (zos keeps EMHI_CE3_LCD).
_nand_read::
	push	ix
	push	hl
	pop	ix
	ld	a, i			; P/V = IFF2: were interrupts enabled?
	jp	pe, 9$
	ld	a, i			; Z80 erratum: an IRQ accepted during LD A,I reads P/V = 0
9$:	push	af			;   (interrupts then stayed off: HALT forever -> watchdog reset)
	di
	in	a, (P_EMLO)
	ld	c, a
	in	a, (P_EMHI)
	ld	b, a
	push	bc			; saved P_EMLO / P_EMHI
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
	ld	a, #EM_DATA
	out	(P_EMLO), a
	ld	a, #EM_CLE
	out	(P_EMLO), a
	ld	a, #NAND_READ1
	ld	(EXT_WINDOW), a
	ld	a, #EM_ALE
	out	(P_EMLO), a
	ld	a, 0 (ix)		; column low, column high, row 0..2
	ld	(EXT_WINDOW), a
	ld	a, 1 (ix)
	ld	(EXT_WINDOW), a
	ld	a, 2 (ix)
	ld	(EXT_WINDOW), a
	ld	a, 3 (ix)
	ld	(EXT_WINDOW), a
	ld	a, 4 (ix)
	ld	(EXT_WINDOW), a
	ld	a, #EM_DATA
	out	(P_EMLO), a
	ld	a, #NAND_READ2		; read confirm: tR starts
	out	(P_NAND_CMD), a
	ld	b, #0			; tR <= 25 us: 256 x 13 T = 136 us at 24.576 MHz (68 at 48)
1$:	djnz	1$
	ld	e, 5 (ix)
	ld	d, 6 (ix)
	ld	c, 7 (ix)
	ld	b, 8 (ix)
	ld	a, b
	or	a, c
	jr	z, 2$
	ld	hl, #EXT_WINDOW
	ldir
2$:	ld	a, #NAND_STOP		; stop sequencer, release CE
	out	(P_NAND_CMD), a
	xor	a, a
	out	(P_NAND_EN), a
	out	(P_NAND_CEMODE), a
	pop	bc
	ld	a, b
	out	(P_EMHI), a
	ld	a, c
	out	(P_EMLO), a
	pop	af
	jp	po, 3$			; interrupts were off: leave them off
	ei
3$:	pop	ix
	xor	a, a
	ret
