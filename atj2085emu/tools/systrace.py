"""Run to a tick count with a key script and print named syscalls (skipping noisy ones).
usage: systrace.py --from T --to T [--keys ...] [extra emu args]"""
import argparse, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu.disk import UserDisk
from emu.dsp import DspHLE

names = json.load(open(os.path.join(HERE, '..', '..', 'lisong_rebuild', 'analysis', 'api_names.json')))
def nm(v, k):
    for sect, d in names.items():
        if ('rst%02x' % v) in sect.replace(' ', ''):
            x = d.get('0x%04x' % k)
            if x: return '/'.join(x)
    return ''

ap = argparse.ArgumentParser()
ap.add_argument('--from', dest='t0', type=int, default=300_000_000)
ap.add_argument('--to', type=int, default=320_000_000)
ap.add_argument('--keys', default='300000000:MENU:200')
ap.add_argument('--disk', default=None)
ap.add_argument('--skip', default='08:0011,08:0000')
a = ap.parse_args()
skip = set()
for s in a.skip.split(','):
    v, k = s.split(':'); skip.add((int(v, 16), int(k, 16)))
logs = []
emu = ATJ2085(FwImage(os.path.join(HERE, '..', '..', 'my-ximpro.fw')), log=lambda s: logs.append(s),
              **({'disk': UserDisk.open(a.disk)} if a.disk else {}))
(setattr(e, 'dsp', DspHLE(e, None)) if 'e' in dir() else setattr(emu, 'dsp', DspHLE(emu, None)))
script = [(int(t), k, int(ms)) for t, k, ms in (x.split(':') for x in a.keys.split(',') if x)]
seen = 0
step = 1_000_000
while emu.ticks < a.to:
    for (t, k, ms) in script:
        if emu.ticks <= t < emu.ticks + step:
            emu.set_usb(k == 'USB+') if k in ('USB+', 'USB-') else emu.press(k, ms)
    emu.run(step)
    if emu.ticks >= a.t0:
        sc = list(emu.syscalls)
        for e in sc[seen:] if seen <= len(sc) else sc:
            t, v, A, HL, DE, BC, c = e
            k = HL if v in (0x10, 0x18, 0x30) else A
            if (v, k) in skip: continue
            print('%9d rst%02x A=%02x HL=%04x DE=%04x BC=%04x from %04x %s' % (t, v, A, HL, DE, BC, c, nm(v, k)))
    emu.syscalls.clear(); seen = 0
    for s in logs:
        if emu.ticks >= a.t0 and 'FWREAD' not in s: print('   LOG', s)
    logs.clear()
