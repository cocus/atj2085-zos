;; zosboot - stage-0 boot selector that replaces SYSCFG.SYS in a patched ATJ2085 firmware.
;; (c) zos project, our own code. Assemble with SDCC's sdasz80 (see ../build_stub.py).
;;
;; The boot record (BREC) loads SYSCFG.SYS exactly like the stock kernel: file 0x0000-0x2CFF
;; -> RAM 0x0600-0x32FF and file 0x3700-0x377F -> RAM 0x0000-0x007F, then `ld sp,0x0400 ;
;; jp 0x0600` with interrupts off. The stock kernel is kept in the firmware as SYSCFG.STK.
;;
;;   RAM 0x0000-0x007F  vector page (_VEC)
;;   RAM 0x0600-0x07FF  entry jump + parameter block (_ENTRY, filled in by zospatch.py)
;;   RAM 0x0800-0x2BFF  free: the zos core (ZOS.AP text) is loaded here from the user disk
;;   RAM 0x2C00-0x30FF  selector code + variables (_MAIN)
;;   RAM 0x3100-0x32FF  sector buffer
;;
;; Decision (DEFAULT = STOCK; zos only when everything checks out):
;;   1. BREC's loader is the known one and still looks for "SYSCFG  SYS"  (else: stock is not
;;      reachable - only then zos is tried without asking, and a watchdog reset if it fails)
;;   2. [F_VBUS] USB cable present                                -> stock
;;   3. [F_KEYS] MENU or VOL held (after the key GPIO set-up)      -> stock
;;   4. BREC's flash reader is the known one (code sums)          else stock
;;   5. user disk: FAT16 boot sector, root directory contains ZOS.AP (8.3 name from the
;;      parameter block)                                          else stock
;;   6. ZOS.AP header: ACTOS AP magic, one text segment at file 0x800 linked at 0x0800,
;;      1..0x2400 bytes, entry 0x0800, file size covers it; FAT chain intact; every sector
;;      readable inside the reader's reach; image starts with `jp`  else stock
;;   7. start zos (takeover: RTC/watchdog/DC-DC/keys set up as the stock OS would, jp 0x0800)
;; stock = re-enter BREC's own loader with the name it looks for patched to "SYSCFG  STK", so
;; the untouched stock kernel is loaded and started by the identical code path as on a stock
;; boot. Nothing here writes to the flash.
	.module zosboot
	.include "zosboot.inc"

;; ======================================================================== vector page
;; 0x0037 must stay non-zero (0x76): BREC's flash wrapper counts nesting there and executes
;; EI when it drops to 0. Jumps to 0 / RST n reboot via the watchdog; IM1 returns until zos
;; installs its own handler.
	.area	_VEC (ABS)
	.org	0x0000
vec_reset::
	di
	ld	a, #WDT_REBOOT
	out	(P_WDT), a
1$:	jr	1$
	.ds	0x08 - 7
	jp	vec_reset
	.ds	0x10 - 0x0B
	jp	vec_reset
	.ds	0x18 - 0x13
	jp	vec_reset
	.ds	0x20 - 0x1B
	jp	vec_reset
	.ds	0x28 - 0x23
	jp	vec_reset
	.ds	0x30 - 0x2B
	jp	vec_reset
	.ds	0x38 - 0x33
	ei
	reti
	.ds	0x66 - 0x3B
	retn

;; ======================================================================== entry + parameters
	.area	_ENTRY (ABS)
	.org	0x0600
entry::
	jp	main
;; ---- parameter block: layout fixed (zospatch.py writes it at file offset 3)
p_magic::	.ascii	"ZOSBOOT1"		; +0x03
p_flags::	.db	F_KEYS | F_VBUS		; +0x0B
p_ver::		.db	1			; +0x0C stub version
p_volx::	.dw	0x0000, 0x0104		; +0x0D FAT volume start, byte offset in BREC reader space
p_ldsig::	.dw	0			; +0x11 16-bit sum of the BREC loader ranges
p_rdsig::	.dw	0			; +0x13 16-bit sum of the BREC reader ranges
p_zname::	.ascii	"ZOS     AP "		; +0x15 zos core file (root directory, 8.3)
p_stk::		.ascii	"SYSCFG  STK"		; +0x20 directory name of the stock kernel copy
p_sys::		.ascii	"SYSCFG  SYS"		; +0x2B what BREC's loader looks for originally
p_end::

;; ======================================================================== selector
	.area	_MAIN (ABS)
	.org	0x2C00
main::
	ld	(sv_bc), bc			; BREC's registers, restored for the stock kernel
	ld	(sv_de), de			; (no push: SP = 0x0400 sits on BREC code)
	ld	sp, #BREC_LOAD_SP
	call	loader_ok
	jr	z, m_ok
	;; stock is not reachable (unknown BREC, or a chain-load already failed): zos or reboot
	call	try_zos
reboot::
	di
	ld	a, #WDT_REBOOT
	out	(P_WDT), a
m_hang:	jr	m_hang

m_ok:	call	keys_gpio			; GPIO first: out of reset the key pins are KEYI/KEYO
	ld	a, (p_flags)
	and	a, #F_VBUS
	jr	z, m_keys
	in	a, (P_USB_STAT)
	and	a, #USBST_VBUS
	jr	nz, go_stock
m_keys:	ld	a, (p_flags)
	and	a, #F_KEYS
	jr	z, m_zos
	call	menu_held
	jr	c, go_stock
m_zos:	call	try_zos				; returns only when zos can not be started
	;; fall through

;; ------------------------------------------------------------------ chain-load the stock kernel
go_stock::
	ld	hl, #BREC_NAMECMP + 1
	ld	de, #p_stk
	ld	b, #11
gs_1:	ld	a, (de)
	ld	(hl), a
	inc	de
	ld	a, l
	add	a, #BREC_NAMESTRIDE
	ld	l, a				; (0x0275..0x02C5: no carry into H)
	djnz	gs_1
	ld	bc, (sv_bc)
	ld	de, (sv_de)
	ld	sp, #BREC_LOAD_SP
	jp	BREC_LOADSYS

;; ------------------------------------------------------------------ start zos
go_zos::
	di
	im	1
	ld	a, #LOSCDIV_STOCK_L		; RTC: 10 ms + 2 Hz timers as the stock kernel
	out	(P_LOSCDIV_L), a
	ld	a, #LOSCDIV_STOCK_H
	out	(P_LOSCDIV_H), a
	ld	a, #LOSCDIV_STOCK_M
	out	(P_LOSCDIV_M), a
	in	a, (P_RTCCTL)
	or	a, #RTCCTL_TIMERS
	out	(P_RTCCTL), a
	in	a, (P_WDT)			; watchdog period + clear (enable left as BREC set it)
	or	a, #WDT_CLR
	out	(P_WDT), a
	in	a, (P_WDT)
	and	a, #0xFF - WDT_PERIOD_MASK
	or	a, #WDT_PERIOD_MAIN
	out	(P_WDT), a
	ld	a, #DCDC_STOCK
	out	(P_DCDC), a
	call	keys_gpio
	in	a, (P_GPC_EN)			; I2C lines released to inputs (stock main-menu state)
	and	a, #0xFF - (GPC_SCL | GPC_SDA)
	out	(P_GPC_EN), a
	ld	sp, #ZOS_STACK
	jp	ZOS_BASE

;; ------------------------------------------------------------------ BREC checks
;; Z = BREC's loader is the known one (code sums) and still compares against "SYSCFG  SYS"
loader_ok:
	ld	hl, #BREC_NAMECMP
	ld	de, #p_sys
	call	name_cmp
	ret	nz
	ld	ix, #ld_ranges
	ld	hl, (p_ldsig)
	jr	sig_cmp

;; Z = BREC's flash reader is the known one and its zone size is sane
reader_ok:
	ld	ix, #rd_ranges
	ld	hl, (p_rdsig)
	call	sig_cmp
	ret	nz
	ld	hl, (BREC_UPZ)
	ld	a, h
	or	a, l
	jr	z, nz_ret
	ld	a, h
	cp	a, #0x09			; <= 0x8FF units per zone
	jr	nc, nz_ret
	xor	a, a
	ret
nz_ret:	or	a, #1
	ret

;; Z = sum16 over the {start, length} list at IX (0-length ends it) equals HL
sig_cmp:
	push	hl
	ld	de, #0
sc_1:	ld	l, 0(ix)
	ld	h, 1(ix)
	ld	c, 2(ix)
	ld	b, 3(ix)
	ld	a, b
	or	a, c
	jr	z, sc_2
sc_3:	ld	a, (hl)				; DE += BC bytes at HL
	add	a, e
	ld	e, a
	jr	nc, sc_4
	inc	d
sc_4:	inc	hl
	dec	bc
	ld	a, b
	or	a, c
	jr	nz, sc_3
	inc	ix
	inc	ix
	inc	ix
	inc	ix
	jr	sc_1
sc_2:	pop	hl
	or	a, a
	sbc	hl, de
	ret

ld_ranges:					; BREC loader code 0x020C-0x0273, 0x02C9-0x037A,
	.dw	0x020C, 0x0224 - 0x020C		; minus the operands of its calls to small helpers
	.dw	0x0226, 0x0230 - 0x0226		; (they sit at other addresses in the F644/F321
	.dw	0x0232, 0x0274 - 0x0232		; boot records; the load itself is identical)
	.dw	0x02C9, 0x02D7 - 0x02C9
	.dw	0x02D9, 0x0324 - 0x02D9
	.dw	0x0326, 0x0349 - 0x0326
	.dw	0x034B, 0x036E - 0x034B
	.dw	0x0370, 0x037B - 0x0370
	.dw	0, 0
rd_ranges:					; BREC flash reader code (variables excluded)
	.dw	0x3CAF, 0x3D34 - 0x3CAF
	.dw	0x3D3A, 0x3DB1 - 0x3D3A
	.dw	0x3DC9, 0x3F40 - 0x3DC9
	.dw	0x3F69, 0x3FCE - 0x3F69
	.dw	0x3FDB, 3
	.dw	0, 0

;; Z = the 11 `cp #c` instructions at HL compare against the 11 chars at DE
name_cmp:
	ld	b, #11
nc_1:	ld	a, (hl)
	cp	a, #Z80_CP_N
	ret	nz
	inc	hl
	ld	a, (de)
	cp	a, (hl)
	ret	nz
	inc	de
	ld	a, l
	add	a, #BREC_NAMESTRIDE - 1
	ld	l, a
	djnz	nc_1
	xor	a, a
	ret

;; ------------------------------------------------------------------ keys
;; GPIO B/G as the stock key driver initialises them: keys read by software
keys_gpio:
	xor	a, a
	out	(P_GPB_CFG), a
	out	(P_KEYSCAN), a
	out	(P_GPB_D), a
	out	(P_GPB_OE), a
	ld	a, #GPB_IE_STOCK
	out	(P_GPB_IE), a
	ld	a, #GPG_G0_IE
	out	(P_GPG_EN), a
	ret

;; CY = MENU or VOL held: 16 samples ~1.6 ms apart, at least 12 pressed
menu_held:
	in	a, (P_GPB_OE)
	ld	(sv_oe), a
	and	a, #0xFF - KEYB_ROWS
	out	(P_GPB_OE), a
	in	a, (P_GPB_IE)
	ld	(sv_ie), a
	or	a, #KEYB_ROWS
	out	(P_GPB_IE), a
	ld	c, #0
	ld	e, #16
mh_1:	ld	hl, #1500
mh_2:	dec	hl
	ld	a, h
	or	a, l
	jr	nz, mh_2
	in	a, (P_GPB_D)
	and	a, #KEYB_MENU | KEYB_VOL
	jr	z, mh_3
	inc	c
mh_3:	dec	e
	jr	nz, mh_1
	ld	a, (sv_oe)
	out	(P_GPB_OE), a
	ld	a, (sv_ie)
	out	(P_GPB_IE), a
	ld	a, c
	cp	a, #12
	ccf
	ret

;; ------------------------------------------------------------------ zos from the user disk
;; Loads ZOS.AP's text to 0x0800 and jumps to go_zos; returns on any failure.
try_zos::
	call	reader_ok
	ret	nz
	;; ---- boot sector (volume sector 0)
	ld	hl, #0
	call	lba16
	call	rdbuf
	ret	c
	ld	hl, (BUF + 0x1FE)
	ld	de, #0xAA55
	or	a, a
	sbc	hl, de
	ret	nz
	ld	hl, (BUF + 0x0B)		; bytes per sector
	ld	de, #0x0200
	or	a, a
	sbc	hl, de
	ret	nz
	ld	hl, #BUF + 0x36			; "FAT16" type string (FAT12/FAT32: not supported)
	ld	de, #s_fat16
	ld	b, #5
	call	memcmp
	ret	nz
	ld	a, (BUF + 0x0D)			; sectors per cluster: power of two
	ld	(spc), a
	or	a, a
	ret	z
	ld	c, #0
tz_sh:	rra
	jr	c, tz_sh2
	inc	c
	jr	tz_sh
tz_sh2:	or	a, a
	ret	nz
	ld	a, c
	ld	(spc_sh), a
	ld	hl, (BUF + 0x0E)		; reserved sectors = first FAT
	ld	(fat_lba), hl
	ld	a, (BUF + 0x10)			; number of FATs: 1 or 2
	or	a, a
	ret	z
	cp	a, #3
	ret	nc
	ld	b, a
	ld	de, (BUF + 0x16)		; sectors per FAT
	ld	a, d
	or	a, e
	ret	z
tz_f:	add	hl, de
	ret	c
	djnz	tz_f
	ld	(cur), hl			; root directory start
	ex	de, hl
	ld	hl, (BUF + 0x11)		; root entries
	ld	a, h
	or	a, l
	ret	z
	ld	bc, #15
	add	hl, bc
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	ld	(cnt), hl			; root directory sectors
	add	hl, de
	ret	c
	ld	(data_lba), hl
	;; ---- root directory: find the zos core file
tz_sec:	ld	hl, (cnt)
	ld	a, h
	or	a, l
	ret	z				; not found
	dec	hl
	ld	(cnt), hl
	ld	hl, (cur)
	push	hl
	inc	hl
	ld	(cur), hl
	pop	hl
	call	lba16
	call	rdbuf
	ret	c
	ld	hl, #BUF
	ld	b, #16
tz_ent:	ld	a, (hl)
	or	a, a
	ret	z				; end of directory: not found
	push	hl
	push	bc
	ld	de, #p_zname
	ld	b, #11
	call	memcmp
	pop	bc
	pop	hl
	jr	nz, tz_nxt
	push	hl
	pop	ix
	ld	a, 11(ix)			; attributes: not a directory / volume label / LFN
	and	a, #0x18
	jr	z, tz_hit
tz_nxt:	ld	de, #32
	add	hl, de
	djnz	tz_ent
	jr	tz_sec
tz_hit:	ld	a, 30(ix)			; size < 64 KB
	or	a, 31(ix)
	ret	nz
	ld	l, 28(ix)
	ld	h, 29(ix)
	ld	(zsize), hl
	ld	l, 26(ix)
	ld	h, 27(ix)
	ld	(clus), hl
	call	clus_ok
	ret	c
	;; ---- file sector 0: AP header
	call	clus_lba
	call	rdbuf
	ret	c
	ld	hl, #BUF
	ld	de, #ap_magic
	ld	b, #8
	call	memcmp
	ret	nz
	ld	de, #ZOS_BASE
	ld	hl, (BUF + 0x08)		; text file offset 0x00000800
	or	a, a
	sbc	hl, de
	ret	nz
	ld	hl, (BUF + 0x0A)
	ld	a, h
	or	a, l
	ret	nz
	ld	hl, (BUF + 0x0E)		; text load address
	or	a, a
	sbc	hl, de
	ret	nz
	ld	hl, (BUF + 0x1C)		; entry
	or	a, a
	sbc	hl, de
	ret	nz
	ld	hl, (BUF + 0x0C)		; text length 1..ZOS_MAXLEN
	ld	a, h
	or	a, l
	ret	z
	ld	(tlen), hl
	ld	de, #ZOS_MAXLEN + 1
	or	a, a
	sbc	hl, de
	ret	nc
	ld	hl, (tlen)			; file size >= 0x800 + length (not truncated)
	ld	de, #0x0800
	add	hl, de
	ex	de, hl
	ld	hl, (zsize)
	or	a, a
	sbc	hl, de
	ret	c
	ld	hl, (tlen)			; file sectors to walk = 4 + ceil(length / 512)
	ld	de, #511
	add	hl, de
	ld	a, h
	srl	a
	add	a, #4
	ld	(nfs), a
	ld	a, #1
	ld	(fs), a
	ld	(sic), a
	ld	hl, #ZOS_BASE
	ld	(dest), hl
	;; ---- text sectors -> 0x0800..
tz_ld:	ld	a, (nfs)
	ld	b, a
	ld	a, (fs)
	cp	a, b
	jr	nc, tz_done
	ld	a, (spc)
	ld	b, a
	ld	a, (sic)
	cp	a, b
	jr	c, tz_same
	call	fat_next
	ret	c
	xor	a, a
	ld	(sic), a
tz_same:
	ld	a, (fs)
	cp	a, #4
	jr	c, tz_skip
	call	clus_lba
	ld	a, (sic)
	call	lba_add_a
	ld	hl, (dest)
	call	rdsec
	ret	c
	ld	hl, (dest)
	inc	h
	inc	h
	ld	(dest), hl
tz_skip:
	ld	hl, #sic
	inc	(hl)
	ld	hl, #fs
	inc	(hl)
	jr	tz_ld
tz_done:
	ld	a, (ZOS_BASE)
	cp	a, #Z80_JP
	ret	nz
	jp	go_zos

;; CY = cluster (clus) out of range 2..0xFFEF
clus_ok:
	ld	hl, (clus)
	ld	de, #2
	or	a, a
	sbc	hl, de
	ret	c
	ld	de, #0xFFF0 - 2
	or	a, a
	sbc	hl, de
	ccf
	ret

;; clus = FAT16 successor of clus; CY = end of chain / bad entry / read error
fat_next:
	ld	hl, (clus)
	ld	e, h
	ld	d, #0
	ld	hl, (fat_lba)
	add	hl, de
	call	lba16
	call	rdbuf
	ret	c
	ld	a, (clus)
	ld	l, a
	ld	h, #0
	add	hl, hl
	ld	de, #BUF
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(clus), de
	jr	clus_ok

;; lba = data_lba + (clus - 2) << spc_sh
clus_lba:
	ld	hl, (clus)
	dec	hl
	dec	hl
	ld	de, #0
	ld	a, (spc_sh)
	or	a, a
	jr	z, cl_2
	ld	b, a
cl_1:	add	hl, hl
	rl	e
	rl	d
	djnz	cl_1
cl_2:	ld	bc, (data_lba)
	add	hl, bc
	jr	nc, cl_3
	inc	de
cl_3:	ld	(lba), hl
	ld	(lba + 2), de
	ret

lba16:	ld	(lba), hl
	ld	hl, #0
	ld	(lba + 2), hl
	ret

lba_add_a:
	ld	hl, (lba)
	ld	e, a
	ld	d, #0
	add	hl, de
	ld	(lba), hl
	ret	nc
	ld	hl, (lba + 2)
	inc	hl
	ld	(lba + 2), hl
	ret

;; Z = B bytes at HL equal those at DE
memcmp:	ld	a, (de)
	cp	a, (hl)
	ret	nz
	inc	hl
	inc	de
	djnz	memcmp
	ret

;; read volume sector `lba` into BUF / into HL; CY = error (outside the reader's reach)
rdbuf:	ld	hl, #BUF
rdsec:	ld	(blk + 5), hl
	ld	hl, (lba)			; t = lba << 1 (byte offset = t << 8)
	ld	de, (lba + 2)
	add	hl, hl
	rl	e
	rl	d
	ld	a, d
	or	a, a
	scf
	ret	nz
	ld	a, (p_volx)
	ld	(blk), a
	ld	a, (p_volx + 1)
	add	a, l
	ld	(blk + 1), a
	ld	a, (p_volx + 2)
	adc	a, h
	ld	(blk + 2), a
	ld	a, (p_volx + 3)
	adc	a, e
	ld	(blk + 3), a
	ret	c
	;; unit U = offset >> 18 -> DE, sector in unit S = (offset >> 9) & 511 -> BC
	ld	h, a
	ld	a, (blk + 2)
	ld	l, a
	ld	a, (blk + 1)
	srl	h
	rr	l
	rra
	ld	c, a
	xor	a, a
	srl	h
	rr	l
	rla
	ld	b, a
	ld	(rs_s), bc
	ex	de, hl
	;; zone: U < upz -> zone 0 (L = U), else zone 1 (L = U - upz < upz, flash has 2 zones)
	ld	hl, (BREC_UPZ)
	scf
	sbc	hl, de
	ld	hl, #BREC_DESC
	ld	a, #0
	jr	nc, rd_zone
	ld	hl, (BREC_CAP)			; capacity >= 4 * upz blocks: a second zone exists
	push	de
	ld	de, (BREC_UPZ)
	ex	de, hl
	add	hl, hl
	add	hl, hl
	ex	de, hl
	or	a, a
	sbc	hl, de
	pop	de
	ret	c
	ld	hl, (BREC_UPZ)
	ex	de, hl
	or	a, a
	sbc	hl, de
	ex	de, hl
	ld	hl, (BREC_UPZ)
	scf
	sbc	hl, de
	ret	c
	ld	hl, #BREC_DESC1
	ld	a, #0x08			; zone 1 = units 0x800..0xFFF
rd_zone:
	ld	(zoff), a
	ld	(lun), de
	ld	a, (BREC_GEOM)			; geometry BREC's table addressing assumes (F641)
	cp	a, #8
	scf
	ret	nz
	ld	e, (hl)				; table descriptor: k << 12 | table unit (< 0x800)
	inc	hl
	ld	d, (hl)
	ld	a, d
	and	a, #0x08
	scf
	ret	nz
	ld	a, (zoff)
	or	a, d
	and	a, #0x0F
	ld	h, a
	ld	l, e
	ld	(tunit), hl
	;; entry L of table copy k: sector 16k + 8 * (L >> 10) + ((L >> 8) & 3), byte 2 * (L & 0xFF)
	ld	a, (lun + 1)			; L >> 8 (0..7)
	ld	b, a
	and	a, #0x03
	ld	c, a
	ld	a, b
	and	a, #0x04
	add	a, a
	add	a, c
	ld	c, a
	ld	a, d
	and	a, #0xF0
	add	a, c
	ld	c, a
	ld	b, #0
	push	bc
	ld	e, #1				; select CE1 (as BREC's reader)
	call	BREC_SEL
	pop	bc
	ld	hl, (tunit)
	ld	de, #lk_blk
	call	BREC_ADDR
	ld	hl, #lk_word
	ld	(lk_blk + 5), hl
	ld	a, #8
	ld	(lk_blk + 7), a
	ld	a, (lun)
	ld	l, a
	ld	h, #0
	add	hl, hl
	ld	bc, #2
	ld	de, #lk_blk
	call	BREC_XFER			; 2 bytes via BREC's cached partial read
	ld	hl, (lk_word)			; zone-relative unit; 0xFFFF = unmapped
	ld	a, h
	cp	a, #0x08
	jr	nc, rd_fail
	ld	a, (zoff)
	or	a, h
	ld	h, a
	ld	bc, (rs_s)
	ld	de, #blk
	call	BREC_ADDR			; NAND address of (unit, sector) -> blk+0..4
	ld	a, #8				; blk+5,6 = destination, +7 = plain RAM
	ld	(blk + 7), a
	ld	hl, #0
	ld	bc, #0x0200
	ld	de, #blk
	call	BREC_XFER			; full sector, DMA straight to the destination
	or	a, a				; CY = 0
	jr	rd_end
rd_fail:
	scf
rd_end:	push	af
	call	BREC_DESEL
	pop	af
	ret

s_fat16:	.ascii	"FAT16"
ap_magic:	.db	0x50, 0x00, 0x57, 0x47, 0x19, 0x97, 0x03, 0x00

;; ---- variables (not part of the image contents that matter)
sv_bc:	.dw	0
sv_de:	.dw	0
sv_oe:	.db	0
sv_ie:	.db	0
zoff:	.db	0
lun:	.dw	0
tunit:	.dw	0
rs_s:	.dw	0
lk_word:	.dw	0
lk_blk:	.ds	8
lba:	.dw	0, 0
blk:	.ds	8
spc:	.db	0
spc_sh:	.db	0
fat_lba:	.dw	0
data_lba:	.dw	0
cur:	.dw	0
cnt:	.dw	0
clus:	.dw	0
zsize:	.dw	0
tlen:	.dw	0
dest:	.dw	0
nfs:	.db	0
fs:	.db	0
sic:	.db	0
main_end::
