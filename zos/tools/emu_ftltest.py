"""zos FTL write path test in atj2085emu on the real NAND dump (modules/ftltest + modules/common/ftlw.inc).

usage: lisong_rebuild/.venv/Scripts/python.exe zos/tools/emu_ftltest.py [OUTDIR] [--no-stock] [--bin=DIR]
(build first: python zos/build.py --no-deploy; default OUTDIR zos/out/ftltest)

1. nandinject.py puts FTLTEST.ZM on the user disk (overlay), the stock firmware boots from the dump
   (its mount scan erases the temp units and rewrites the tables, as on the device), ZOS.AP is
   injected at 300 M T-states, PLAY in the module list runs FTLTEST: writes into free clusters, flushes, reads
   back (results in ZRAM1 at 0x3400, see modules/ftltest/main.c ftres_t).
2. The NAND state right before zos (before.ovl) and at the end (after.ovl) are saved and checked
   with lisong_rebuild/tools/ftlread.py: mapping tables unchanged, every sector of the touched units
   = before (not written) or the test pattern with the exact stock spare (written), temp units
   erased, nothing else changed, FAT check, spare/table consistency (ftlread info), and the
   emulator's NAND rule checks (no program over programmed bytes, NOP <= 4, page order).
3. (unless --no-stock) the STOCK firmware boots from after.ovl: it must reach the same screen as
   from before.ovl (mount scan accepts the result); its NAND state is checked again (stock.ovl).
Exit code 0 = all checks passed.
"""
import os, sys, struct, subprocess, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
EMU = os.path.join(ROOT, 'atj2085emu')
TOOLS = os.path.join(ROOT, 'lisong_rebuild', 'tools')
sys.path.insert(0, EMU)
sys.path.insert(0, TOOLS)
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu import pngout
from ftlread import Ftl, Fat, fatcheck, ecc512, UD_FAT_BASE, FW_UNITS, load_overlay, RAW

DUMP = os.path.join(ROOT, 'readback-my-ximpro.s1clone')
FW = os.path.join(ROOT, 'my-ximpro.fw')
CACHE = os.path.join(EMU, 'work', 'nandcache')
OUTB = os.path.join(HERE, '..', 'out', 'atj2085')
RES = 0x3400                       # ftres_t (HAL_MODRAM_BASE)
MAXW = 48
fails = []


def check(cond, what):
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond:
        fails.append(what)


def pat(lba, seed):
    v = ((lba & 0xFF) * 31 + seed) & 0xFF
    b = bytearray(((i * 7) + v) & 0xFF for i in range(512))
    b[0:4] = struct.pack('<I', lba)
    b[4] = seed
    return bytes(b)


def new_emu(ovl, logf):
    return ATJ2085(FwImage(FW), log=lambda s: logf.write(s + '\n'), nand=DUMP, nand_overlay=ovl,
                   nand_cache=CACHE)


def run_until(emu, t, step=1_000_000):
    while emu.ticks < t:
        emu.run(step)


def chip(emu):
    return emu.nand.chips[1]


def rules(emu):
    s = chip(emu).stats
    return {k: s[k] for k in ('programs', 'erases', 'overprogram', 'nop_over', 'out_of_order')}


def zos_run(out):
    for n in ('ZOS.AP', 'FTLTEST.ZM'):
        if os.path.abspath(OUTB) != out:
            shutil.copy(os.path.join(OUTB, n), os.path.join(out, n))
    inj = os.path.join(out, 'inject.ovl')
    subprocess.run([sys.executable, os.path.join(TOOLS, 'nandinject.py'), DUMP, inj,
                    os.path.join(out, 'FTLTEST.ZM')], check=True)
    logf = open(os.path.join(out, 'run.log'), 'w')
    emu = new_emu(inj, logf)
    run_until(emu, 300_000_000)
    r0 = rules(emu)
    print('stock boot: NAND %s' % r0)
    check(r0['overprogram'] == r0['nop_over'] == r0['out_of_order'] == 0,
          'stock boot obeys the NAND rules (model check baseline)')
    emu.nand.save_overlay(os.path.join(out, 'before.ovl'))
    ap = open(os.path.join(out, 'ZOS.AP'), 'rb').read()
    ln, addr = struct.unpack_from('<HH', ap, 12)
    m = emu.m
    emu.mem[addr:addr + ln] = ap[0x800:0x800 + ln]
    sp = (m.sp - 4) & 0xFFFF
    emu.mem[sp:sp + 4] = bytes(4)
    m.sp, m.pc = sp, 0x0800
    t = emu.ticks
    run_until(emu, t + 40_000_000)                         # module list (no SHELL.ZM): FTLTEST
    emu.press('PLAY', 120)
    t0 = emu.ticks
    stage = -1
    while emu.ticks - t0 < 6_000_000_000:
        emu.run(2_000_000)
        r = bytes(emu.mem[RES:RES + 262])
        if r[4] != stage:
            stage = r[4]
            print('  stage %d at %.0f M' % (stage, (emu.ticks - t0) / 1e6))
        if r[:4] == b'FTLT' or stage == 6:
            break
    pngout.save_lcd(emu.lcd, os.path.join(out, 'ftltest.png'))
    emu.nand.save_overlay(os.path.join(out, 'after.ovl'))
    r1 = rules(emu)
    print('zos run: NAND %s, %.0f M T-states' % (r1, (emu.ticks - t0) / 1e6))
    v = chip(emu).violations
    for k, txt in v[:20]:
        print('    RULE %s: %s' % (k, txt))
    check(r1['overprogram'] == r1['nop_over'] == r1['out_of_order'] == 0,
          'zos writes obey the NAND rules: no program over programmed bytes, NOP <= 4, page order')
    logf.write('end pc=%04x ticks=%d nand=%s\n' % (emu.m.pc, emu.ticks, emu.nand.stats()))
    logf.close()
    return r, r1['programs'] - r0['programs'], r1['erases'] - r0['erases']


def parse(r):
    magic, stage, nfail, err, nw, cmp_bad, coh_bad, boot_bad, refuse_bad = struct.unpack_from('<4s8B', r, 0)
    lba_a, lba_x = struct.unpack_from('<II', r, 12)
    wl = struct.unpack_from('<%dI' % MAXW, r, 20)
    ws = r[20 + 4 * MAXW:20 + 5 * MAXW]
    ticks, = struct.unpack_from('<H', r, 20 + 5 * MAXW)
    return dict(magic=magic, stage=stage, nfail=nfail, err=err, nw=nw, cmp_bad=cmp_bad, coh_bad=coh_bad,
                boot_bad=boot_bad, refuse_bad=refuse_bad, lba_a=lba_a, lba_x=lba_x,
                writes=[(wl[i], ws[i]) for i in range(min(nw, MAXW))], ticks=ticks)


def unit_of(lba):
    return FW_UNITS + ((UD_FAT_BASE + lba) >> 9)


def verify_state(out, ovl, ref_ovl, writes, tag):
    """the NAND state `ovl` against `ref_ovl` (state before the zos writes)"""
    b = Ftl(DUMP, CACHE, ref_ovl)
    a = Ftl(DUMP, CACHE, ovl)
    check(all(a.tables[z][:0x7FF] == b.tables[z][:0x7FF] for z in range(a.zones)),
          '%s: mapping tables (logical units, free list, temp unit) unchanged' % tag)
    final = {}
    for lba, seed in writes:
        final[lba] = seed
    units = sorted({unit_of(l) for l in final})
    nwr = nsame = 0
    bad = []
    for U in units:
        z, L = divmod(U, a.luz)
        for s in range(512):
            page, col = a.sector_loc(U, s)
            ra, rb = a.c.read_raw(page), b.c.read_raw(page)
            g = 0x800 + (s & 3) * 16
            da, sa = ra[col:col + 512], ra[g:g + 16]
            lba = (U - FW_UNITS) * 512 + s - UD_FAT_BASE
            if lba in final:
                want = pat(lba, final[lba]) if final[lba] else rb[col:col + 512]
                spare = b'\xff\xff' + struct.pack('<H', L) + b'\xff' * 4 + ecc512(want) + b'\xff\xff\x55\xff'
                if da != want or sa != spare:
                    bad.append('unit %X sector %d (lba %d): %s' % (U, s, lba, 'data' if da != want else 'spare ' + sa.hex()))
                nwr += 1
            else:
                if da != rb[col:col + 512] or sa != rb[g:g + 16]:
                    bad.append('unit %X sector %d: changed but not written' % (U, s))
                nsame += 1
    for x in bad[:10]:
        print('    ' + x)
    check(not bad, '%s: units %s: %d written sectors = pattern + stock spare (FF FF L FFx4 ECC FF FF 55 FF), '
          '%d other sectors identical (data + spare)' % (tag, ' '.join('%X' % u for u in units), nwr, nsame))
    # temp units erased, nothing else changed
    temp_blocks = set()
    for z in range(a.zones):
        t = a.tables[z][0x7FD]
        temp_blocks |= {(z * a.upz + t) * 2, (z * a.upz + t) * 2 + 1}
    ok = all(a.c.read_raw(blk * a.ppb + p) == b'\xff' * RAW for blk in temp_blocks for p in range(a.ppb))
    check(ok, '%s: temp units erased (blocks %s)' % (tag, ' '.join('%X' % x for x in sorted(temp_blocks))))
    ea, pa = load_overlay(ovl)
    eb, pb = load_overlay(ref_ovl)
    blocks = {p // a.ppb for p in set(pa) | set(pb)} | (ea ^ eb)
    allowed = temp_blocks | {(a.unit_phys(U)[0] * a.upz + a.unit_phys(U)[1]) * 2 + i for U in units for i in (0, 1)}
    other = []
    for blk in sorted(blocks - allowed):
        if any(a.c.read_raw(blk * a.ppb + p) != b.c.read_raw(blk * a.ppb + p) for p in range(a.ppb)):
            other.append(blk)
    check(not other, '%s: no other block changed (%s)' % (tag, ' '.join('%X' % x for x in other[:20]) or 'none'))
    return a


def ftlread(ovl, cmd):
    r = subprocess.run([sys.executable, os.path.join(TOOLS, 'ftlread.py'), DUMP, cmd, '--overlay', ovl],
                       capture_output=True, text=True)
    return r.stdout


def tool_checks(ovl, tag):
    info = ftlread(ovl, 'info')
    print('\n'.join('    ' + l for l in info.splitlines()))
    check(info.count('spare logical number mismatches: 0') == 2 and info.count('sig 0x55AA') == 2,
          '%s: ftlread info: both tables valid (55AA), 0 spare/table mismatches' % tag)
    fc = ftlread(ovl, 'fatcheck')
    print('\n'.join('    ' + l for l in fc.splitlines()))
    check('problems: 0' in fc and 'fat_copies_equal: True' in fc and 'lost_clusters: 0' in fc,
          '%s: ftlread fatcheck: 0 problems, 0 lost clusters, FAT copies equal' % tag)


def stock_boot(out, ovl, name, ticks=420_000_000):
    logf = open(os.path.join(out, name + '.log'), 'w')
    emu = new_emu(ovl, logf)
    shots = []
    for t in (300_000_000, ticks):
        run_until(emu, t)
        f = os.path.join(out, '%s_%dM.png' % (name, t // 1_000_000))
        pngout.save_lcd(emu.lcd, f)
        shots.append(open(f, 'rb').read())
    emu.nand.save_overlay(os.path.join(out, name + '.ovl'))
    pc = emu.m.pc
    logf.close()
    return shots, pc, rules(emu)


def main():
    global OUTB
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    for a in sys.argv[1:]:
        if a.startswith('--bin='):                  # ZOS.AP + FTLTEST.ZM from here (out/ is shared)
            OUTB = a[6:]
    out = os.path.abspath(args[0] if args else os.path.join(HERE, '..', 'out', 'ftltest'))
    os.makedirs(out, exist_ok=True)
    print('zos FTLTEST run')
    raw, nprog, nerase = zos_run(out)
    r = parse(raw)
    print('  result: %s' % {k: v for k, v in r.items() if k != 'writes'})
    print('  writes: %s' % ' '.join('%d/%d' % w for w in r['writes']))
    print('  NAND ops by zos: %d programs, %d erases' % (nprog, nerase))
    check(r['magic'] == b'FTLT' and r['nfail'] == 0, 'FTLTEST finished, module reports ALL OK (writes + 7 unit closes: %.2f s emulated)'
          % (r['ticks'] / 100))
    check(r['err'] == 0 and r['cmp_bad'] == 0, 'no write error, read-back of %d written LBAs = pattern' % len(r['writes']))
    check(r['coh_bad'] == 0, 'ftlw_sync + hal_blk_read sees the new data of the open unit')
    check(r['refuse_bad'] == 0, 'refused: LBA >= 2^23 (ERANGE), LBA in a missing zone (EMAP), nothing left open')
    check(unit_of(r['lba_x']) != unit_of(r['lba_x'] + 31), 'cluster X straddles units %X/%X'
          % (unit_of(r['lba_x']), unit_of(r['lba_x'] + 31)))
    before, after = os.path.join(out, 'before.ovl'), os.path.join(out, 'after.ovl')
    verify_state(out, after, before, r['writes'], 'after zos')
    tool_checks(after, 'after zos')
    if '--no-stock' not in sys.argv:
        print('stock firmware boot on the zos-written NAND (reference: the state before zos)')
        ref, pc_ref, _ = stock_boot(out, before, 'stock_ref')
        got, pc, rl = stock_boot(out, after, 'stock_after')
        check(got == ref, 'stock boot from after.ovl: screens at 300 M / 420 M identical to the reference boot')
        check(not (0x0412 <= pc <= 0x042C), 'stock boot not in the kernel error handler (pc %04X)' % pc)
        check(rl['overprogram'] == rl['nop_over'] == rl['out_of_order'] == 0, 'stock boot: NAND rules ok %s' % rl)
        verify_state(out, os.path.join(out, 'stock_after.ovl'), os.path.join(out, 'stock_ref.ovl'),
                     r['writes'], 'after stock boot (vs the reference boot)')
        tool_checks(os.path.join(out, 'stock_after.ovl'), 'after stock boot')
    print('%d checks failed' % len(fails) if fails else 'all checks passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
