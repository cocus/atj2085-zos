"""SCSI transport that talks to the newbrec image running in atj2085emu (USB host model): lets
nandtool.py (dump / erase / write / flash-brec ...) run end to end without a device.

    t = EmuTransport(brec_path, workdir)      # boots through the BROM model, opens the USB item, enumerates
    status, data = t.scsi(cdb, data_in_len=0, data_out=None)
    t.emu / t.host                             # the emulator and the host model (screenshots, NAND state)
"""
import os, struct, sys
import emuboot
from emu.usbhost import UsbHostModel, HostError

MENU_TICKS = 40_000_000


class EmuTransport:
    def __init__(self, brec_path, workdir, log=lambda s: None):
        os.makedirs(workdir, exist_ok=True)
        self.workdir = workdir
        self.overlay = os.path.join(workdir, 'nand.ovl')
        self.emu = emuboot.boot(brec_path, self.overlay, log=log)
        self.host = UsbHostModel(self.emu, log=log)
        self.tag = 0x2000
        self.emu.run(MENU_TICKS)                    # LCD init (~1 s of programmed waits) + menu
        self.press('PLAY')                          # first item = USB
        self.emu.set_usb(True)
        self.drive(self.host.attach_and_reset())
        self.drive(self._enumerate())

    def shot(self, name):
        emuboot.shot(self.emu, os.path.join(self.workdir, name))

    def run(self, ticks):
        self.emu.run(ticks)

    def press(self, key, ms=150):
        self.emu.press(key, ms)
        self.emu.run(ms * 12000 + 6_000_000)

    def drive(self, gen, limit=4_000_000_000):
        res = {}

        def wrapper():
            res['v'] = yield from gen
        self.host.run(wrapper())
        t0 = self.emu.ticks
        while self.host.gen is not None:
            self.emu.run(200_000)
            if self.emu.ticks - t0 > limit:
                raise HostError('emulator time limit')
        if self.host.errors:
            raise HostError(self.host.errors[-1])
        return res.get('v')

    def _enumerate(self):
        h = self.host
        d = yield from h.control([0x80, 6, 0, 1, 0, 0, 64, 0])
        assert len(d) == 18, 'device descriptor'
        self.vid_pid = d[8:12]
        yield from h.control([0x00, 5, 7, 0, 0, 0, 0, 0])
        yield from h.sleep(100_000)
        yield from h.control([0x80, 6, 0, 2, 0, 0, 0xFF, 0])
        yield from h.control([0x00, 9, 1, 0, 0, 0, 0, 0])
        yield from h.control([0xA1, 0xFE, 0, 0, 0, 0, 1, 0])

    def _cmd(self, cdb, dlen, din, data):
        h = self.host
        self.tag += 1
        cbw = b'USBC' + struct.pack('<IIBBB', self.tag, dlen, 0x80 if din else 0, 0, len(cdb)) + bytes(cdb).ljust(16, b'\0')
        yield from h.bulk_out(cbw)
        got = b''
        if dlen:
            if din:
                got = yield from h.bulk_in(dlen)
            else:
                yield from h.bulk_out(data)
        csw = yield from h.bulk_in(13)
        assert csw[:4] == b'USBS' and struct.unpack_from('<I', csw, 4)[0] == self.tag, 'bad CSW'
        return csw[12], got

    def scsi(self, cdb, data_in_len=0, data_out=None):
        if data_out is not None:
            return self.drive(self._cmd(cdb, len(data_out), False, bytes(data_out)))
        return self.drive(self._cmd(cdb, data_in_len, True, None))

    def close(self):
        self.emu.nand.save_overlay(self.overlay)
