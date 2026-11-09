;; Variable-port USB accessors shared with the USB module's ZRAM1 helper.
	.module usbaudio_ports
	.area	_CODE
_port_in::
	ld	c, a
	in	a, (c)
	ret
_port_out::
	ld	c, a
	out	(c), l
	ret
