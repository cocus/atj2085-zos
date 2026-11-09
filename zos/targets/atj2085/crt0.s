;; zos start-up, hosted mode: the stock OS loads the image at 0x0800 (via the homebrew launcher)
;; and enters it like an IAR banked function: SP -> [bank word][return], param in DE.
;; Exit restores the caller's bank (OS BankSwitch, RST 10h HL=0x1008) and returns HL.
	.module crt0
	.globl	_zos_start

	.area	_CODE
entry::
	jp	_zos_start		; 0x0800; the API jump table follows at 0x0803 (jtab.s, generated)

	.area	_HOME
	.area	_INITIALIZER
	.area	_GSINIT
_zos_gsinit::				; crt0 is linked first: start of SDCC's static-initializer code
	.area	_GSFINAL
	ret
	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area	_BSS
	.area	_HEAP
