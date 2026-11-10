"""zos USB module test in atj2085emu with the minimal host model (atj2085emu/emu/usbhost.py).

usage: lisong_rebuild/.venv/Scripts/python.exe zos/tools/emu_usb_test.py [OUTDIR]  (default zos/out/usbtest)
Runs build.py --no-deploy itself and copies ZOS.AP + the USB modules (USB.ZM launcher, USBDISK.ZM,
USBCON.ZM, USBAUDIO.ZM: MODS) into OUTDIR (out/ is shared).

Boots the stock firmware from the NAND dump with both USB modules injected into the user disk (nandinject
overlay), starts ZOS.AP like emu_nand_zos.py, opens MODULES -> USB (launcher), then (each function
is its own module, chained from the launcher and back):
  1. MASS STORAGE: plug the cable, enumerate (descriptors, SET_ADDRESS, SET_CONFIGURATION,
     GET_MAX_LUN), run SCSI (INQUIRY, TUR, READ CAPACITY, MODE SENSE 6/10, READ FORMAT CAPACITIES,
     READ(10) single + 9-block ping-pong compared with ftlread's FTL, WRITE(10) -> DATA PROTECT,
     out-of-range READ, unknown opcode, REQUEST SENSE after each failure), invalid CBW -> stall.
  2. disconnect, switch to SERIAL CONSOLE, connect: enumerate the CDC-ACM device, SET_LINE_CODING
     control write with data), SET_CONTROL_LINE_STATE, then console commands (help, ver, in,
     peek, sec 0, rdmode 1, wr on) and checks of the output.
  3. MASS STORAGE again (ping-pong READ, writes enabled): MODE SENSE without WP, WRITE(10) into a
     free cluster (forward, then backward = unit close), READ(10) back (the read closes the open
     unit), SYNCHRONIZE CACHE; afterwards the emulated NAND (overlay) is read with ftlread.py.
The SIE side is a MODEL (see usbhost.py): this checks zos' USB logic, not the silicon.
Exit code 0 = all checks passed. Screenshots + log in OUTDIR.
"""
import os, sys, struct, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
EMU = os.path.join(ROOT, 'atj2085emu')
TOOLS = os.path.join(ROOT, 'lisong_rebuild', 'tools')
sys.path.insert(0, EMU)
sys.path.insert(0, TOOLS)
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu import pngout
from emu.usbhost import UsbHostModel, HostError
from ftlread import Ftl, Fat, UD_FAT_BASE, ecc512, FW_UNITS

DUMP = os.path.join(ROOT, 'readback-my-ximpro.s1clone')
MODS = ('ZOS.AP', 'USB.ZM', 'USBDISK.ZM', 'USBCON.ZM', 'USBAUDIO.ZM')
OUTB = os.path.join(HERE, '..', 'out', 'atj2085')
MHZ = 24_576_000
fails = []


def check(cond, what):
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond:
        fails.append(what)


def le32(b, o=0):
    return struct.unpack_from('<I', b, o)[0]


def be32(b, o=0):
    return struct.unpack_from('>I', b, o)[0]


def enumerate_dev(h, pid, cfg_len):
    yield from h.attach_and_reset()
    d = yield from h.control([0x80, 6, 0, 1, 0, 0, 64, 0])
    check(len(d) == 18 and d[0] == 18 and d[1] == 1, 'device descriptor (wLength 64) = 18 bytes')
    check(d[8:12] == bytes([0x09, 0x12, pid, 0]), 'VID:PID 1209:%04x' % pid)
    revision = b'\x15\x00' if pid == 3 else b'\x10\x00'
    check(d[12:14] == revision, 'device revision %04x' % int.from_bytes(revision, 'little'))
    yield from h.control([0x00, 5, 7, 0, 0, 0, 0, 0])                 # SET_ADDRESS 7
    yield from h.sleep(100_000)
    check(h.addr == 0x87, 'address 7 set after the status stage (51h = %02x)' % h.addr)
    d = yield from h.control([0x80, 6, 0, 1, 0, 0, 18, 0])
    check(len(d) == 18, 'device descriptor (wLength 18)')
    c9 = yield from h.control([0x80, 6, 0, 2, 0, 0, 9, 0])
    check(len(c9) == 9 and c9[2] == cfg_len, 'config descriptor header, total %d' % c9[2])
    c = yield from h.control([0x80, 6, 0, 2, 0, 0, 0xFF, 0])
    check(len(c) == cfg_len, 'full config descriptor (wLength 255): %d bytes' % len(c))
    s0 = yield from h.control([0x80, 6, 0, 3, 0, 0, 0xFF, 0])
    check(s0 == bytes([4, 3, 9, 4]), 'string 0 (LANGID)')
    s2 = yield from h.control([0x80, 6, 2, 3, 9, 4, 0xFF, 0])
    prod = s2[2:].decode('utf-16-le') if s2 else ''
    check(prod.startswith('zos'), 'product string "%s"' % prod)
    q = yield from h.control([0x80, 6, 0, 6, 0, 0, 10, 0], ok_stall=True)
    check(q is None, 'device qualifier stalls (full speed only)')
    st = yield from h.control([0x80, 0, 0, 0, 0, 0, 2, 0])
    check(st == b'\0\0', 'GET_STATUS(device)')
    yield from h.control([0x00, 9, 1, 0, 0, 0, 0, 0])                 # SET_CONFIGURATION 1
    g = yield from h.control([0x80, 8, 0, 0, 0, 0, 1, 0])
    check(g == b'\x01', 'GET_CONFIGURATION = 1')
    return c


class Bot:
    def __init__(self, h):
        self.h, self.tag = h, 0x1000

    def cmd(self, cdb, dlen, din, data=None):
        h = self.h
        self.tag += 1
        cbw = b'USBC' + struct.pack('<IIBBB', self.tag, dlen, 0x80 if din else 0, 0, len(cdb)) + bytes(cdb).ljust(16, b'\0')
        yield from h.bulk_out(cbw)
        got = None
        if dlen:
            if din:
                got = yield from h.bulk_in(dlen)
            else:
                yield from h.bulk_out(data or bytes(dlen))
        csw = yield from h.bulk_in(13)
        if len(csw) != 13 or csw[:4] != b'USBS' or le32(csw, 4) != self.tag:
            raise HostError('bad CSW %s' % csw.hex())
        return got, csw[12], le32(csw, 8)

    def sense(self):
        d, st, _ = yield from self.cmd([0x03, 0, 0, 0, 18, 0], 18, True)
        return d[2] & 15, d[12]


def rd(b, ftl, lba, n, what):
    d, st, res = yield from b.cmd([0x28, 0] + list(struct.pack('>I', lba)) + [0, n >> 8, n & 255, 0], n * 512, True)
    ok = st == 0 and res == 0 and len(d) == n * 512
    for i in range(n):
        ok = ok and d[i * 512:(i + 1) * 512] == ftl.read_ud(UD_FAT_BASE + lba + i)
    check(ok, '%s: READ(10) lba %d x %d = FTL reference' % (what, lba, n))


def windows_attach(h, ftl, b, what, wp=True):
    """the command sequence Windows' usbstor + disk/partmgr/fastfat send after SET_CONFIGURATION
    (as seen in bus captures of USB sticks on Windows 7/10; order and lengths approximated)"""
    n = yield from h.control([0xA1, 0xFE, 0, 0, 0, 0, 1, 0])
    check(n == b'\0', 'GET_MAX_LUN = 0')
    d, st, _ = yield from b.cmd([0x12, 0, 0, 0, 0x24, 0], 0x24, True)
    check(st == 0 and len(d) == 36 and d[0] == 0 and d[1] == 0x80 and d[4] == 31,
          'INQUIRY 36: type 0, RMB, additional length 31')
    d, st, _ = yield from b.cmd([0x23, 0, 0, 0, 0, 0, 0, 0, 0xFC, 0, 0, 0], 0xFC, True)
    check(st == 0 and len(d) == 0xFC and d[3] == 8 and d[8] == 2, 'READ FORMAT CAPACITIES (0xFC)')
    d, st, _ = yield from b.cmd([0x12, 0, 0, 0, 0x24, 0], 0x24, True)
    d, st, _ = yield from b.cmd([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8, True)
    last = be32(d)
    check(st == 0 and last == 1981936 and be32(d, 4) == 512, 'READ CAPACITY(10) %d / 512' % last)
    for page, alloc in ((0x3F, 0xC0), (0x1C, 0xC0), (0x08, 0x04), (0x3F, 0x04)):
        d, st, _ = yield from b.cmd([0x1A, 0, page, 0, alloc, 0], alloc, True)
        check(st == 0 and len(d) == alloc and d[2] == (0x80 if wp else 0) and d[0] == 3,
              'MODE SENSE(6) page %02x alloc %d: %s, data length 3' % (page, alloc, 'WP' if wp else 'not WP'))
    for _ in range(2):
        d, st, _ = yield from b.cmd([0, 0, 0, 0, 0, 0], 0, True)
        check(st == 0, 'TEST UNIT READY')
    yield from rd(b, ftl, 0, 1, what)
    yield from rd(b, ftl, 0, 8, what)
    d, st, res = yield from b.cmd([0x12, 1, 0x80, 0, 0xFF, 0], 0xFF, True)
    check(st == 1 and len(d) == 0xFF, 'INQUIRY VPD 80: CSW failed, %d zero bytes' % len(d))
    k, a = yield from b.sense()
    check((k, a) == (5, 0x24), 'REQUEST SENSE: ILLEGAL REQUEST / invalid field')
    d, st, _ = yield from b.cmd([0x1E, 0, 0, 0, 1, 0], 0, True)
    check(st == 0, 'PREVENT MEDIUM REMOVAL')
    yield from rd(b, ftl, 1, 128, what)
    yield from rd(b, ftl, last - 7, 8, what)
    yield from rd(b, ftl, 300000, 9, what)


def wpat(lba, seed):
    return bytes(((i * 7) + lba * 31 + seed) & 0xFF for i in range(512))


def free_lba(ftl):
    """first sector of the first free cluster (FAT 1) that lies inside one FTL unit"""
    fat = Fat(lambda s: ftl.read_ud(UD_FAT_BASE + s), 0)
    f = fat.fat()
    for c in range(2, fat.nclus + 2):
        s = fat.data0 + (c - 2) * fat.spc
        if f[c] == 0 and (UD_FAT_BASE + s) >> 9 == (UD_FAT_BASE + s + fat.spc - 1) >> 9:
            return s
    raise HostError('no free cluster')


WRITES = []                                   # (lba, seed) in order, checked on the NAND afterwards


def msc_pp_script(h, ftl):
    """third mass-storage session after 'rdmode 1' (stock bank ping-pong) and 'wr on'"""
    yield from enumerate_dev(h, 1, 32)
    b = Bot(h)
    yield from windows_attach(h, ftl, b, 'ping-pong READ path', wp=False)
    d, st, _ = yield from b.cmd([0x12, 0, 0, 0, 36, 0], 36, True)
    check(st == 0 and d[16:27] == b'USB DISK RW', 'INQUIRY product "%s" (writes on)' % d[16:32].decode())
    d, st, _ = yield from b.cmd([0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 0xFF, 0], 0xFF, True)
    check(st == 0 and not d[3] & 0x80, 'MODE SENSE(10): not write protected')
    lba = free_lba(ftl)

    def wr(l, n, seed):
        data = b''.join(wpat(l + i, seed) for i in range(n))
        WRITES.extend((l + i, seed) for i in range(n))
        d, st, res = yield from b.cmd([0x2A, 0] + list(struct.pack('>I', l)) + [0, 0, n, 0], n * 512, False, data)
        check(st == 0 and res == 0, 'WRITE(10) lba %d x %d (free cluster)' % (l, n))

    def rdback(l, n, what):
        d, st, res = yield from b.cmd([0x28, 0] + list(struct.pack('>I', l)) + [0, 0, n, 0], n * 512, True)
        want = {x: s for x, s in WRITES}
        ok = st == 0 and res == 0 and len(d) == n * 512
        for i in range(n):
            ok = ok and d[i * 512:(i + 1) * 512] == wpat(l + i, want[l + i])
        check(ok, '%s: READ(10) lba %d x %d = written data' % (what, l, n))

    yield from wr(lba + 1, 9, 1)                              # 4 + 4 + 1 blocks per transfer
    yield from rdback(lba + 1, 9, 'open unit (closed by the read)')
    yield from wr(lba + 5, 3, 2)                              # rewrite inside the unit
    yield from wr(lba, 2, 3)                                  # backward: close + reopen
    d, st, _ = yield from b.cmd([0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0], 0, True)
    check(st == 0, 'SYNCHRONIZE CACHE(10): unit closed')
    yield from rdback(lba, 10, 'after SYNCHRONIZE CACHE')
    d, st, res = yield from b.cmd([0x2A, 0] + list(struct.pack('>I', 1981936)) + [0, 0, 2, 0], 1024, False,
                                  bytes(1024))
    k, a = yield from b.sense()
    check(st == 1 and (k, a) == (5, 0x21), 'WRITE(10) past the end: ILLEGAL REQUEST / LBA out of range')


def msc_script(h, ftl):
    yield from enumerate_dev(h, 1, 32)
    b = Bot(h)
    yield from windows_attach(h, ftl, b, 'flat READ path')
    d, st, _ = yield from b.cmd([0x12, 0, 0, 0, 36, 0], 36, True)
    check(st == 0 and len(d) == 36 and d[1] & 0x80 and d[8:11] == b'zos', 'INQUIRY: removable, "%s"' % d[8:36].decode())
    d, st, _ = yield from b.cmd([0x00, 0, 0, 0, 0, 0], 0, True)
    check(st == 0, 'TEST UNIT READY')
    d, st, _ = yield from b.cmd([0x25] + [0] * 9, 8, True)
    last = be32(d)
    check(st == 0 and be32(d, 4) == 512, 'READ CAPACITY: last LBA %d, 512-byte blocks' % last)
    check(last == 0x1EC0 * 256 - 0x8210, 'capacity = stock formula 256*0x1EC0 - 0x8210')
    d, st, _ = yield from b.cmd([0x1A, 0, 0x3F, 0, 0xC0, 0], 0xC0, True)
    check(st == 0 and len(d) == 0xC0 and d[2] & 0x80 and d[0] == 3, 'MODE SENSE(6): write protected')
    d, st, _ = yield from b.cmd([0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 0xFF, 0], 0xFF, True)
    check(st == 0 and d[3] & 0x80, 'MODE SENSE(10): write protected')
    d, st, _ = yield from b.cmd([0x23] + [0] * 6 + [0, 0xFC, 0], 0xFC, True)
    check(st == 0 and be32(d, 4) == last + 1 and d[8] == 2, 'READ FORMAT CAPACITIES')
    d, st, _ = yield from b.cmd([0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0], 512, True)
    ref = ftl.read_ud(UD_FAT_BASE + 0)
    check(st == 0 and d == ref, 'READ(10) LBA 0 = FTL reference (boot sector)')
    check(d[510:512] == b'\x55\xaa' and struct.unpack_from('<I', d, 0x20)[0] == last + 1,
          'boot sector: 55AA, total sectors %d = last LBA + 1' % struct.unpack_from('<I', d, 0x20)[0])
    lba0 = 300000
    d, st, _ = yield from b.cmd([0x28, 0] + list(struct.pack('>I', lba0)) + [0, 0, 9, 0], 9 * 512, True)
    ok = st == 0 and len(d) == 9 * 512
    for i in range(9):
        ok = ok and d[i * 512:(i + 1) * 512] == ftl.read_ud(UD_FAT_BASE + lba0 + i)
    check(ok, 'READ(10) 9 blocks at %d (ping-pong 4+4+1) = FTL reference' % lba0)
    d, st, res = yield from b.cmd([0x2A, 0, 0, 0, 0, 100, 0, 0, 2, 0], 1024, False, bytes(range(256)) * 4)
    check(st == 1 and res == 1024, 'WRITE(10) 2 blocks: data drained, CSW failed (residue %d)' % res)
    k, a = yield from b.sense()
    check((k, a) == (7, 0x27), 'REQUEST SENSE after WRITE: DATA PROTECT 07/27 (got %02x/%02x)' % (k, a))
    d, st, _ = yield from b.cmd([0x28, 0] + list(struct.pack('>I', last)) + [0, 0, 2, 0], 1024, True)
    check(st == 1 and len(d) == 1024, 'READ(10) past the end: zero data + CSW failed')
    k, a = yield from b.sense()
    check((k, a) == (5, 0x21), 'sense ILLEGAL REQUEST / LBA out of range')
    d, st, _ = yield from b.cmd([0xA0] + [0] * 11, 16, True)
    check(st == 1 and d == bytes(16), 'unknown opcode: zero data + CSW failed')
    k, a = yield from b.sense()
    check((k, a) == (5, 0x20), 'sense ILLEGAL REQUEST / invalid opcode')
    yield from h.bulk_out(b'XXXX' + bytes(27))                            # invalid CBW
    yield from h.sleep(200_000)
    check(h.ep[3]['mode'] == 1 and h.ep[4]['mode'] == 1, 'invalid CBW: bulk endpoints stalled')
    st = yield from h.control([0x82, 0, 0, 0, 0x82, 0, 2, 0])
    check(st == b'\1\0', 'GET_STATUS(EP 0x82) = halted')
    yield from h.control([0x21, 0xFF, 0, 0, 0, 0, 0, 0])                 # BOT reset recovery
    yield from h.control([0x02, 1, 0, 0, 0x82, 0, 0, 0])
    yield from h.control([0x02, 1, 0, 0, 0x01, 0, 0, 0])
    d, st, _ = yield from b.cmd([0x00, 0, 0, 0, 0, 0], 0, True)
    check(st == 0, 'TEST UNIT READY after reset recovery')
    d, st, _ = yield from b.cmd([0x1B, 0, 0, 0, 2, 0], 0, True)
    check(st == 0, 'START STOP UNIT (eject)')
    d, st, _ = yield from b.cmd([0x00, 0, 0, 0, 0, 0], 0, True)
    k, a = yield from b.sense()
    check(st == 1 and (k, a) == (2, 0x3A), 'after eject: NOT READY / medium not present')


def cdc_script(h, ftl):
    yield from enumerate_dev(h, 2, 75)
    yield from h.control([0x21, 0x20, 0, 0, 0, 0, 7, 0], bytes([0x80, 0x25, 0, 0, 0, 0, 8]))   # 9600 8N1
    lc = yield from h.control([0xA1, 0x21, 0, 0, 0, 0, 7, 0])
    check(lc == bytes([0x80, 0x25, 0, 0, 0, 0, 8]), 'SET/GET_LINE_CODING round trip (control write data)')
    yield from h.control([0x21, 0x22, 3, 0, 0, 0, 0, 0])                  # DTR + RTS
    out = bytearray()

    def drain(ticks=30_000_000):
        while True:
            d = yield from h.bulk_in_any(ticks=ticks)
            if d is None:
                return
            out.extend(d)

    yield from drain()
    check(b'zos USB console' in out and out.endswith(b'zos> '), 'greeting + prompt on DTR')

    def cmd(line):
        out.clear()
        for i in range(0, len(line), 5):                                    # several short packets
            yield from h.bulk_out(line[i:i + 5])
            yield from drain(3_000_000)
        yield from drain()
        return bytes(out)

    r = yield from cmd(b'help\r')
    check(b'peek ADDR' in r and r.endswith(b'zos> '), 'help')
    r = yield from cmd(b'ver\r')
    check(b'last LBA 001E3DF0' in r, 'ver (last LBA 1E3DF0)')
    r = yield from cmd(b'in 50\r')
    check(b'50 = 40' in r, 'in 50 -> VBUS bit')
    r = yield from cmd(b'peek 803 6\r')
    check(r.startswith(b'peek 803 6\r\n0803: C3 '), 'peek 803 6 (API jump table)')
    r = yield from cmd(b'sec 0\r')
    ref = ftl.read_ud(UD_FAT_BASE)
    want = ('01F0: ' + ' '.join('%02X' % x for x in ref[0x1F0:0x200])).encode()
    check(want in r and r.count(b'\r\n') >= 33, 'sec 0 hexdump matches the boot sector (%d bytes out)' % len(r))
    r = yield from cmd(b'pke\x08\x08eek 4000 4\r')
    check(b'4000: 5A 4D' in r, 'line editing (backspace) + peek of the module header "ZM"')
    r = yield from cmd(b'bogus\r')
    check(b'unknown command' in r, 'unknown command')
    r = yield from cmd(b'log\r')
    open(os.path.join(OUTDIR, 'console_log.txt'), 'wb').write(r)
    check(b'SETUP A1 FE' in r and b'SCSI 12 ok' in r and b'SCSI 28 ok   lba 0000 n 08' in r
          and b'SCSI 2A FAIL' in r and b'INVALID CBW' in r,
          'log: GET_MAX_LUN, INQUIRY, READ 8 at 0, WRITE failure, invalid CBW (%d bytes)' % len(r))
    r = yield from cmd(b'rdmode 1\r')
    check(b'READ(10) path 1' in r, 'rdmode 1 (stock ping-pong for the next mass-storage session)')
    r = yield from cmd(b'wr\r')
    check(b'mass-storage writes off, write error 00' in r, 'wr: writes off by default')
    r = yield from cmd(b'wr on\r')
    check(b'mass-storage writes ON' in r, 'wr on (writes for the next mass-storage session)')


def audio_script(h, terminal_type):
    c = yield from enumerate_dev(h, 3, 111)
    descs = []
    off = 0
    valid = True
    while off < len(c):
        n = c[off]
        if n < 2 or off + n > len(c):
            valid = False
            break
        descs.append(c[off:off + n])
        off += n
    input_terminal = next((d for d in descs if d[1] == 0x24 and d[2] == 2), b'')
    feature_unit = next((d for d in descs if d[1] == 0x24 and d[2] == 6), b'')
    output_terminal = next((d for d in descs if d[1] == 0x24 and d[2] == 3), b'')
    fmt = next((d for d in descs if d[1] == 0x24 and d[2] == 2 and len(d) == 11), b'')
    endpoint = next((d for d in descs if d[1] == 5 and d[2] == 0x83), b'')
    cs_endpoint = next((d for d in descs if d[1] == 0x25), b'')
    freq = int.from_bytes(fmt[8:11], 'little') if fmt else 0
    packet = struct.unpack_from('<H', endpoint, 4)[0] if endpoint else 0
    check(valid and off == len(c) and len(descs) > 0, 'Audio configuration descriptor chain is well formed')
    check(input_terminal and int.from_bytes(input_terminal[4:6], 'little') == terminal_type,
          'Audio source terminal type %04x' % terminal_type)
    check(input_terminal[6] == 0 and output_terminal[6] == 0
          and feature_unit[3:6] == b'\x02\x01\x02'
          and feature_unit[6:10] == b'\0\0\0\0'
          and output_terminal[7] == 2,
          'AudioControl has no terminal associations and a valid source-to-USB topology')
    check(freq == 32000 and packet == 64 and len(endpoint) == 9 and endpoint[3] == 0x05
          and endpoint[7:9] == b'\0\0',
          'Audio stream is asynchronous isochronous, 32 kHz, 64-byte packet, 9-byte UAC endpoint')
    check(len(cs_endpoint) == 7 and cs_endpoint[3] == 0,
          'Fixed 32 kHz endpoint does not advertise an unimplemented sampling-frequency control')
    alt = yield from h.control([0x81, 10, 0, 0, 1, 0, 1, 0])
    check(alt == b'\0', 'AudioStreaming interface starts at alternate setting 0')
    yield from h.control([0x01, 11, 1, 0, 1, 0, 0, 0])
    alt = yield from h.control([0x81, 10, 0, 0, 1, 0, 1, 0])
    check(alt == b'\1', 'SET_INTERFACE starts audio capture')
    yield from h.control([0x01, 11, 0, 0, 1, 0, 0, 0])
    alt = yield from h.control([0x81, 10, 0, 0, 1, 0, 1, 0])
    check(alt == b'\0', 'SET_INTERFACE stops audio capture')


def stage(out):
    """build, then copy ZOS.AP + USB.ZM under build.py's lock (out/ is shared by concurrent builds,
    each of which empties out/atj2085 first)"""
    import shutil, time, msvcrt
    for _ in range(5):
        subprocess.run([sys.executable, os.path.join(HERE, '..', 'build.py'), '--no-deploy'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(os.path.join(HERE, '..', 'out', '.buildlock'), 'a+b') as f:
            while True:
                try:
                    f.seek(0)
                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.5)
            try:
                ok = all(os.path.exists(os.path.join(OUTB, n)) for n in MODS)
                if ok:
                    for n in MODS:
                        shutil.copy(os.path.join(OUTB, n), os.path.join(out, n))
            finally:
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        if ok:
            return out
    sys.exit('could not stage ' + ' / '.join(MODS))


def main():
    global OUTB, OUTDIR
    out = OUTDIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'out', 'usbtest')
    os.makedirs(out, exist_ok=True)
    OUTB = stage(out)
    ovl = os.path.join(out, 'usb.ovl')
    subprocess.run([sys.executable, os.path.join(TOOLS, 'nandinject.py'), DUMP, ovl]
                   + [os.path.join(OUTB, n) for n in MODS[1:]], check=True)
    logf = open(os.path.join(out, 'run.log'), 'w')
    log = lambda s: logf.write(s + '\n')
    emu = ATJ2085(FwImage(os.path.join(ROOT, 'my-ximpro.fw')), log=log, nand=DUMP, nand_overlay=ovl,
                  nand_cache=os.path.join(EMU, 'work', 'nandcache'))
    ap = open(os.path.join(OUTB, 'ZOS.AP'), 'rb').read()
    ln, addr = struct.unpack_from('<HH', ap, 12)
    text = ap[0x800:0x800 + ln]
    ftl = Ftl(DUMP, os.path.join(EMU, 'work', 'nandcache'))
    h = None
    step = 1_000_000

    def run_until(t):
        while emu.ticks < t:
            emu.run(step)
            if h is not None and h.gen is not None and h.errors:
                break

    run_until(300_000_000)
    m = emu.m
    emu.mem[addr:addr + len(text)] = text
    sp = (m.sp - 4) & 0xFFFF
    emu.mem[sp:sp + 4] = bytes(4)
    m.sp, m.pc = sp, 0x0800
    t = emu.ticks
    for dt, k in ((40, 'PLAY'), (160, 'PLAY')):     # module list (no SHELL.ZM) -> USB, connect
        run_until(t + dt * 1_000_000)
        emu.press(k, 120)
    run_until(emu.ticks + 60_000_000)
    pngout.save_lcd(emu.lcd, os.path.join(out, '1_msc_connected_nocable.png'))
    h = UsbHostModel(emu, log)
    h._sync_out(h.bits)
    emu.set_usb(True)
    phases = (('MASS STORAGE (flat READ)', msc_script, '2_msc', ('PLAY', 'RIGHT', 'PLAY')),
    ('SERIAL CONSOLE', cdc_script, '4_cdc', ('PLAY', 'RIGHT', 'PLAY', 'PLAY')),
              ('USB AUDIO: MIC', lambda host, disk: audio_script(host, 0x0201),
     '6_usb_audio_mic', ('PLAY', 'RIGHT', 'PLAY')),
              ('USB AUDIO: RADIO', lambda host, disk: audio_script(host, 0x0701),
     '7_usb_audio_radio', ('PLAY', 'MENU', 'LEFT', 'LEFT', 'PLAY')),
              ('MASS STORAGE (ping-pong READ)', msc_pp_script, '8_msc_pp',
               ()))
    for name, script, tag, keys in phases:
        print(name)
        h.errors.clear()
        h.run(script(h, ftl))
        t0 = emu.ticks
        while h.gen is not None and emu.ticks - t0 < 3_000_000_000 and not h.errors:
            emu.run(step)
            h.poll()
        check(h.gen is None and not h.errors, '%s host script completed (%.1f s emulated)'
              % (name, (emu.ticks - t0) / MHZ))
        for e in h.errors:
            print('    ' + e)
        h.gen = None
        run_until(emu.ticks + 40_000_000)                 # let the LCD status catch up
        pngout.save_lcd(emu.lcd, os.path.join(out, tag + '_done.png'))
        if keys:                                       # disconnect, other function, connect
            for k in keys:
                emu.press(k, 120)
                run_until(emu.ticks + 60_000_000)
            check(h.pullup == 0x80, 'pull-up on again after the function switch')
    ovl2 = os.path.join(out, 'usb_after.ovl')
    emu.nand.save_overlay(ovl2)
    st = emu.nand.chips[1].stats
    check(st['overprogram'] == st['nop_over'] == st['out_of_order'] == 0,
          'NAND rules (no program over programmed bytes, NOP <= 4, page order): %s'
          % {k: st[k] for k in ('programs', 'erases', 'overprogram', 'nop_over', 'out_of_order')})
    if WRITES:
        a = Ftl(DUMP, os.path.join(EMU, 'work', 'nandcache'), ovl2)
        want = {x: s for x, s in WRITES}
        ok = all(a.read_ud(UD_FAT_BASE + x) == wpat(x, s) for x, s in want.items())
        sp_ok = True
        for x, s in want.items():
            U = FW_UNITS + ((UD_FAT_BASE + x) >> 9)
            page, col = a.sector_loc(U, (UD_FAT_BASE + x) & 511)
            g = 0x800 + (col // 512) * 16
            sp = a.c.read_raw(page)[g:g + 16]
            sp_ok = sp_ok and sp == b'\xff\xff' + struct.pack('<H', U % a.luz) + b'\xff' * 4 + ecc512(wpat(x, s)) + b'\xff\xff\x55\xff'
        check(ok and sp_ok, 'NAND after the session (ftlread + overlay): %d written sectors = data, stock spare' % len(want))
    for x in h.trace[-60:]:
        log(repr(x))
    logf.close()
    print('%d checks failed' % len(fails) if fails else 'all checks passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
