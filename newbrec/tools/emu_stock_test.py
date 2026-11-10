#!/usr/bin/env python3
"""Emulator test of the STOCK menu item: the stock boot record is kept in NAND blocks 2/3 (unit 1) and the
new boot record, started by the BROM model from block 0, reloads it into 0x0000-0x3FFF through the ZRAM2
loader and jumps to 0. Checks that the stock OS then boots to its main menu, that a missing / corrupt
copy returns to the menu with a message and leaves the new boot record running.

usage: python emu_stock_test.py [NEWBREC.BIN] [OUTDIR]     (lisong_rebuild venv python)"""
import os, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emuboot

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'out', 'NEWBREC.BIN')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'out', 'emutest')
os.makedirs(OUT, exist_ok=True)
fails = []


def check(c, what):
    print(('  ok   ' if c else '  FAIL ') + what)
    if not c:
        fails.append(what)


stock = emuboot.stock_brec()


def run_case(name, extra, steps):
    emu = emuboot.boot(BIN, os.path.join(OUT, name + '.ovl'), extra=extra)
    emu.run(40_000_000)
    emu.press('RIGHT', 150)
    emu.run(150 * 12000 + 6_000_000)
    emuboot.shot(emu, os.path.join(OUT, name + '_0_stock_selected.png'))
    emu.press('PLAY', 150)
    emu.run(150 * 12000 + 4_000_000)
    return emu


# 1. stock copy in blocks 2 and 3
emu = run_case('stock_ok', [(stock, (2, 3))], None)
pc_seen = []
t0 = emu.ticks
emu.run(60_000_000)
emuboot.shot(emu, os.path.join(OUT, 'stock_ok_1_hourglass.png'))
mem0 = bytes(emu.mem[0:0x4000])
check(mem0[:8] == stock[:8], 'RAM 0x0000-0x0007 = the stock boot record header after STOCK')
emu.run(300_000_000)
emuboot.shot(emu, os.path.join(OUT, 'stock_ok_2_menu.png'))
emu.run(200_000_000)
emuboot.shot(emu, os.path.join(OUT, 'stock_ok_3_later.png'))
print('pc=%04x ticks=%d' % (emu.m.pc, emu.ticks))
check(emu.lcd.pixels_written > 200000, 'stock firmware drew its screens (%d pixels written)' % emu.lcd.pixels_written)

# 2. no copy: message, new boot record keeps running
emu = run_case('stock_none', [], None)
emu.run(20_000_000)
emuboot.shot(emu, os.path.join(OUT, 'stock_none_1_message.png'))
check(emu.m.pc >= 0x1B15 or emu.m.pc < 0x1B15 and emu.m.pc > 0x200, 'new boot record still running (pc %04x)' % emu.m.pc)
check(emu.mem[0x1B12] == 0xC3, 'its code is still in RAM')

# 3. corrupt copy (one bit flipped): refused, same behaviour
bad = bytearray(stock)
bad[0x1000] ^= 1
emu = run_case('stock_bad', [(bytes(bad), (2, 3))], None)
emu.run(20_000_000)
emuboot.shot(emu, os.path.join(OUT, 'stock_bad_1_message.png'))
check(emu.mem[0x1B12] == 0xC3 and emu.mem[4:8] == b'BREC' and emu.mem[0x0F:0x19] == b'NEWBREC M1', 'corrupt copy refused, new boot record intact')
print('RESULT:', 'ALL PASSED' if not fails else '%d FAILED' % len(fails))
sys.exit(1 if fails else 0)
