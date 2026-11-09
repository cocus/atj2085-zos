"""trace.py UNTIL N [skip_loops]: run until PC==UNTIL (hex) or ticks (decimal, prefix t), then step N instrs."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
fw = os.path.join(os.path.dirname(__file__), '..', '..', 'my-ximpro.fw')
quiet = os.environ.get('QUIET')
e = ATJ2085(FwImage(fw), log=lambda s: None if quiet else print(s))
u = sys.argv[1]
hit = int(os.environ.get('HIT', '1'))
if u.startswith('t'):
    e.run(int(u[1:]))
else:
    e.user_bps.add(int(u, 16)); e.m.set_breakpoint(int(u, 16))
    for _ in range(hit):
        while e.run(10**9) is None: pass
        if _ < hit - 1: e.m.step_over_breakpoint()
seen = {}
for _ in range(int(sys.argv[2])):
    a, h, t = e.disasm(e.m.pc, 1)[0]
    seen[a] = seen.get(a, 0) + 1
    r = e.m
    if seen[a] <= 3:
        print('%04x %-10s %-22s af=%04x bc=%04x de=%04x hl=%04x ix=%04x iy=%04x sp=%04x' % (a, h, t, r.af, r.bc, r.de, r.hl, r.ix, r.iy, r.sp))
    e.step()
