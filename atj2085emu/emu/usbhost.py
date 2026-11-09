"""Minimal USB host + SIE model for atj2085emu (new, optional; nothing in soc.py depends on it).

Models the ATJ2085 USB device controller as the stock USBDISK.AP uses it (ports 0x3E, 0x50-0x6A,
ZRAM2 bank ownership in port 0x70 bits 5:4; lisong_rebuild/analysis/usb_msc.md 2.1-2.4) plus a
scripted host. Where the silicon is not documented the model implements the inference written
down in lisong_rebuild/analysis/zos_usb.md, so a passing run proves the FIRMWARE LOGIC (state
machines, descriptors, SCSI/CDC replies, buffer ownership discipline), not the hardware.

Model:
  * transfers are seen at "arm" granularity: the firmware writes 64h then 65h (BR = 1); the host
    completes the transfer when the firmware has polled 68h/53h at least twice since arming
    (so the firmware sees BR = 1 first, as on the device), then BR = 0 and the 53h bit is set.
  * DMA address space = the USB-owned banks, concatenated: 70h[5:4] = 00 -> B1|B2 (0x000-0xFFF),
    01 -> B1 only, 10 -> B2 only, 11 -> none (a transfer then is a model error).
  * CPU view of 0x4000-0x4FFF: 11 -> B1|B2, 10 -> B1, 01 -> B2 (only 0x4000-0x47FF), 00 -> nothing.
    The emulator keeps one flat ZRAM2 window; the model swaps bank contents in/out of it on every
    70h write.
  * EP0 control read: data packets of 8; the transfer ends with a short arm, a zero-length arm,
    mode 3 (EPMODE_CRLAST) or when wLength is reached; until then 57h bit 5 (lock) stays set.
  * EP0 control write with data (zos inference): EPI 1 armed with mode 2 and count n receives the
    n data bytes and completes the status stage. No data: EPI 1, mode 2, count 0.
  * Stall = mode 1 on the endpoint (the host records it).
Usage: see zos/tools/emu_usb_test.py.
"""


class HostError(Exception):
    pass


EPI_CR, EPI_CW, EPI_EP1IN, EPI_EP1OUT, EPI_EP2IN = 0, 1, 2, 3, 4


class UsbHostModel:
    def __init__(self, emu, log=print):
        self.emu, self.log = emu, log
        self.orig_in, self.orig_out = emu._in, emu._out
        emu.m.set_input_callback(self._in)
        emu.m.set_output_callback(self._out)
        self.banks = [bytearray(0x800), bytearray(0x800)]
        self.bits = emu.ports[0x70] & 0x30
        self.pullup = 0
        self.addr = 0
        self.irq = 0
        self.epirq = 0
        self.setupst = 0
        self.setup = bytes(8)
        self.epi = 0
        self.ep = [dict(mode=0, mps=0, sa=0, cnth=0, cnt=0, br=False, polls=0, done=0) for _ in range(8)]
        self.gen = None
        self.errors = []
        self.trace = []
        self.stalls = []

    # ---- ZRAM2 banks / CPU view --------------------------------------------------------------
    def _view(self):
        e = self.emu
        if e._half_src(0, e.p05) == ('z',):
            return e.mem, 0x4000
        return e.zram_win, 0

    def _sync_out(self, bits):                 # CPU view -> banks (for MCU-owned banks)
        buf, base = self._view()
        if bits == 0x30:
            self.banks[0][:] = buf[base:base + 0x800]
            self.banks[1][:] = buf[base + 0x800:base + 0x1000]
        elif bits == 0x20:
            self.banks[0][:] = buf[base:base + 0x800]
        elif bits == 0x10:
            self.banks[1][:] = buf[base:base + 0x800]

    def _sync_in(self, bits):                  # banks -> CPU view
        buf, base = self._view()
        if bits == 0x30:
            buf[base:base + 0x800] = self.banks[0]
            buf[base + 0x800:base + 0x1000] = self.banks[1]
        elif bits == 0x20:
            buf[base:base + 0x800] = self.banks[0]
        elif bits == 0x10:
            buf[base:base + 0x800] = self.banks[1]

    def _usb_space(self):
        if self.bits == 0x00:
            return [(0, 0), (0x800, 1)]
        if self.bits == 0x10:                  # B1 to USB
            return [(0, 0)]
        if self.bits == 0x20:                  # B2 to USB
            return [(0, 1)]
        return []

    def _dma(self, off, n, data=None):
        sp = self._usb_space()
        size = 0x800 * len(sp)
        if off + n > size:
            self.err('DMA %s 0x%03x+%d outside the USB-owned banks (70h bits %02x)'
                     % ('write' if data is not None else 'read', off, n, self.bits))
            return bytes(n)
        out = bytearray()
        for i in range(n):
            a = off + i
            bank = sp[a >> 11][1]
            if data is not None:
                self.banks[bank][a & 0x7FF] = data[i]
            else:
                out.append(self.banks[bank][a & 0x7FF])
        return bytes(out)

    def err(self, s):
        self.errors.append('[%d] %s' % (self.emu.ticks, s))
        self.log('USB MODEL ERROR: ' + s)

    # ---- ports -------------------------------------------------------------------------------
    def _in(self, port):
        p = port & 0xFF
        if p == 0x3E:
            return self.pullup | 0x04
        if p == 0x50:
            v = self.orig_in(port)
            return v & 0x40                         # VBUS from the emulator's cable switch
        if 0x51 <= p <= 0x6A:
            if p in (0x52, 0x53, 0x68):
                self.poll()
            return self._reg(p)
        return self.orig_in(port)

    def _reg(self, p):
        e = self.ep[self.epi]
        if p == 0x51: return self.addr
        if p == 0x52: return self.irq
        if p == 0x53: return self.epirq
        if p == 0x57: return self.setupst
        if 0x58 <= p <= 0x5F: return self.setup[p - 0x58]
        if p == 0x60: return self.epi
        if p == 0x61: return e['mode']
        if p == 0x62: return e['sa'] >> 8
        if p == 0x63: return e['sa'] & 0xFF
        if p == 0x64: return e['done'] >> 8
        if p == 0x65: return e['done'] & 0xFF
        if p == 0x66: return e['mps']
        if p == 0x68: return sum(1 << i for i, x in enumerate(self.ep) if x['br'])
        return 0

    def _out(self, port, v):
        p = port & 0xFF
        if p == 0x70:
            old = self.bits
            self._sync_out(old)
            self.orig_out(port, v)
            self.bits = v & 0x30
            self._sync_in(self.bits)
            return
        if p == 0x05:
            self.orig_out(port, v)                 # the soc swaps the window; banks follow it
            return
        if p == 0x3E:
            self.pullup = v & 0x80
            self.orig_out(port, v)
            return
        if not 0x50 <= p <= 0x6A:
            return self.orig_out(port, v)
        e = self.ep[self.epi]
        if p == 0x50: pass
        elif p == 0x51: self.addr = v
        elif p == 0x52: self.irq &= ~v & 0xFF
        elif p == 0x53: self.epirq &= ~v & 0xFF
        elif p == 0x60: self.epi = v & 7
        elif p == 0x61: e['mode'] = v
        elif p == 0x62: e['sa'] = (e['sa'] & 0xFF) | (v << 8)
        elif p == 0x63: e['sa'] = (e['sa'] & 0xFF00) | v
        elif p == 0x64:
            e['cnth'] = v
            e['br'] = False
        elif p == 0x65:
            e['cnt'] = (e['cnth'] << 8) | v
            e['br'] = True
            e['polls'] = 0
            e['done'] = 0
            self.trace.append((self.emu.ticks, 'arm', self.epi, e['mode'], e['mps'], e['sa'], e['cnt']))
            if e['mode'] == 1:
                self.stalls.append((self.emu.ticks, self.epi))
        elif p == 0x66: e['mps'] = v

    # ---- host side -----------------------------------------------------------------------------
    def run(self, gen):
        """start a host script (generator; each `yield` = wait for the next firmware poll)"""
        self.gen = gen
        self.poll()

    def poll(self):
        for x in self.ep:
            if x['br']:
                x['polls'] += 1
        if self.gen is None:
            return
        try:
            next(self.gen)
        except StopIteration:
            self.gen = None
            self.log('USB host script finished')
        except HostError as ex:
            self.gen = None
            self.err('host: %s' % ex)

    def ready(self, epi):
        x = self.ep[epi]
        return x['br'] and x['polls'] >= 2

    def complete(self, epi, n):
        x = self.ep[epi]
        x['br'] = False
        x['done'] = n
        self.epirq |= 1 << epi

    def wait(self, cond, what, ticks=200_000_000):
        t0 = self.emu.ticks
        while not cond():
            if self.emu.ticks - t0 > ticks:
                raise HostError('timeout waiting for ' + what)
            yield

    def sleep(self, ticks):
        t0 = self.emu.ticks
        while self.emu.ticks - t0 < ticks:
            yield

    # ---- bus operations -------------------------------------------------------------------------
    def attach_and_reset(self):
        yield from self.wait(lambda: self.pullup, 'pull-up (attach)')
        yield from self.sleep(500_000)             # ~20 ms debounce
        self.irq |= 0x80                           # bus reset
        yield from self.wait(lambda: not (self.irq & 0x80) and self.addr == 0x80, 'bus reset handled')
        yield from self.sleep(50_000)

    def control(self, req, data=None, ok_stall=False):
        """req = 8 setup bytes. IN: returns the data; OUT/no data: returns b''. None = stalled."""
        req = bytes(req)
        wlen = req[6] | (req[7] << 8)
        self.setup = req
        self.setupst = 0x58 | 0x20
        self.irq |= 0x20
        yield from self.wait(lambda: not (self.irq & 0x20), 'SETUP taken')
        got = bytearray()
        if req[0] & 0x80:
            while True:
                yield from self.wait(lambda: self.ready(EPI_CR) or self._stalled(EPI_CW), 'control IN data')
                if self._stalled(EPI_CW):
                    return self._stall(req, ok_stall)
                x = self.ep[EPI_CR]
                n = x['cnt']
                d = self._dma(x['sa'], n)
                take = min(n, wlen - len(got))
                got += d[:take]
                last = n % 8 != 0 or n == 0 or x['mode'] == 3 or len(got) >= wlen
                if last:
                    self.setupst &= ~0x20          # host sends the OUT status, lock released
                self.complete(EPI_CR, n)
                if last:
                    break
            self.trace.append((self.emu.ticks, 'ctl-in', req.hex(), bytes(got).hex()))
            return bytes(got)
        yield from self.wait(lambda: (self.ready(EPI_CW) and self.ep[EPI_CW]['mode'] == 2)
                             or self._stalled(EPI_CW), 'control OUT/status')
        if self._stalled(EPI_CW):
            return self._stall(req, ok_stall)
        x = self.ep[EPI_CW]
        if wlen:
            if x['cnt'] != wlen:
                raise HostError('control write armed for %d bytes, wLength %d' % (x['cnt'], wlen))
            self._dma(x['sa'], wlen, data)
        self.setupst &= ~0x20
        self.complete(EPI_CW, wlen)
        self.trace.append((self.emu.ticks, 'ctl-out', req.hex(), (data or b'').hex()))
        return b''

    def _stalled(self, epi):
        x = self.ep[epi]
        return x['mode'] == 1 and x['br'] and x['polls'] >= 1

    def _stall(self, req, ok):
        x = self.ep[EPI_CW]
        x['br'] = False
        self.setupst &= ~0x20
        self.trace.append((self.emu.ticks, 'ctl-stall', req.hex()))
        if not ok:
            raise HostError('control request %s stalled' % req.hex())
        return None

    def bulk_out(self, data, epi=EPI_EP1OUT):
        data = bytes(data)
        while data:
            yield from self.wait(lambda: self.ready(epi) or self._stalled(epi), 'bulk OUT armed')
            x = self.ep[epi]
            if x['mode'] == 1:
                raise HostError('bulk OUT endpoint stalled')
            n = min(len(data), x['cnt'])
            self._dma(x['sa'], n, data[:n])
            data = data[n:]
            self.complete(epi, n)

    def bulk_in(self, n, epi=EPI_EP2IN):
        got = bytearray()
        while len(got) < n:
            yield from self.wait(lambda: self.ready(epi) or self._stalled(epi), 'bulk IN armed')
            x = self.ep[epi]
            if x['mode'] == 1:
                raise HostError('bulk IN endpoint stalled')
            c = x['cnt']
            got += self._dma(x['sa'], c)
            self.complete(epi, c)
            if c % 64 or c == 0:
                break                              # short packet ends the transfer
        if len(got) > n:
            raise HostError('device sent %d bytes, host asked %d' % (len(got), n))
        return bytes(got)

    def bulk_in_any(self, epi=EPI_EP2IN, ticks=50_000_000):
        """one IN transfer (console output), None if nothing within `ticks`"""
        t0 = self.emu.ticks
        while not (self.ready(epi) or self._stalled(epi)):
            if self.emu.ticks - t0 > ticks:
                return None
            yield
        x = self.ep[epi]
        if x['mode'] == 1:
            raise HostError('bulk IN endpoint stalled')
        d = self._dma(x['sa'], x['cnt'])
        self.complete(epi, x['cnt'])
        return d
