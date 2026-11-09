;; ACTOS OS calls for SDCC (default __sdcccall(1)). OS services follow the IAR
;; convention: they preserve BC, DE, IX, IY.
	.module actos
	.area	_CODE

;; uint8_t os_get_msg(void)  - RST 08h A=0x11 GetSysMsg, 0 = none
_os_get_msg::
	ld	a, #0x11
	rst	0x08
	ret

;; void os_put_msg(uint8_t msg) - RST 08h A=0x10 PutSysMsg, E = msg
_os_put_msg::
	push	de
	ld	e, a
	ld	a, #0x10
	rst	0x08
	pop	de
	ret

;; void wdt_kick(void) - port 0x4E bit 3 = clear watchdog
_wdt_kick::
	in	a, (0x4e)
	set	3, a
	out	(0x4e), a
	ret

;; void os_backlight(uint8_t on) - UI API 0x14A4 (port EE bit 1)
_os_backlight::
	push	de
	ld	e, a
	ld	hl, #0x14a4
	rst	0x10
	pop	de
	ret

;; ---- OS timer (TM_SetTimer RST10 HL=0x0008: DE = handler, C = period -> A = id;
;;      TM_KillTimer RST10 HL=0x0108: E = id). Handler runs from the timer interrupt.
	.area	_DATA
_os_ticks::
	.ds	2
	.area	_CODE
tick_isr:
	push	hl
	ld	hl, (_os_ticks)
	inc	hl
	ld	(_os_ticks), hl
	pop	hl
	ret

;; uint8_t os_timer_start(uint8_t period)   (A = period, returns A = timer id)
_os_timer_start::
	push	bc
	push	de
	ld	c, a
	ld	de, #tick_isr
	ld	hl, #0x0008
	rst	0x10
	pop	de
	pop	bc
	ret

;; void os_timer_stop(uint8_t id)            (A = id)
_os_timer_stop::
	push	de
	ld	e, a
	ld	hl, #0x0108
	rst	0x10
	pop	de
	ret

;; uint16_t os_exec_ap(const char *name, uint16_t param)   HL = name, DE = param -> DE = result
;; RST 08h A=0x20 ExecAP (stock MAIN 0x0C63: DE = name, BC = param -> HL). Runs the AP, which
;; overwrites this one; the kernel reloads the caller from flash before returning, so the caller's
;; variables come back with their initial (image) values - keep state on the stack.
_os_exec_ap::
	push	ix
	push	iy
	ld	c, e
	ld	b, d
	ex	de, hl
	ld	a, #0x20
	rst	0x08
	ex	de, hl
	pop	iy
	pop	ix
	ret

;; uint8_t os_drv_install(uint8_t mode, const char *name)   A = mode, DE = name -> A != 0 ok
;; RST 10h HL=0x1000 DRV_Install (stock MAIN 0x15DB: DE = "UI_6B33L.DRV", C = 1)
_os_drv_install::
	push	bc
	push	de
	push	ix
	ld	c, a
	ld	hl, #0x1000
	rst	0x10
	pop	ix
	pop	de
	pop	bc
	ret

;; uint8_t os_usb_cable(void) -> A != 0: USB cable plugged (RST 10h HL=0x0021 GetUsbCableStatus)
_os_usb_cable::
	push	bc
	push	de
	push	ix
	ld	hl, #0x0021
	rst	0x10
	pop	ix
	pop	de
	pop	bc
	ret

;; System settings block ("comval", 512 B in the VM area), exactly as the stock MAIN reads/writes it
;; (bank40 0x1DF3): VMRead(DE = buf, BC = 0x200, [stack] = 0x001A), GetDefaultComval(DE = buf),
;; VM write 0x0092(DE = buf, BC = 0x200), UI_SetContrast(DE = value).
;; void os_comval_read(void *buf512)     HL = buf
_os_comval_read::
	push	bc
	push	de
	push	ix
	ex	de, hl
	ld	bc, #0x0200
	ld	hl, #0x001a
	push	hl
	ld	hl, #0x0091		; VMRead
	rst	0x10
	pop	hl
	pop	ix
	pop	de
	pop	bc
	ret

;; void os_comval_write(const void *buf512)   HL = buf
_os_comval_write::
	push	bc
	push	de
	push	ix
	ex	de, hl
	ld	bc, #0x0200
	ld	hl, #0x0092		; VM write
	rst	0x10
	pop	ix
	pop	de
	pop	bc
	ret

;; void os_comval_default(void *buf512)  HL = buf
_os_comval_default::
	push	bc
	push	de
	push	ix
	ex	de, hl
	ld	hl, #0x002b		; GetDefaultComval
	rst	0x10
	pop	ix
	pop	de
	pop	bc
	ret

;; void os_set_contrast(uint16_t v)      HL = v
_os_set_contrast::
	push	bc
	push	de
	push	ix
	ex	de, hl
	ld	hl, #0x11a4		; UI_SetContrast
	rst	0x10
	pop	ix
	pop	de
	pop	bc
	ret
