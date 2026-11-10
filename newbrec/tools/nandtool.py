#!/usr/bin/env python3
"""nandtool - PC side of the newbrec USB mode: the player shows the WHOLE NAND as one raw disk of 512-byte
sectors in physical order (LBA = block * 256 + page * 4 + sector; 64 pages of 4 sectors per block; 1 GB =
8192 blocks on the ximpro). READ(10) reads; WRITE(10) erases a block when sector 0 of its page 0 is written
and programs sectors with hardware ECC and the BREC spare (FF x8, ECC x4, FF FF 55 FF), so a block must be
written in ascending sector order. The spare bytes of the stock FTL (bad-block flag, 0xBB table marker,
logical unit number) are NOT readable or writable through this raw disk: dump/restore are data-only
(see README "Limits").

usage:  nandtool.py DEVICE COMMAND ...
  DEVICE   \\\\.\\PhysicalDriveN (Windows, run as Administrator), /dev/sdX (Linux, root), an image file, or
           emu:PATH_TO_NEWBREC.BIN  (the image running in atj2085emu, for tests)
  info                          size, blocks, boot-record check of blocks 0-3
  dump OUT [FIRST [COUNT]]      data of COUNT blocks from block FIRST (default all) to a file
  read-block N OUT              one block (128 KB) to a file
  write-block N FILE            erase + write one block from a 128 KB file, read back and compare
  erase N [COUNT]               erase blocks (writes an FF sector 0, which erases the block; data reads FF)
  restore FILE [FIRST]          write a dump back from block FIRST (default 0), verified; blocks 0 and 1
                                are written LAST, block 1 before block 0 (a half restore keeps a boot record)
  flash-brec FILE               write a 16 KB boot record (0x55AA + sum checked) to block 1, verify, then block 0
  put-slot BLOCK FILE [--brec]  write FILE (padded to 128 KB blocks) into consecutive blocks from BLOCK;
                                --brec: FILE is a 16 KB boot record, written to BLOCK and BLOCK+1 (slot copies)
Add --yes to skip the confirmation of commands that write.
"""
import os, struct, sys, hashlib

SECT, SPB, BLK = 512, 256, 512 * 256
BREC_LEN = 0x4000


# ----------------------------------------------------------------------------------------- devices
class FileDev:
    """image file or physical drive: aligned reads / writes at lba * 512"""
    def __init__(self, path, write=False):
        self.path = path
        self.f = open(path, 'r+b' if write else 'rb', buffering=0)
        self.n = self._size()

    def _size(self):
        if os.name == 'nt' and self.path.startswith('\\\\.\\'):
            import ctypes
            from ctypes import wintypes
            k32 = ctypes.windll.kernel32
            buf = ctypes.c_longlong()
            ret = wintypes.DWORD()
            if k32.DeviceIoControl(wintypes.HANDLE(ctypes.windll.msvcrt._get_osfhandle(self.f.fileno())),
                                   0x7405C, None, 0, ctypes.byref(buf), 8, ctypes.byref(ret), None):
                return buf.value // SECT
        return self.f.seek(0, 2) // SECT

    def sectors(self):
        return self.n

    def read(self, lba, n):
        self.f.seek(lba * SECT)
        d = b''
        while len(d) < n * SECT:
            c = self.f.read(n * SECT - len(d))
            if not c:
                break
            d += c
        return d.ljust(n * SECT, b'\xff')

    def write(self, lba, data):
        self.f.seek(lba * SECT)
        self.f.write(data)


class EmuDev:
    """the image in the emulator, driven through the USB host model with real SCSI commands"""
    def __init__(self, brec, workdir):
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, here)
        from emutransport import EmuTransport
        self.t = EmuTransport(brec, workdir)
        st, d = self.t.scsi([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8)
        assert st == 0
        self.n = struct.unpack('>I', d[:4])[0] + 1

    def sectors(self):
        return self.n

    def read(self, lba, n):
        out = b''
        while n:
            k = min(n, 32)
            st, d = self.t.scsi([0x28, 0] + list(struct.pack('>I', lba)) + [0, k >> 8, k & 255, 0], k * SECT)
            if st:
                raise IOError('READ(10) failed at %d' % lba)
            out += d
            lba += k
            n -= k
        return out

    def write(self, lba, data):
        n = len(data) // SECT
        o = 0
        while n:
            k = min(n, 32)
            st, _ = self.t.scsi([0x2A, 0] + list(struct.pack('>I', lba)) + [0, k >> 8, k & 255, 0],
                                data_out=data[o:o + k * SECT])
            if st:
                raise IOError('WRITE(10) failed at %d' % lba)
            lba += k
            o += k * SECT
            n -= k


def open_dev(spec, write):
    if spec.startswith('emu:'):
        return EmuDev(spec[4:], os.environ.get('NANDTOOL_EMU_WORK', 'emu_work'))
    return FileDev(spec, write)


# ----------------------------------------------------------------------------------------- helpers
def brec_ok(b):
    return (len(b) == BREC_LEN and struct.unpack_from('<H', b, 0x3FFC)[0] == 0x55AA
            and struct.unpack_from('<H', b, 0x3FFE)[0] == sum(struct.unpack_from('<8191H', b)) & 0xFFFF)


def read_block(dev, blk):
    return dev.read(blk * SPB, SPB)


def write_block(dev, blk, data, verify=True):
    """data = whole block or the first pages of it (multiple of 512, ascending sectors from sector 0)"""
    assert len(data) % SECT == 0 and 0 < len(data) <= BLK
    dev.write(blk * SPB, data)
    if verify:
        back = dev.read(blk * SPB, len(data) // SECT)
        if back != data:
            bad = next(i for i in range(len(data)) if back[i] != data[i])
            raise IOError('verify failed in block %d at byte %d' % (blk, bad))


def confirm(a, what):
    if '--yes' in a:
        return
    if input('%s - type YES to continue: ' % what) != 'YES':
        sys.exit('aborted')


def cmd_info(dev):
    n = dev.sectors()
    blocks = n // SPB
    print('sectors %d  blocks %d  size %d MB' % (n, blocks, n * SECT // (1 << 20)))
    for b in range(4):
        d = read_block(dev, b)[:BREC_LEN]
        ff = d == b'\xff' * BREC_LEN
        print('block %d: %s  md5(16 KB) %s' % (b, 'erased' if ff else ('boot record OK (0x55AA + sum)' if brec_ok(d)
                                                                   else 'data, not a valid boot record'),
                                               hashlib.md5(d).hexdigest()))


def cmd_dump(dev, out, first=0, count=None):
    blocks = dev.sectors() // SPB
    count = blocks - first if count is None else count
    with open(out, 'wb') as f:
        for b in range(first, first + count):
            f.write(read_block(dev, b))
            if b % 64 == 0:
                print('block %d/%d' % (b, first + count), end='\r')
    print('wrote %s: %d blocks' % (out, count))


def cmd_erase(dev, first, count):
    for b in range(first, first + count):
        write_block(dev, b, b'\xff' * SECT)
    print('erased blocks %d..%d' % (first, first + count - 1))


def flash_brec(dev, path, blocks=(1, 0)):
    b = open(path, 'rb').read()
    if not brec_ok(b):
        sys.exit('%s is not a valid 16 KB boot record (size, 0x55AA, sum)' % path)
    for blk in blocks:
        write_block(dev, blk, b)
        print('block %d written and verified' % blk)
    print('md5 %s' % hashlib.md5(b).hexdigest())


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] in ('-h', '--help'):
        print(__doc__)
        return 1
    spec, cmd, rest = a[0], a[1], [x for x in a[2:] if x != '--yes']
    flags = a[2:]
    writes = cmd in ('write-block', 'erase', 'restore', 'flash-brec', 'put-slot')
    dev = open_dev(spec, writes)
    if cmd == 'info':
        cmd_info(dev)
    elif cmd == 'dump':
        cmd_dump(dev, rest[0], int(rest[1]) if len(rest) > 1 else 0, int(rest[2]) if len(rest) > 2 else None)
    elif cmd == 'read-block':
        open(rest[1], 'wb').write(read_block(dev, int(rest[0])))
    elif cmd == 'write-block':
        d = open(rest[1], 'rb').read()
        confirm(flags, 'write block %s' % rest[0])
        write_block(dev, int(rest[0]), d.ljust(BLK, b'\xff')[:BLK])
        print('block %s written and verified' % rest[0])
    elif cmd == 'erase':
        confirm(flags, 'erase blocks %s' % rest)
        cmd_erase(dev, int(rest[0]), int(rest[1]) if len(rest) > 1 else 1)
    elif cmd == 'flash-brec':
        confirm(flags, 'flash the boot record %s to blocks 1 then 0' % rest[0])
        flash_brec(dev, rest[0])
    elif cmd == 'restore':
        d = open(rest[0], 'rb').read()
        first = int(rest[1]) if len(rest) > 1 else 0
        nb = (len(d) + BLK - 1) // BLK
        confirm(flags, 'write %d blocks from block %d' % (nb, first))
        order = [b for b in range(nb) if first + b > 1]          # boot record blocks last: 1, then 0
        order += [sp - first for sp in (1, 0) if first <= sp < first + nb]
        for b in order:
            write_block(dev, first + b, d[b * BLK:(b + 1) * BLK].ljust(BLK, b'\xff'))
            if b % 64 == 0:
                print('block %d/%d' % (first + b, first + nb), end='\r')
        print('restored %d blocks' % nb)
    elif cmd == 'put-slot':
        blk, path = int(rest[0]), rest[1]
        d = open(path, 'rb').read()
        if '--brec' in rest:
            if not brec_ok(d):
                sys.exit('not a valid boot record')
            confirm(flags, 'put the boot record in blocks %d and %d' % (blk, blk + 1))
            flash_brec(dev, path, (blk + 1, blk) if blk else (1, 0))
        else:
            nb = (len(d) + BLK - 1) // BLK
            confirm(flags, 'write %d bytes to blocks %d..%d' % (len(d), blk, blk + nb - 1))
            for i in range(nb):
                write_block(dev, blk + i, d[i * BLK:(i + 1) * BLK].ljust(BLK, b'\xff'))
            print('slot written: blocks %d..%d, md5 %s' % (blk, blk + nb - 1, hashlib.md5(d).hexdigest()))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
