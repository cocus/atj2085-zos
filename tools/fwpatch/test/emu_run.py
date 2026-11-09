"""Boot a zospatch-patched firmware on the real NAND dump in atj2085emu (development test only).

usage: emu_run.py FW OUTDIR --overlay OVL [--keys "T:KEY:ms,..."] [--usb] [--ticks N] [--shots "T:f.png,..."]
  OVL   tools/fwinject.py output (FWIMAGE in the firmware area, merged with nandinject.py files)
  --usb USB cable plugged from power-on (VBUS escape test)
Writes OUTDIR/run.log, OUTDIR/result.json {milestones, decision, sums} and the screenshots.
Decision = first of: go_zos / go_stock / reboot (stub labels from zosboot.sym).
"""
import os, sys, json, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
FWP = os.path.dirname(HERE)
TOP = os.path.abspath(os.path.join(FWP, '..', '..'))
sys.path.insert(0, os.path.join(TOP, 'atj2085emu'))
sys.path.insert(0, FWP)
from emu.fwimage import FwImage            # noqa: E402
from emu.soc import ATJ2085                 # noqa: E402
from emu import pngout                      # noqa: E402
from emu.dsp import DspHLE                  # noqa: E402
import zospatch as Z                        # noqa: E402

DUMP = os.path.join(TOP, 'readback-my-ximpro.s1clone')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fw'); ap.add_argument('out')
    ap.add_argument('--overlay', required=True)
    ap.add_argument('--keys', default='')
    ap.add_argument('--usb', action='store_true')
    ap.add_argument('--zosap', default=None, help='ZOS.AP on the disk: compare with RAM at zos entry')
    ap.add_argument('--shots', default='')
    ap.add_argument('--ticks', type=int, default=260_000_000)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, 'run.log'), 'w')
    log = lambda s: logf.write(s + '\n')
    emu = ATJ2085(FwImage(a.fw), log=log, nand=DUMP, nand_overlay=a.overlay,
                  nand_cache=os.path.join(TOP, 'atj2085emu', 'work', 'nandcache'))
    emu.dsp = DspHLE(emu, None, pacing='emu', log=log)
    mem = emu.mem
    syms = json.load(open(os.path.join(FWP, 'zosboot.sym')))
    res = dict(milestones={}, decision=None, sums={})
    ms = res['milestones']

    def mark(name):
        if name not in ms:
            ms[name] = emu.ticks
            log('[%09d] MILESTONE %s' % (emu.ticks, name))

    def is_stub():
        return bytes(mem[0x0603:0x060B]) == b'ZOSBOOT1'

    def at0212():
        if is_stub():
            mark('stub_entry')
            blob = bytes(mem[0:0x4000])
            res['sums'] = dict(loader_ram=Z.sum8_16(blob, Z.LOADER_RANGES), reader_ram=Z.sum8_16(blob, Z.READER_RANGES),
                               upz='%#x' % (mem[0x3CAA] | mem[0x3CAB] << 8))
        elif bytes(mem[0x18:0x1b]) == bytes([0xc3, 0xbe, 0x32]):
            mark('kernel_entry')
        return False
    emu.add_hle(0x0212, at0212, passthru=True, name='BREC jp 0x0600')

    def dec(name):
        def f():
            if is_stub():
                mark(name)
                if res['decision'] is None and name != 'try_zos':
                    res['decision'] = name
            return False
        return f
    for n in ('go_zos', 'go_stock', 'reboot', 'try_zos'):
        emu.add_hle(syms[n], dec(n), passthru=True, name=n)

    def at0800():
        if res['decision'] == 'go_zos' and 'zos_entry' not in ms:
            mark('zos_entry')
            if a.zosap:
                ap = open(a.zosap, 'rb').read()
                n = ap[12] | ap[13] << 8
                res['image_ok'] = bytes(mem[0x0800:0x0800 + n]) == ap[0x800:0x800 + n]
                res['image_len'] = n
        return False
    emu.add_hle(0x0800, at0800, passthru=True, name='zos entry')

    keys = []
    for x in filter(None, a.keys.split(',')):
        t, k, d = x.split(':')
        keys.append((int(t), k, int(d)))
    if a.usb:
        emu.set_usb(True)
    shots = [(int(t), os.path.join(a.out, f)) for t, f in (x.split(':', 1) for x in a.shots.split(',') if x)]
    step = 100_000
    lcd0 = emu.lcd.pixels_written
    while emu.ticks < a.ticks:
        for t, k, d in keys:
            if emu.ticks <= t < emu.ticks + step:
                if d >= 100000:
                    emu.key_model.hold(k)
                else:
                    emu.press(k, d)
                log('[%09d] key %s %d' % (emu.ticks, k, d))
        r = emu.run(step)
        if emu.lcd.pixels_written != lcd0:
            for base in ('kernel_entry', 'zos_entry'):
                if base in ms:
                    mark('lcd_after_' + base)
            lcd0 = emu.lcd.pixels_written
        for t, f in shots:
            if emu.ticks - step < t <= emu.ticks:
                pngout.save_lcd(emu.lcd, f)
        if r:
            log('stopped: %s' % r)
            break
    pngout.save_lcd(emu.lcd, os.path.join(a.out, 'end.png'))
    log('end pc=%04x ticks=%d regs=%s' % (emu.m.pc, emu.ticks, emu.regs()))
    for t, c, off, n, dest, sel, fname, foff in list(emu.fwreads)[-60:]:
        log('  fwread %9d %04x %08x %04x -> %04x %s+%x' % (t, c, off, n, dest, fname, foff))
    json.dump(res, open(os.path.join(a.out, 'result.json'), 'w'), indent=1)
    print(json.dumps(res))


if __name__ == '__main__':
    main()
