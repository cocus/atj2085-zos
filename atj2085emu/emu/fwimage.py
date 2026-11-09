"""Firmware image access.

Accepts either an AFI updater image (e.g. my-ximpro.fw, magic 'AFI\\0') - the
FWIMAGE.FW entry is extracted from its directory - or a raw FWIMAGE.FW
(magic 55 AA F0 0F; then a BREC must be supplied separately).

FWIMAGE.FW layout (verified against BREC loader at BRECF641.BIN:0x0216 and
SYSCFG.SYS:0x0733): sector 0 = header, from byte 0x200 a directory of 32-byte
entries: name[11] ... u32 sector @+0x10, u32 length @+0x14, checksum @+0x1C.
The kernel addresses the image by *byte offset* (sector << 9).
"""
import struct


class FwImage:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        self.afi = {}
        if raw[:4] == b'AFI\0':
            for i in range(0x20, 0x200, 32):
                e = raw[i:i + 32]
                if e[0] == 0:
                    continue
                off, ln = struct.unpack_from('<II', e, 0x10)
                nm = e[:8].decode('latin1').rstrip() + '.' + e[8:11].decode('latin1').rstrip()
                self.afi[nm.upper()] = raw[off:off + ln]
            self.fw = self.afi.get('FWIMAGE.FW')
            if not self.fw:
                raise ValueError('FWIMAGE.FW not found in AFI directory')
        elif raw[:4] == b'\x55\xaa\xf0\x0f':
            self.fw = raw
        else:
            raise ValueError('unknown firmware container (want AFI or FWIMAGE.FW)')
        self.files = {}
        for i in range(0x200, 0x200 + 240 * 32, 32):
            e = self.fw[i:i + 32]
            if e[0] in (0, 0xff):
                break
            name = e[:8].decode('latin1').rstrip() + '.' + e[8:11].decode('latin1').rstrip()
            sec, ln = struct.unpack_from('<II', e, 0x10)
            self.files[name.upper()] = (sec * 512, ln)

    def brec(self):
        """Boot record (BREC*.BIN) from the AFI container, or None."""
        for k, v in self.afi.items():
            if k.startswith('BREC') and v:
                return v
        return None

    def read(self, off, n):
        d = self.fw[off:off + n]
        return d + b'\xff' * (n - len(d))

    def file(self, name):
        off, ln = self.files[name.upper()]
        return self.fw[off:off + ln]

    def name_at(self, off):
        """Which directory file contains byte offset `off` (for tracing)."""
        for n, (o, ln) in self.files.items():
            if o <= off < o + max(ln, 1):
                return n, off - o
        return None, off
