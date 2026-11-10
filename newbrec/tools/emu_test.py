#!/usr/bin/env python3
"""End-to-end emulator test of the newbrec image (BROM model -> menu -> USB raw NAND disk).

usage: python emu_test.py [NEWBREC.BIN] [OUTDIR]       (run with the lisong_rebuild venv python)
Needs the untracked prerequisites (see emuboot.py). Exit code 0 = all checks passed. Screenshots in OUTDIR.
Checks: BROM accepts the image; menu + key navigation; 'not in M1' items; USB mode: enumeration,
INQUIRY / READ CAPACITY, raw READ(10) = the emulated chip's pages, WRITE(10) erase-on-first-sector with ECC +
spare format verified against the chip model, rewrite, nandtool dump / flash-brec / put-slot / erase on the
emulated disk, eject, cable pull -> back to the menu."""
import os, struct, sys, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emuboot
from emutransport import EmuTransport
from emu.nand import NandChip, ecc512
import nandtool

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'out', 'NEWBREC.BIN')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'out', 'emutest')
fails = []


def check(c, what):
    print(('  ok   ' if c else '  FAIL ') + what)
    if not c:
        fails.append(what)


def chip_sector(emu, lba):
    c = emu.nand.chips[1]
    page = bytes(c.page_data(lba >> 2))
    s = lba & 3
    return page[s * 512:(s + 1) * 512], page[0x800 + 16 * s:0x800 + 16 * s + 16]


brec = open(BIN, 'rb').read()
check(len(brec) == 0x4000 and nandtool.brec_ok(brec), 'image: 16384 bytes, 0x55AA, sum')
check(brec[:8] == bytes.fromhex('c300020042524543') and brec[0x200:0x215].hex() == '310002dbdff601d3dfcd121bcd1602310004c30006',
      'header bytes 0-7 and entry code 0x200-0x214 = installed stock BREC family')
# --- menu part: boot, keys
os.makedirs(OUT, exist_ok=True)
emu = emuboot.boot(BIN, os.path.join(OUT, 'nand.ovl'))
check(emu.brom_block == 0, 'BROM model accepted the image from block 0')
emu.run(40_000_000)
emuboot.shot(emu, os.path.join(OUT, '1_menu.png'))
a, b = emu.lcd.pixels_written, emu.lcd.cmd_hist.get(0xAF, 0)
check(b >= 1, 'LCD init sequence ran (display on seen), %d pixels written' % a)


def press(k, ms=150):
    emu.press(k, ms)
    emu.run(ms * 12000 + 6_000_000)


press('RIGHT')
emuboot.shot(emu, os.path.join(OUT, '2_right.png'))
press('PLAY')
emuboot.shot(emu, os.path.join(OUT, '3_stock_not_m1.png'))
press('LEFT')
press('LEFT')                       # wraps to the last item
emuboot.shot(emu, os.path.join(OUT, '4_wrap.png'))
press('RIGHT')                      # back to USB
sp = [i for i in range(0x80, 0x200) if emu.mem[i] != 0x76]
print('stack low water mark: %#06x (stack top 0x0200)' % (min(sp) if sp else 0x200))
del emu

# --- USB part
t = EmuTransport(BIN, OUT)
emu, h = t.emu, t.host
t.shot('5_usb_enumerated.png')
check(h.addr == 0x87 and bytes(t.vid_pid) == bytes([9, 0x12, 1, 0]), 'enumerated: address 7, VID:PID 1209:0001')
st, d = t.scsi([0x12, 0, 0, 0, 36, 0], 36)
check(st == 0 and d[:8] == bytes([0, 0x80, 2, 2, 31, 0, 0, 0]) and d[16:32] == b'NEWBREC RAW NAND', 'INQUIRY')
st, d = t.scsi([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8)
last = struct.unpack('>I', d[:4])[0]
check(st == 0 and last == 8192 * 256 - 1 and d[4:] == struct.pack('>I', 512), 'READ CAPACITY = %d sectors' % (last + 1))
st, _ = t.scsi([0, 0, 0, 0, 0, 0])
check(st == 0, 'TEST UNIT READY')
st, d = t.scsi([0x1A, 0, 0x3F, 0, 4, 0], 4)
check(st == 0 and d[2] == 0, 'MODE SENSE(6): not write protected')


def rd(lba, n):
    st, d = t.scsi([0x28, 0] + list(struct.pack('>I', lba)) + [0, n >> 8, n & 255, 0], n * 512)
    return st, d


for lba, n in ((0, 8), (2, 7), (100 * 256 + 3, 9), (8191 * 256, 5), (4 * 256, 1)):
    st, d = rd(lba, n)
    ref = b''.join(chip_sector(emu, lba + i)[0] for i in range(n))
    check(st == 0 and d == ref, 'READ(10) lba %d x %d = chip data columns' % (lba, n))
st, d = rd(0, 32)
check(nandtool.brec_ok(d[:0x4000]), 'sectors 0..31 = the boot record that is running (0x55AA + sum)')

# raw write: block 4 (free in the dump), 3 pages ascending, first sector erases
import random
random.seed(5)
data = bytes(random.randrange(256) for _ in range(12 * 512))
lba = 4 * 256
st, _ = t.scsi([0x2A, 0] + list(struct.pack('>I', lba)) + [0, 0, 12, 0], data_out=data)
check(st == 0, 'WRITE(10) block 4, 12 sectors (3 pages)')
ok = True
for i in range(12):
    dd, sp_ = chip_sector(emu, lba + i)
    e = ecc512(data[i * 512:(i + 1) * 512])
    ok = ok and dd == data[i * 512:(i + 1) * 512] and sp_ == b'\xff' * 8 + bytes(e) + b'\xff\xff\x55\xff'
check(ok, 'chip: data columns + spare = FF x8, ECC x4, FF FF 55 FF for every sector')
st, d = rd(lba, 12)
check(st == 0 and d == data, 'READ(10) back = written data')
c = emu.nand.chips[1]
check(bytes(c.page_data(4 * 64 + 3)) == b'\xff' * 0x840, 'page 3 of the block still erased')
# rewrite (new erase)
data2 = bytes(random.randrange(256) for _ in range(4 * 512))
st, _ = t.scsi([0x2A, 0] + list(struct.pack('>I', lba)) + [0, 0, 4, 0], data_out=data2)
st2, d = rd(lba, 8)
check(st == 0 and st2 == 0 and d[:2048] == data2 and d[2048:] == b'\xff' * 2048, 'rewrite of the block: erase first, old pages gone')
# neighbours untouched
st, d = rd(0, 32)
check(nandtool.brec_ok(d[:0x4000]), 'block 0 untouched')
# out of range
st, _ = t.scsi([0x28, 0] + list(struct.pack('>I', 8192 * 256)) + [0, 0, 1, 0], 512)
check(st == 1, 'READ beyond the end -> CHECK CONDITION')
st, d = t.scsi([0x03, 0, 0, 0, 18, 0], 18)
check(d[2] == 5 and d[12] == 0x21, 'REQUEST SENSE: ILLEGAL REQUEST / LBA out of range')
t.shot('6_usb_after_io.png')

# nandtool on the emulated disk
class Wrap:     # nandtool EmuDev interface on the already running transport
    def __init__(self, t):
        self.t = t
        st, d = t.scsi([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8)
        self.n = struct.unpack('>I', d[:4])[0] + 1
    sectors = nandtool.EmuDev.sectors
    read = nandtool.EmuDev.read
    write = nandtool.EmuDev.write


dev = Wrap(t)
nandtool.cmd_info(dev)
# flash-brec of the image itself to blocks 1, 0 (identical content) and of a modified copy to blocks 2, 3
mod = bytearray(brec)
mod[0x3000] ^= 0x55
struct.pack_into('<H', mod, 0x3FFE, nandtool.brec_sum(mod) if hasattr(nandtool, 'brec_sum') else sum(struct.unpack_from('<8191H', mod)) & 0xFFFF)
open(os.path.join(OUT, 'brec_mod.bin'), 'wb').write(mod)
nandtool.flash_brec(dev, os.path.join(OUT, 'brec_mod.bin'), (3, 2))
b2 = nandtool.read_block(dev, 2)[:0x4000]
b3 = nandtool.read_block(dev, 3)[:0x4000]
check(b2 == bytes(mod) and b3 == bytes(mod), 'flash-brec: blocks 3 then 2 verified = file')
dd0, sp0 = chip_sector(emu, 2 * 256)
check(sp0 == b'\xff' * 8 + bytes(ecc512(dd0)) + b'\xff\xff\x55\xff', 'flash-brec spare format = stock BREC format')
check(all(bytes(c.page_data(2 * 64 + p)) == b'\xff' * 0x840 for p in range(8, 64)), 'flash-brec: pages 8-63 of the block erased')
blk = os.path.join(OUT, 'blk.bin')
nandtool.cmd_dump(dev, blk, 2, 2)
check(open(blk, 'rb').read()[:0x4000] == bytes(mod), 'dump of blocks 2-3 holds the written boot record')
nandtool.cmd_erase(dev, 2, 2)
check(nandtool.read_block(dev, 2) == b'\xff' * nandtool.BLK and nandtool.read_block(dev, 3) == b'\xff' * nandtool.BLK,
      'erase: blocks 2 and 3 read FF')
payload = bytes(random.randrange(256) for _ in range(200_000))
open(os.path.join(OUT, 'slot.bin'), 'wb').write(payload)
for i in range(2):
    nandtool.write_block(dev, 10 + i, payload[i * nandtool.BLK:(i + 1) * nandtool.BLK].ljust(nandtool.BLK, b'\xff'))
got = nandtool.read_block(dev, 10) + nandtool.read_block(dev, 11)
check(got[:len(payload)] == payload, 'put-slot style write of 200000 bytes over 2 blocks')

# eject, then the cable is pulled -> menu
st, _ = t.scsi([0x1B, 0, 0, 0, 2, 0])
st2, _ = t.scsi([0, 0, 0, 0, 0, 0])
check(st == 0 and st2 == 1, 'START STOP UNIT (eject), then TEST UNIT READY fails (medium not present)')
t.emu.run(40_000_000)
t.shot('7_usb_ejected.png')
t.emu.set_usb(False)
t.emu.run(60_000_000)
t.shot('8_after_unplug.png')
sp = [i for i in range(0x80, 0x200) if emu.mem[i] != 0x76]
print('stack low water mark (USB run): %#06x' % (min(sp) if sp else 0x200))
nand_stats = emu.nand.stats()
print('NAND stats', nand_stats)
print('RESULT:', 'ALL PASSED' if not fails else '%d FAILED: %s' % (len(fails), fails))
sys.exit(1 if fails else 0)
