;; ACTOS file-system / drive calls for SDCC (__sdcccall(1) on the C side, IAR convention on
;; the OS side: arg1 E/DE, arg2 C/BC, more on the stack; result A (8-bit) / HL (16-bit)).
;; Contracts taken from the stock READER.AP call sites (v04 firmware).
	.module fs
	.area	_CODE

;; uint8_t drv_active_ud(uint8_t ud, uint8_t mode)       A = ud, L = mode -> A != 0 ok
_drv_active_ud::
	push	bc
	push	de
	ld	e, a
	ld	c, l
	ld	hl, #0x1300		; DRV_ActiveUD
	rst	0x10
	pop	de
	pop	bc
	ret

;; uint8_t drv_active_drive(char letter, uint8_t mode)   A = 'C', L = mode -> A != 0 ok
_drv_active_drive::
	push	bc
	push	de
	ld	e, a
	ld	c, l
	ld	hl, #0x1101		; DRV_ActiveDrive
	rst	0x10
	pop	de
	pop	bc
	ret

;; uint8_t fs_init(void) -> A != 0 ok
_fs_init::
	push	bc
	push	de
	ld	hl, #0x1080		; FS_Init
	rst	0x10
	pop	de
	pop	bc
	ret

;; uint8_t fs_cd(const char *name)          HL = name (":" root, "\\" parent)
_fs_cd::
	push	bc
	push	de
	ex	de, hl
	ld	hl, #0x1081		; FS_CD
	rst	0x10
	pop	de
	pop	bc
	ret

;; uint8_t fs_dir_first(char *buf, const char *ext3)   HL = buf (8.3 name out), DE = ext ("AP ")
;; uint8_t fs_dir_next(char *buf, const char *ext3)
_fs_dir_first::
	ld	a, #0x80		; FS_DIR_FILE
	jr	fs_dir
_fs_dir_next::
	ld	a, #0x00		; FS_DIR_FILE_CONT
fs_dir:
	push	bc
	push	de
	ld	b, d
	ld	c, e			; BC = ext
	ex	de, hl			; DE = buf
	ld	l, a
	push	hl			; stack arg: type (low byte)
	ld	hl, #0x1082		; FS_Dir
	rst	0x10
	pop	hl
	pop	de
	pop	bc
	ret

;; uint16_t fs_fopen(const char *name)      HL = name (8.3 as returned by fs_dir) -> DE = handle (0 = fail)
_fs_fopen::
	push	bc
	ex	de, hl
	ld	c, #0x00		; FS_OPEN_NORMAL
	ld	hl, #0x1087		; FS_FOpen
	rst	0x10
	ex	de, hl			; IAR result HL -> SDCC result DE
	pop	bc
	ret

;; uint8_t fs_fread(void *buf, uint16_t handle)   reads ONE 512-byte sector; HL = buf, DE = handle
_fs_fread::
	push	bc
	push	de			; stack arg: handle
	ex	de, hl			; DE = buf
	ld	c, #1			; 1 sector
	ld	hl, #0x128c		; FS_FRead
	rst	0x10
	pop	de
	pop	bc
	ret

;; void fs_fclose(uint16_t handle)           HL = handle
_fs_fclose::
	push	bc
	push	de
	ex	de, hl
	ld	hl, #0x108a		; FS_FClose
	rst	0x10
	pop	de
	pop	bc
	ret
