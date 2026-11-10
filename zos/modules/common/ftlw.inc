;; zos NAND/FTL write path (module side), compatible with the stock "641" FTL. API + rules: ftlw.h.
;; Use: a module adds one .s file containing   .include "../common/ftlw.inc"
;; (assembler, not C, because the USB module has little room left: ~0.6 KB here vs ~1.5 KB as C).
;;
;; FTL (nand_ftl.md 2.x, 3): global logical unit lu = 0x40 + 1 + lba / 512 (FAT volume = UD sector
;; 0x200 + lba), zone z = lu / 0x7B0, L = lu % 0x7B0; physical unit = z * 2048 + table_z[L], the
;; zone's temp unit = z * 2048 + table_z[0x7FD] (core ftl_tbl). Unit u = blocks 2u, 2u + 1 (two
;; planes); superpage sp (0..127, 4 sectors) = block 2u + (sp & 1), page sp >> 1.
;; Stock UD_SF641 algorithm: the open unit's sectors go to the same offsets of the temp unit,
;; superpages in ascending order, skipped ones copied from the old unit (copy-back); close = copy
;; the rest, erase the old unit, copy the temp unit back into it, erase the temp unit. The map
;; never changes.
;; NAND cycles mirror UD_SF641.DRV: program 0x37C9, copy-back 0x3924/0x39DE, erase 0x3785, ready
;; wait 0x39FB; select/release as BREC 0x3D3A/0x3D5F (interrupts off while CE1 is selected).
;; Entry points: SDCC __sdcccall(1) (8-bit arg in A, 16-bit in HL, 32-bit in HL:DE = high:low,
;; further args on the stack, popped by the callee; result in A; IX preserved).
;; Options (define before the .include):
;;   FTLW_IDLE = 1  call void ftlw_idle(void) (module C code) in every NAND busy-wait poll, with
;;                  CE1 selected and interrupts off: it may touch ports and RAM below 0x8000 (e.g.
;;                  drain the ADC FIFO, serve the DSP), never the external window. REC uses it.
;;   FTLW_FW = 1    also assemble ftlw_fwwrite(): FWIMAGE copy A (firmware logical units 0..0x1F,
;;                  zone 0) for the FIRMWARE update. Nothing else can reach units 0..0x40.
;;   FTLW_BREC = 1  also assemble the raw boot-record path ftlw_brec_erase/prog (FIRMWARE module):
;;                  physical blocks 0 and 1 only, outside the FTL (NAND_AND_BOOT.md 5).
	.include "regs.inc"
	.globl	_nand_read, _ftl_tbl, _hal_watchdog
.ifdef FTLW_IDLE
	.globl	_ftlw_idle
.endif

;; ftlw.h result codes
FTLW_OK		= 0
FTLW_ERANGE	= 1
FTLW_EMAP	= 2
FTLW_EDIRTY	= 3
FTLW_EFAIL	= 4
FTLW_ETIMEOUT	= 5
FTLW_EWP	= 6
;; FTL geometry (BREC 0x16FB, nand_ftl.md 2.1-2.4)
UPZ		= 2048		; physical units per zone
LUNITS		= 0x7B0		; logical units per zone
TEMP_ENTRY	= 0x7FD		; table entry of the zone's temp unit
FW_UNITS	= 0x40		; logical units 0..0x3F firmware, 0x40 VM, FAT volume from 0x41
FW_A_SECTORS	= 0x4000	; FWIMAGE copy A = firmware units 0..0x1F (B = 0x20..0x3F: BREC's)
SUPERPAGES	= 128		; per unit
SP_MARK		= 0x55		; spare byte 14 of every written sector (UD 0x3884)
REQ_BUF		= 5		; nand_req_t: c0 c1 r0 r1 r2, u16 buf, u16 len
REQ_LEN		= 7

	.area	_DATA
o_lu:	.ds	2		; open logical unit (global) + 1 ("key"), 0 = none
o_pu:	.ds	2		; its physical unit (global)
o_tu:	.ds	2		; the zone's temp unit (global)
o_sp:	.ds	1		; superpage being filled in the temp unit
o_mask:	.ds	1		; sectors of o_sp already programmed
w_err:	.ds	1		; sticky error
w_sp:	.ds	1		; current write: superpage, sector-in-page, its mask bit
w_col:	.ds	1
w_bit:	.ds	1
nw_l:	.ds	2		; spare bytes 2-3: zone-relative logical unit L
nw_a:	.ds	9		; address cycles (col lo, col hi, row 0-2) + nand_read buf/len
nw_b:	.ds	5		; copy-back target address cycles
sw:	.ds	4		; spare bytes 0-3 read back
nw_s:	.ds	3		; IFF, P_EMLO, P_EMHI before the selection
nw_c:	.ds	2		; random data input column
.ifdef FTLW_BREC
_ftlw_brec_ecc::
	.ds	4		; ECC bytes (spare +8..+11) of the last nw_prog
.endif

	.area	_CODE
;; IDLE: with FTLW_IDLE, call the module hook between the core reads of open() (mapping table,
;; spares: ~30 k T-states together), all registers kept; otherwise nothing
	.macro	IDLE
.ifdef FTLW_IDLE
	call	idle_keep
.endif
	.endm
.ifdef FTLW_IDLE
idle_keep:
	push	af
	push	bc
	push	de
	push	hl
	call	_ftlw_idle
	pop	hl
	pop	de
	pop	bc
	pop	af
	ret
.endif

;; ---- addresses -----------------------------------------------------------------------------
;; nw_a = column 0 of superpage A of unit HL (BREC 0x3F1B):
;; row0 = u0 << 7 | sp0 << 6 | sp >> 1, row1 = (u >> 1) & 0xFF, row2 = u >> 9
addr:
	ld	c, a
	xor	a, a
	ld	(nw_a), a
	ld	(nw_a+1), a
	srl	h
	rr	l			; HL = u >> 1, carry = u bit 0
	rra
	srl	c			; C = sp >> 1, carry = sp bit 0
	jr	nc, 1$
	or	a, #NAND_ROW0_PLANE
1$:	or	a, c
	ld	(nw_a+2), a
	ld	(nw_a+3), hl
	ret

;; spare bytes 0-3 of superpage A of unit HL -> sw (core nand_read)
rdspare:
	call	addr
	ld	a, #(NAND_SPARE >> 8)
	ld	(nw_a+1), a
	ld	hl, #sw
	ld	(nw_a+REQ_BUF), hl
	ld	hl, #4
	ld	(nw_a+REQ_LEN), hl
	ld	hl, #nw_a
	jp	_nand_read

;; Z if sw = FF FF FF FF
allff:
	ld	hl, #sw
	ld	a, (hl)
	inc	hl
	and	a, (hl)
	inc	hl
	and	a, (hl)
	inc	hl
	and	a, (hl)
	inc	a
	ret

;; key (logical unit + 1) of FAT volume sector HL:DE (needs lba < 2^23: H = 0, L < 0x80) -> HL
lu_of:
	ld	h, l
	ld	l, d
	srl	h
	rr	l			; lba >> 9
	ld	bc, #FW_UNITS + 1 + 1
	add	hl, bc
	ret

;; ---- NAND: select / release / cycles / ready -----------------------------------------------
nw_sel:
	ld	a, i			; P/V = IFF2
	jp	pe, 9$
	ld	a, i			; Z80 erratum: an IRQ accepted during LD A,I reads P/V = 0
9$:	di
	ld	a, #0
	jp	po, 1$
	inc	a
1$:	ld	(nw_s), a
	in	a, (P_EMLO)
	ld	(nw_s+1), a
	in	a, (P_EMHI)
	ld	(nw_s+2), a
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

;; release; A = C (result)
nw_rel:
	ld	a, #NAND_STOP
	out	(P_NAND_CMD), a
	xor	a, a
	out	(P_NAND_EN), a
	out	(P_NAND_CEMODE), a
	ld	a, (nw_s+2)
	out	(P_EMHI), a
	ld	a, (nw_s+1)
	out	(P_EMLO), a
	ld	a, (nw_s)
	or	a, a
	ld	a, c
	ret	z
	ei
	ret

;; CLE cycle B, then C address cycles from (HL) (UD 0x37AF). DE preserved.
nw_cyc:
	ld	a, #EM_DATA
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

;; random data input at column D:E (E = low byte). DE preserved.
nw_rnd:
	ld	hl, #nw_c
	ld	(hl), e
	inc	hl
	ld	(hl), d
	dec	hl
	ld	bc, #(NAND_RNDIN << 8) | 2
	jr	nw_cyc

;; ready wait + status (UD 0x39FB) -> C = FTLW_OK / FTLW_ETIMEOUT / FTLW_EFAIL / FTLW_EWP
nw_wait:
	ld	b, #0x60		; >= tWB (and tR after a read): ~50 us at 24 MHz
1$:	djnz	1$
	ld	a, #NAND_STATUS
	out	(P_NAND_CMD), a
	ld	de, #0x7000		; timeout (as UD 0x39FB)
2$:
.ifdef FTLW_IDLE
	push	de
	call	_ftlw_idle		; module hook: ports + RAM < 0x8000 only (see top)
	pop	de
.endif
	ld	a, (EXT_WINDOW)
	ld	b, a
	and	a, #NAND_ST_READY
	jr	nz, 3$
	dec	de
	ld	a, d
	or	a, e
	jr	nz, 2$
	ld	a, #NAND_RESET
	out	(P_NAND_CMD), a
	ld	c, #FTLW_ETIMEOUT
	ret
3$:	ld	c, #FTLW_EFAIL
	ld	a, b
	and	a, #NAND_ST_FAIL
	ret	nz
	ld	c, #FTLW_EWP
	ld	a, b
	and	a, #NAND_ST_NWP
	ret	z
	ld	c, #FTLW_OK
	ret

;; DMA1 ports SA2 .. CMD for a 512-byte ZRAM1 -> NAND data transfer (BREC table 0x3DBC order)
nw_dma:	.db	0, DMA_A3_ZRAM1, 0		; SA2, SA3, ISA
	.db	0, 0, 0, DMA_A3_NAND, 0		; DA0-DA2 (A16/A15 = 0: data cycles), DA3, IDA
	.db	0xFF, 0x01			; count - 1 = 0x1FF
	.db	DMA_MODE_WS3, DMA_CMD_START	; mode, start
NW_DMA_N = 12

;; program one sector, HL = buf (ZRAM1): address nw_a (column = sector-in-page * 512), 512 data
;; bytes by DMA1 with the ECC engine running, then the 16 spare bytes
;; FF FF L_lo L_hi FF FF FF FF ECC x4 FF FF 55 FF (L = nw_l) at column 0x800 + 16 * sector-in-page,
;; program confirm, status (UD 0x37C9) -> A
nw_prog:
	push	hl
	call	nw_sel
	ld	hl, #nw_a
	ld	bc, #(NAND_PROG1 << 8) | 5
	call	nw_cyc
	ld	a, #ECC_START
	out	(P_ECC_CTL), a
	pop	hl
	ld	a, l
	out	(P_DMA1_SA0), a
	ld	a, h
	out	(P_DMA1_SA1), a
	ld	hl, #nw_dma
	ld	bc, #(NW_DMA_N << 8) | P_DMA1_SA2
3$:	ld	a, (hl)
	out	(c), a
	inc	hl
	inc	c
	djnz	3$
4$:	in	a, (P_DMA1_CMD)		; the CPU is halted until done; be sure
	and	a, #DMA_CMD_START
	jr	nz, 4$
	ld	a, (nw_a+1)		; column high = 2 * sector-in-page
	add	a, a
	add	a, a
	add	a, a
	ld	e, a			; spare column low = 16 * sector-in-page
	ld	d, #(NAND_SPARE >> 8)
	call	nw_rnd
	ld	hl, #EXT_WINDOW
	ld	a, #0xFF
	ld	(hl), a			; +0 good block
	ld	(hl), a			; +1 data unit
	ld	a, (nw_l)
	ld	(hl), a			; +2/+3 logical unit number (zone relative)
	ld	a, (nw_l+1)
	ld	(hl), a
	ld	a, #0xFF
	ld	(hl), a			; +4..+7
	ld	(hl), a
	ld	(hl), a
	ld	(hl), a
5$:	in	a, (P_ECC_CTL)
	and	a, #ECC_BUSY
	jr	nz, 5$
	in	a, (P_ECC_SP8)		; +8..+11 hardware ECC
	ld	(hl), a
.ifdef FTLW_BREC
	ld	(_ftlw_brec_ecc+0), a
.endif
	in	a, (P_ECC_SP9)
	ld	(hl), a
.ifdef FTLW_BREC
	ld	(_ftlw_brec_ecc+1), a
.endif
	in	a, (P_ECC_SP10)
	ld	(hl), a
.ifdef FTLW_BREC
	ld	(_ftlw_brec_ecc+2), a
.endif
	in	a, (P_ECC_SP11)
	ld	(hl), a
.ifdef FTLW_BREC
	ld	(_ftlw_brec_ecc+3), a
.endif
	ld	a, #0xFF
	ld	(hl), a			; +12, +13
	ld	(hl), a
	ld	(hl), #SP_MARK		; +14
	ld	(hl), a			; +15
	ld	a, #NAND_PROG2
	out	(P_NAND_CMD), a
	call	nw_wait
	jp	nw_rel

;; copy-back the page at nw_a into the page at nw_b (same plane), A = skip: the sectors whose
;; bit is set (already programmed in the target) become FF in the page register -> A
nw_copy:
	push	af
	call	nw_sel
	ld	hl, #nw_a
	ld	bc, #(NAND_READ1 << 8) | 5
	call	nw_cyc
	ld	bc, #(NAND_CBREAD << 8) | 0
	call	nw_cyc
	call	nw_wait
	pop	de			; D = skip
	ld	a, c
	or	a, a
	jp	nz, nw_rel
	push	de
	ld	hl, #nw_b
	ld	bc, #(NAND_RNDIN << 8) | 5
	call	nw_cyc
	pop	de
	ld	e, #0			; E = column high of sector g = 2 * g
6$:	srl	d
	jr	nc, 7$
	push	de
	ld	d, e
	ld	e, #0
	call	nw_rnd			; data column (0, 2g): 512 x FF
	ld	a, #0xFF
	ld	b, #0
8$:	ld	(EXT_WINDOW), a
	ld	(EXT_WINDOW), a
	djnz	8$
	pop	de
	push	de
	ld	a, e
	add	a, a
	add	a, a
	add	a, a
	ld	e, a
	ld	d, #(NAND_SPARE >> 8)
	call	nw_rnd			; spare column 0x800 + 16g: 16 x FF
	ld	a, #0xFF
	ld	b, #16
9$:	ld	(EXT_WINDOW), a
	djnz	9$
	pop	de
7$:	inc	e
	inc	e
	ld	a, e
	cp	a, #8
	jr	nz, 6$
	ld	a, #NAND_PROG2
	out	(P_NAND_CMD), a
	call	nw_wait
	jp	nw_rel

;; erase both blocks (planes) of unit HL (two-plane erase, UD 0x3785) -> A
erase:
	xor	a, a
	call	addr
	call	nw_sel
	ld	hl, #nw_a+2
	ld	bc, #(NAND_ERASE1 << 8) | 3
	call	nw_cyc
	ld	hl, #nw_a+2
	ld	a, (hl)
	or	a, #NAND_ROW0_PLANE
	ld	(hl), a
	ld	bc, #(NAND_ERASE1 << 8) | 3
	call	nw_cyc
	ld	a, #NAND_ERASE2
	out	(P_NAND_CMD), a
	call	nw_wait
	jp	nw_rel

;; ---- FTL ---------------------------------------------------------------------------------
;; copy-back superpage o_sp. B = 0: old unit -> temp unit, sectors in E skipped;
;; B = 1: temp unit -> old unit (E = 0) -> A
copy:
	push	de
	ld	hl, (o_tu)
	ld	de, (o_pu)
	dec	b
	jr	nz, 1$			; B = 0: target temp, source old
	ex	de, hl			; B = 1: target old, source temp
1$:	push	de
	ld	a, (o_sp)
	call	addr			; target
	ld	hl, #nw_a
	ld	de, #nw_b
	ld	bc, #5
	ldir
	pop	hl
	ld	a, (o_sp)
	call	addr			; source
	pop	de
	ld	a, e
	jp	nw_copy

;; copy the old unit's superpages o_sp .. A-1 into the temp unit (o_sp minus o_mask) -> A
fill:
	ld	hl, #o_sp
	cp	a, (hl)
	jr	z, 9$
	jr	c, 9$
	push	af
	ld	a, (o_mask)
	cp	a, #0x0F		; all 4 sectors written: nothing to copy (NOP limit)
	jr	z, 2$
	ld	e, a
	ld	b, #0
	call	copy
	or	a, a
	jr	nz, 8$
2$:	ld	hl, #o_sp
	inc	(hl)
	xor	a, a
	ld	(o_mask), a
	call	_hal_watchdog
	pop	af
	jr	fill
8$:	pop	bc
	ret
9$:	xor	a, a
	ret

;; errors are sticky. fail_old: the old unit is intact -> drop the temp copy (best effort)
fail_old:
	push	af
	ld	hl, (o_tu)
	call	erase
	pop	af
fail:
	ld	(w_err), a
	ld	hl, #0
	ld	(o_lu), hl
	ret

;; open key HL = logical unit + 1 (checks the table entries, the data unit's spare, the temp
;; unit) -> A
open:
	push	hl
	dec	hl			; logical unit
	ld	b, #0			; zone
	ld	de, #LUNITS
1$:	or	a, a
	sbc	hl, de
	jr	c, 2$
	inc	b
	jr	1$
2$:	add	hl, de
	ld	(nw_l), hl		; L
	push	bc
	ex	de, hl
	ld	a, b
	IDLE
	call	_ftl_tbl		; A = zone, DE = entry -> DE
	ld	(o_pu), de
	pop	bc
	push	bc
	ld	a, b
	ld	de, #TEMP_ENTRY
	IDLE
	call	_ftl_tbl
	ld	(o_tu), de
	pop	bc
	ld	hl, (o_pu)
	ld	a, h
	cp	a, #(UPZ >> 8)
	jr	nc, 8$			; unmapped (FFFF) / no such zone
	ld	a, d
	cp	a, #(UPZ >> 8)
	jr	nc, 8$
	or	a, a
	sbc	hl, de
	jr	z, 8$
	ld	a, b			; + zone * UPZ
	add	a, a
	add	a, a
	add	a, a
	ld	c, a
	ld	hl, #o_pu+1
	ld	a, (hl)
	add	a, c
	ld	(hl), a
	ld	hl, #o_tu+1
	ld	a, (hl)
	add	a, c
	ld	(hl), a
	ld	hl, (o_pu)		; data unit: good block, data unit, logical number L or FFFF
	xor	a, a
	IDLE
	call	rdspare
	ld	hl, (sw)
	ld	a, h
	and	a, l
	inc	a
	jr	nz, 8$
	ld	hl, (sw+2)
	ld	a, h
	and	a, l
	inc	a
	jr	z, 3$			; never written
	ld	de, (nw_l)
	or	a, a
	sbc	hl, de
	jr	nz, 8$
3$:	ld	hl, (o_tu)		; temp unit erased: first page of both planes
	xor	a, a
	IDLE
	call	rdspare
	call	allff
	jr	nz, 7$
	ld	hl, (o_tu)
	ld	a, #1
	IDLE
	call	rdspare
	call	allff
	jr	nz, 7$
	xor	a, a
	ld	(o_sp), a
	ld	(o_mask), a
	pop	hl
	ld	(o_lu), hl
	ret
7$:	ld	a, #FTLW_EDIRTY
	jr	9$
8$:	ld	a, #FTLW_EMAP
9$:	pop	hl
	ret

;; ---- API (ftlw.h) --------------------------------------------------------------------------
;; uint8_t ftlw_flush(void)
_ftlw_flush::
	ld	a, (w_err)
	or	a, a
	ret	nz
	ld	hl, (o_lu)
	ld	a, h
	or	a, l
	ret	z
	ld	a, #SUPERPAGES
	call	fill
	or	a, a
	jp	nz, fail_old
	ld	hl, (o_pu)		; from here the temp unit holds the only complete copy
	call	erase
	or	a, a
	jp	nz, fail
	ld	(o_sp), a
1$:	ld	b, #1
	ld	e, #0
	call	copy
	or	a, a
	jp	nz, fail
	call	_hal_watchdog
	ld	hl, #o_sp
	inc	(hl)
	ld	a, (hl)
	cp	a, #SUPERPAGES
	jr	c, 1$
	ld	hl, (o_tu)
	call	erase
	or	a, a
	jp	nz, fail
	ld	h, a
	ld	l, a
	ld	(o_lu), hl
	ret

;; uint8_t ftlw_write(uint32_t lba, const void *buf512)     HL:DE = lba, buf on the stack
_ftlw_write::
	ld	a, (w_err)
	or	a, a
	jp	nz, wr_out
	ld	a, #FTLW_ERANGE
	inc	h
	dec	h
	jp	nz, wr_out
	bit	7, l			; lba >= 2^23: unit numbers would wrap
	jp	nz, wr_out
	call	lu_of			; never the firmware / VM units (DE kept)
;; HL = key (logical unit + 1), DE = sector bits 0-15 (bits 0-8 = sector in the unit)
wr_key:
	push	hl
	ld	a, e			; sector in page, its bit
	and	a, #3
	ld	(w_col), a
	ld	b, a
	ld	a, #1
	jr	z, 2$
1$:	add	a, a
	djnz	1$
2$:	ld	(w_bit), a
	ld	a, d			; superpage = (lba & 511) >> 2
	rra
	ld	a, e
	rra
	srl	a
	ld	(w_sp), a
	pop	de			; DE = key
	ld	hl, (o_lu)
	or	a, a
	sbc	hl, de
	jr	nz, 5$			; another unit
	ld	a, (o_sp)
	ld	c, a
	ld	a, (w_sp)
	cp	a, c
	jr	c, 5$			; an earlier superpage
	jr	nz, 6$
	ld	a, (o_mask)
	ld	hl, #w_bit
	and	a, (hl)
	jr	z, 6$			; same superpage, sector not yet written
5$:	push	de
	call	_ftlw_flush
	pop	hl
	or	a, a
	jp	nz, wr_out
	call	open
	or	a, a
	jp	nz, wr_out
6$:	ld	a, (w_sp)
	call	fill
	or	a, a
	jp	nz, wr_fail
	ld	hl, (o_tu)
	ld	a, (w_sp)
	call	addr
	ld	a, (w_col)
	add	a, a
	ld	(nw_a+1), a		; column = sector-in-page * 512
	pop	bc
	pop	hl			; buf
	push	hl
	push	bc
	call	nw_prog
	or	a, a
	jp	nz, wr_fail
	ld	a, (w_bit)
	ld	hl, #o_mask
	or	a, (hl)
	ld	(hl), a
	xor	a, a
	jr	wr_out
wr_fail:
	call	fail_old
wr_out:
	pop	hl
	pop	bc
	jp	(hl)

.ifdef FTLW_FW
;; uint8_t ftlw_fwwrite(uint32_t sector, const void *buf512)   HL:DE = sector, buf on the stack
;; FWIMAGE copy A ONLY: sector < FW_A_SECTORS = firmware logical units 0..0x1F of zone 0 (same
;; FTL algorithm as ftlw_write). Copy B (units 0x20..0x3F) is BREC's: it refreshes it from A at
;; the next boot. Any other sector -> FTLW_ERANGE, nothing written.
_ftlw_fwwrite::
	ld	a, (w_err)
	or	a, a
	jp	nz, wr_out
	ld	a, h
	or	a, l
	ld	a, #FTLW_ERANGE
	jp	nz, wr_out
	ld	a, d
	cp	a, #(FW_A_SECTORS >> 8)
	ld	a, #FTLW_ERANGE
	jp	nc, wr_out
	ld	a, d
	srl	a			; unit = sector >> 9 (0..0x1F)
	inc	a			; key = unit + 1
	ld	l, a
	ld	h, #0
	jp	wr_key
.endif

.ifdef FTLW_BREC
;; ---- boot record (BREC) raw path: physical blocks 0 and 1 ONLY -----------------------------
;; The FTL never uses blocks 0/1 (unit 0); these entries reach nothing else: any other block
;; number -> FTLW_ERANGE, nothing sent to the chip. Format as stored by the vendor tools (dump):
;; pages 0-7 = the 16 KB, each 512-byte sector with spare FF x8, ECC x4, FF FF 55 FF.
;; uint8_t ftlw_brec_erase(uint8_t block)       A = 0 / 1: single-block erase (60 row D0)
_ftlw_brec_erase::
	cp	a, #2
	jr	c, 1$
	ld	a, #FTLW_ERANGE
	ret
1$:	rrca
	rrca				; block bit 0 -> row0 bit 6 (NAND_ROW0_PLANE)
	ld	(nw_a+2), a
	xor	a, a
	ld	(nw_a+3), a
	ld	(nw_a+4), a
	call	nw_sel
	ld	hl, #nw_a+2
	ld	bc, #(NAND_ERASE1 << 8) | 3
	call	nw_cyc
	ld	a, #NAND_ERASE2
	out	(P_NAND_CMD), a
	call	nw_wait
	jp	nw_rel

;; uint8_t ftlw_brec_prog(uint16_t bps, const void *buf512)
;; HL = block << 5 | page << 2 | sector (block 0/1, page 0-7, sector 0-3), DE = buf (ZRAM1):
;; one 512-byte sector + its 16 spare bytes (nw_prog with L = FFFF); ECC -> ftlw_brec_ecc
_ftlw_brec_prog::
	ld	a, h
	or	a, a
	jr	nz, 8$
	ld	a, l
	cp	a, #64
	jr	nc, 8$
	and	a, #3
	add	a, a
	ld	(nw_a+1), a		; column high = 2 * sector (column = sector * 512)
	ld	a, l
	rra
	rra
	and	a, #7			; page
	bit	5, l
	jr	z, 1$
	or	a, #NAND_ROW0_PLANE	; block 1
1$:	ld	(nw_a+2), a
	xor	a, a
	ld	(nw_a), a
	ld	(nw_a+3), a
	ld	(nw_a+4), a
	ld	hl, #0xFFFF
	ld	(nw_l), hl
	ex	de, hl
	jp	nw_prog
8$:	ld	a, #FTLW_ERANGE
	ret
.endif

;; uint8_t ftlw_sync(uint32_t lba, uint16_t n)   HL:DE = lba, n on the stack
;; flush if the open unit is one of lu_of(lba) .. lu_of(lba) + n / 512 + 1 (conservative)
_ftlw_sync::
	call	lu_of
	ex	de, hl
	ld	hl, (o_lu)		; 0 (closed) - lu: large
	or	a, a
	sbc	hl, de
	pop	bc
	pop	de			; n
	push	bc
	srl	d
	inc	d			; n / 512 + 1
	ld	a, h
	or	a, a
	jr	nz, 1$
	ld	a, d
	cp	a, l
	jp	nc, _ftlw_flush
1$:	xor	a, a
	ret

;; uint8_t ftlw_pending(void)
_ftlw_pending::
	ld	hl, (o_lu)
	ld	a, h
	or	a, l
	ret	z
	ld	a, #1
	ret

;; uint8_t ftlw_error(void)
_ftlw_error::
	ld	a, (w_err)
	ret

;; void ftlw_reset(void)
_ftlw_reset::
	xor	a, a
	ld	(w_err), a
	ret
