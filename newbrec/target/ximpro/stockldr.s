;; newbrec target ximpro: the STOCK loader. Assembled for 0x4000 (build.py turns it into stockldr.h; the
;; boot record copies it to the ZRAM2 window and calls it, because it overwrites 0x0000-0x3FFF).
;;
;; Finds a boot record copy in NAND block 2, else block 3 (unit 1: outside the stock FTL, which scans
;; units from 2; the BROM itself tries blocks 0,1,2,3), the way the mask ROM does (data columns of pages
;; 0-7, word 0x3FFC = 0x55AA, 16-bit word sum), plus the text "BREC" at byte 4. Pass 1 reads a candidate
;; into the ZRAM2 buffer 0x4800 and checks it WITHOUT touching 0x0000-0x3FFF, so a failure returns to the
;; caller (A = 1) with the new boot record intact. Pass 2 re-reads the pages to 0x0000.., restores the
;; machine state the mask ROM leaves (usb_msc.md 4.2: 4E = A2, 04 = 01, 27 = 0, 42 = 80, 70 |= 30,
;; 05 = F7, 00 = 10, sp = 4F00) and the registers this boot record changed, and jumps to 0x0000.
	.module stockldr
	.include "regs.inc"
BUF	= 0x4800
BLK	= 0x47F0
ADR	= 0x47F2
	.area	_LDR (ABS)
	.org	0x4000
ldr:	di
	ld	a, #2
	ld	(BLK), a
1$:	call	check
	jr	z, load
	ld	a, (BLK)
	inc	a
	ld	(BLK), a
	cp	a, #4
	jr	c, 1$
	ld	a, #1
	ret

;; Z = the block in BLK holds a valid boot record
check:	ld	de, #0			; running sum
	ld	b, #0			; page
2$:	push	bc
	push	de
	ld	a, b
	ld	hl, #BUF
	call	rdpage
	pop	de
	pop	bc
	ld	a, b
	or	a, a
	jr	nz, 3$
	ld	hl, (BUF+4)		; page 0: "BREC" at bytes 4-7
	ld	a, #'B'
	cp	a, l
	ret	nz
	ld	a, #'R'
	cp	a, h
	ret	nz
	ld	hl, (BUF+6)
	ld	a, #'E'
	cp	a, l
	ret	nz
	ld	a, #'C'
	cp	a, h
	ret	nz
3$:	ld	bc, #1024		; words per page
	ld	a, (ADR+8)		; page number (kept by rdpage)
	cp	a, #7
	jr	nz, 5$
	ld	bc, #1023		; last page: the stored sum itself is not part of the sum
5$:	ld	hl, #BUF
6$:	ld	a, e
	add	a, (hl)
	ld	e, a
	inc	hl
	ld	a, d
	adc	a, (hl)
	ld	d, a
	inc	hl
	dec	bc
	ld	a, b
	or	a, c
	jr	nz, 6$
	ld	a, (ADR+8)
	cp	a, #7
	jr	nz, 7$
	ld	a, (BUF+0x7FC)		; 0x55AA at 0x3FFC
	cp	a, #0xAA
	ret	nz
	ld	a, (BUF+0x7FD)
	cp	a, #0x55
	ret	nz
	ld	a, (hl)			; HL -> the stored sum (word 0x3FFE)
	cp	a, e
	ret	nz
	inc	hl
	ld	a, (hl)
	cp	a, d
	ret
7$:	inc	a
	ld	b, a
	jr	2$

load:	ld	sp, #0x4F00		; the stack must not be in 0x0000-0x3FFF any more
	ld	b, #0
8$:	push	bc
	ld	a, b
	ld	l, #0
	sla	a
	sla	a
	sla	a
	ld	h, a			; dest = page * 0x800
	ld	a, b
	call	rdpage
	pop	bc
	inc	b
	ld	a, b
	cp	a, #8
	jr	c, 8$
	ld	a, #0xA2
	out	(P_WDT), a
	ld	a, #0x01
	out	(0x04), a
	xor	a, a
	out	(0x27), a
	ld	a, #0x80
	out	(P_PLLCTL), a
	in	a, (P_CLKCTL2)
	or	a, #CLK2_BANKS
	out	(P_CLKCTL2), a
	ld	a, #0xF7
	out	(P_PAGE), a
	ld	a, #0x10
	out	(P_CLKCTL), a
	xor	a, a			; registers this boot record changed: back to their reset values
	out	(P_USB_PULLUP), a
	ld	a, #0x04
	out	(P_RTCCTL), a
	ld	a, #0xEC
	out	(P_DCDC), a
	ld	a, #0x28
	out	(P_PWRCTL), a
	ld	a, #0x01
	out	(P_MFP), a
	ld	a, #0x07
	out	(P_GPC_EN), a
	ld	a, #0x03
	out	(P_GPC_D), a
	im	1
	jp	0x0000

;; A = page 0-7 of block (BLK), HL = destination: 2048 data bytes
rdpage:	ld	(ADR+8), a
	push	hl
	ld	c, a
	ld	a, (BLK)
	rrca
	rrca
	or	a, c			; row0 = block << 6 | page (block 2 or 3, row1 = row2 = 0)
	ld	(ADR+2), a
	xor	a, a
	ld	(ADR), a
	ld	(ADR+1), a
	ld	(ADR+3), a
	ld	(ADR+4), a
	out	(P_EMLO), a
	ld	a, #EMHI_CE1_NAND
	out	(P_EMHI), a
	ld	a, #NAND_CEMODE_SAMSUNG
	out	(P_NAND_CEMODE), a
	ld	a, #NAND_EN_CE1
	out	(P_NAND_EN), a
	ld	a, #NAND_STATUS
	out	(P_NAND_CMD), a
	ld	a, #EM_CLE
	out	(P_EMLO), a
	xor	a, a
	ld	(EXT_WINDOW), a		; read
	ld	a, #EM_ALE
	out	(P_EMLO), a
	ld	hl, #ADR
	ld	b, #5
9$:	ld	a, (hl)
	ld	(EXT_WINDOW), a
	inc	hl
	djnz	9$
	xor	a, a
	out	(P_EMLO), a
	ld	a, #NAND_READ2
	out	(P_NAND_CMD), a
	ld	b, #0
10$:	djnz	10$			; tR
	pop	de
	ld	bc, #0x800
	ld	hl, #EXT_WINDOW
	ldir
	ld	a, #NAND_STOP
	out	(P_NAND_CMD), a
	xor	a, a
	out	(P_NAND_EN), a
	out	(P_NAND_CEMODE), a
	out	(P_EMHI), a
	out	(P_EMLO), a
	ret
