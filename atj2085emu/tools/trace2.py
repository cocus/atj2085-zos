"""trace2.py --at TICKS [--keys ..] [--bp HEX [--hit N]] [--n N] [--disk X]: run with key script to TICKS,
then optionally to breakpoint, then single-step N instructions printing each (first 3 visits/addr)."""
import argparse, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu.disk import UserDisk
from emu.dsp import DspHLE
ap = argparse.ArgumentParser()
ap.add_argument('--at', type=int, default=0)
ap.add_argument('--keys', default='300000000:MENU:200')
ap.add_argument('--bp', default=None)
ap.add_argument('--hit', type=int, default=1)
ap.add_argument('--n', type=int, default=200)
ap.add_argument('--max', type=int, default=3)
ap.add_argument('--disk', default=None)
ap.add_argument('--verbose', action='store_true')
a = ap.parse_args()
kw = {'disk': UserDisk.open(a.disk)} if a.disk else {}
e = ATJ2085(FwImage(os.path.join(HERE, '..', '..', 'my-ximpro.fw')), log=(print if a.verbose else (lambda s: None)), **kw)
(setattr(e, 'dsp', DspHLE(e, None)) if 'e' in dir() else setattr(emu, 'dsp', DspHLE(emu, None)))
script = [(int(t), k, int(ms)) for t, k, ms in (x.split(':') for x in a.keys.split(',') if x)]
step = 1_000_000
while e.ticks < a.at:
    for (t, k, ms) in script:
        if e.ticks <= t < e.ticks + step:
            e.set_usb(k == 'USB+') if k in ('USB+', 'USB-') else e.press(k, ms)
    e.run(min(step, a.at - e.ticks))
if a.bp:
    bp = int(a.bp, 16)
    e.user_bps.add(bp); e.m.set_breakpoint(bp)
    for i in range(a.hit):
        r = None
        while r is None:
            for (t, k, ms) in script:
                if e.ticks <= t < e.ticks + step:
                    e.set_usb(k == 'USB+') if k in ('USB+', 'USB-') else e.press(k, ms)
            r = e.run(step)
        if i < a.hit - 1:
            e.m.step_over_breakpoint()
    e.user_bps.discard(bp)
    if bp not in e.bp_handlers: e.m.clear_breakpoint(bp)
    print('hit bp at tick', e.ticks)
seen = {}
for _ in range(a.n):
    ad, h, t = e.disasm(e.m.pc, 1)[0]
    seen[ad] = seen.get(ad, 0) + 1
    r = e.m
    if seen[ad] <= a.max:
        print('%04x %-10s %-24s af=%04x bc=%04x de=%04x hl=%04x ix=%04x iy=%04x sp=%04x' % (ad, h, t, r.af, r.bc, r.de, r.hl, r.ix, r.iy, r.sp))
    e.step()
