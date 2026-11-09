"""frames.py START STEP N [KEY@TICK ...] : save N LCD frames to scratch dir."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu import pngout
out = os.environ.get('OUT', '.')
e = ATJ2085(FwImage(os.path.join(os.path.dirname(__file__), '..', '..', 'my-ximpro.fw')), log=lambda s: None)
ev = sorted((int(x.split('@')[1]), x.split('@')[0]) for x in sys.argv[4:])
start, step, n = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
t = 0
for i in range(n):
    target = start + i * step
    while e.ticks < target:
        nxt = min([tt for tt, _ in ev if tt > e.ticks] + [target])
        e.run(max(1, nxt - e.ticks))
        for tt, k in ev:
            if tt <= e.ticks and (tt, k) not in getattr(e, '_done', set()):
                e._done = getattr(e, '_done', set()) | {(tt, k)}; e.press(k, 200)
    pngout.save_lcd(e.lcd, os.path.join(out, 'f%02d.png' % i), scale=1)
    print(i, e.ticks, '%04x' % e.m.pc)
