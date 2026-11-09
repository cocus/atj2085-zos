"""Boot a firmware image from the real NAND dump in atj2085emu and log the boot milestones.

usage: emu_native.py FW OUTDIR [--overlay OVL] [--keys "T:KEY:ms,..."] [--shots "T:file.png,..."]
                     [--ticks N] [--wav W] [--stop-after-menu]
  FW       the .fw whose FWIMAGE is in the overlay (fwinject.py); used for file names in traces
  OVL      NAND overlay = fwinject.py output (FWIMAGE in the firmware area, optionally merged with
           nandinject.py user-disk files). Without it the dump's own (stock) firmware boots.
Times are absolute T-states from BREC entry (0x0200); a key with ms >= 100000 is held until released
by the run's end (e.g. "0:MENU:200000" = MENU held from power-on).

Milestones (OUTDIR/run.log + OUTDIR/milestones.json):
  brec_jp0600[n]   BREC 0x0212 `jp 0x0600` (n = 1 first load, 2 after a chain-load)
  stub_entry       0x0600 entered with the zkern stub in RAM ("ZKS1" at 0x0602)
  go_zos/go_stock  stub decision (labels from build_v04/zkern/stub_syms.json)
  zos_entry        0x0800 executed after go_zos
  kernel_entry     0x0600 entered with the stock kernel in RAM (vector page RST 18 -> jp 0x32BE)
  lcd_first_after_X  first LCD pixel write after milestone X
  backlight_on/off   port 0xEE bit 1 changes
"""
import os, sys, json, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
EMU = os.path.join(TOP, 'atj2085emu')
sys.path.insert(0, EMU)
from emu.fwimage import FwImage            # noqa: E402
from emu.soc import ATJ2085                 # noqa: E402
from emu import pngout                      # noqa: E402
from emu.dsp import DspHLE                  # noqa: E402
from emu.audio import AudioSink             # noqa: E402

DUMP = os.path.join(TOP, 'readback-my-ximpro.s1clone')
SYMS = os.path.join(TOP, 'build_v04', 'zkern', 'stub_syms.json')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fw'); ap.add_argument('out')
    ap.add_argument('--overlay', default=None)
    ap.add_argument('--keys', default='')
    ap.add_argument('--shots', default='')
    ap.add_argument('--ticks', type=int, default=400_000_000)
    ap.add_argument('--wav', default=None)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, 'run.log'), 'w')
    log = lambda s: logf.write(s + '\n')
    emu = ATJ2085(FwImage(a.fw), log=log, nand=DUMP, nand_overlay=a.overlay,
                  nand_cache=os.path.join(EMU, 'work', 'nandcache'))
    sink = AudioSink(a.wav, live=False, log=log) if a.wav else None
    emu.dsp = DspHLE(emu, sink, pacing='emu', log=log)
    mem = emu.mem
    ms = {}
    order = []

    def mark(name, extra=''):
        if name in ms:
            return
        ms[name] = emu.ticks
        order.append(name)
        log('[%09d] MILESTONE %s %s' % (emu.ticks, name, extra))

    state = {'n0212': 0, 'zos': False, 'lcd': emu.lcd.pixels_written, 'bl': None}

    def at0212():
        state['n0212'] += 1
        mark('brec_jp0600_%d' % state['n0212'])
        if bytes(mem[0x0602:0x0606]) == b'ZKS1':
            mark('stub_entry')
        elif bytes(mem[0x18:0x1b]) == bytes([0xc3, 0xbe, 0x32]):
            mark('kernel_entry', 'regs %s' % emu.regs())
        return False
    emu.add_hle(0x0212, at0212, passthru=True, name='BREC jp 0x0600')

    syms = json.load(open(SYMS)) if os.path.exists(SYMS) else {}

    def sym_hook(name):
        def f():
            if bytes(mem[0x0602:0x0606]) == b'ZKS1':
                mark(name, 'regs %s ports 05=%02x 27=%02x 4e=%02x' % (
                    emu.regs(), emu.p05, emu.ports[0x27], emu.ports[0x4e]))
                if name == 'go_zos':
                    state['zos'] = True
            return False
        return f
    for n in ('go_zos', 'go_stock'):
        if n in syms:
            emu.add_hle(syms[n], sym_hook(n), passthru=True, name=n)

    def at0800():
        if state['zos'] and 'zos_entry' not in ms:
            mark('zos_entry', 'regs %s' % emu.regs())
            log('  ports at zos entry: ' + ' '.join('%02x=%02x' % (p, emu.ports[p]) for p in range(256)))
        return False
    emu.add_hle(0x0800, at0800, passthru=True, name='zos entry')

    keys = []
    for x in filter(None, a.keys.split(',')):
        t, k, d = x.split(':')
        keys.append((int(t), k, int(d)))
    shots = [(int(t), os.path.join(a.out, f)) for t, f in (x.split(':', 1) for x in a.shots.split(',') if x)]
    step = 100_000                     # milestone / LCD granularity (T-states)
    while emu.ticks < a.ticks:
        for t, k, d in keys:
            if emu.ticks <= t < emu.ticks + step:
                if k.upper() in ('USB+', 'USB-'):
                    emu.set_usb(k.upper() == 'USB+')
                elif d >= 100000:
                    emu.key_model.hold(k)
                    log('[%09d] hold %s' % (emu.ticks, k))
                else:
                    emu.press(k, d)
                    log('[%09d] press %s %d ms' % (emu.ticks, k, d))
        r = emu.run(step)
        pw = emu.lcd.pixels_written
        if pw != state['lcd']:
            for base in ('stub_entry', 'kernel_entry', 'zos_entry'):
                if base in ms and 'lcd_first_after_' + base not in ms:
                    mark('lcd_first_after_' + base)
            state['lcd'] = pw
        bl = bool(emu.ports[0xee] & 2)
        if bl != state['bl']:
            log('[%09d] backlight %s' % (emu.ticks, 'ON' if bl else 'OFF'))
            state['bl'] = bl
        for t, f in shots:
            if emu.ticks - step < t <= emu.ticks:
                pngout.save_lcd(emu.lcd, f)
                log('[%09d] shot %s' % (emu.ticks, f))
        if r:
            log('stopped: %s' % r)
            break
    log('end pc=%04x ticks=%d regs=%s' % (emu.m.pc, emu.ticks, emu.regs()))
    log('  ports at end: ' + ' '.join('%02x=%02x' % (p, emu.ports[p]) for p in range(256)))
    log('nand=%s' % emu.nand.stats())
    log('dsp=%s' % (emu.dsp.stats,))
    for t, ev in emu.dsp.history[-40:]:
        log('  %9d %s' % (t, ev))
    log('--- FW reads (last 40): tick caller off len dest sel file+off')
    for t, c, off, n, dest, sel, fname, foff in list(emu.fwreads)[-40:]:
        log('  %9d %04x %06x %04x %04x %02x %s+%x' % (t, c, off, n, dest, sel, fname, foff))
    if sink is not None:
        sink.close()
    pngout.save_lcd(emu.lcd, os.path.join(a.out, 'end.png'))
    json.dump({k: ms[k] for k in order}, open(os.path.join(a.out, 'milestones.json'), 'w'), indent=1)
    logf.close()
    for k in order:
        print('%-28s %11d' % (k, ms[k]))


if __name__ == '__main__':
    main()
