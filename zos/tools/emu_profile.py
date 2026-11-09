"""PC-sampling profile of zos in atj2085emu on the NAND dump (same setup as emu_nand_zos.py).

usage: emu_profile.py KEYS FROM_T TO_T [--overlay OVL]
Samples the PC every ~2000 T-states between FROM_T and TO_T and prints the hottest functions,
resolved with out/atj2085/zos.map (core) and the module maps (0x4000+, by the module's name).
"""
import os, re, sys, bisect, argparse, collections, glob, struct
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'atj2085emu'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu.dsp import DspHLE


def load_map(path):
    syms = []
    for ln in open(path, errors='replace'):
        for a, n in re.findall(r'\b([0-9A-F]{8})\s+([A-Za-z_$.][\w$.]*)', ln):
            if not n.startswith(('s__', 'l__')):
                syms.append((int(a, 16), n))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('keys'); ap.add_argument('t0', type=int); ap.add_argument('t1', type=int)
    ap.add_argument('--overlay'); ap.add_argument('--module', default='files')
    ap.add_argument('--dump', default=os.path.join(ROOT, 'readback-my-ximpro.s1clone'))
    a = ap.parse_args()
    out = os.path.join(HERE, '..', 'out', 'atj2085')
    core = load_map(os.path.join(out, 'zos.map'))
    mod = load_map(glob.glob(os.path.join(out, 'mod_' + a.module, '*.map'))[0])
    emu = ATJ2085(FwImage(os.path.join(ROOT, 'my-ximpro.fw')), log=lambda s: None, nand=a.dump,
                  nand_overlay=a.overlay, nand_cache=os.path.join(ROOT, 'atj2085emu', 'work', 'nandcache'))
    emu.dsp = DspHLE(emu, None, pacing='emu', log=lambda s: None)
    ap_img = open(os.path.join(out, 'ZOS.AP'), 'rb').read()
    ln, addr = struct.unpack_from('<HH', ap_img, 12)
    keys = [(int(t), k, int(ms)) for t, k, ms in (x.split(':') for x in a.keys.split(',') if x)]
    hist = collections.Counter()
    injected = False
    while emu.ticks < a.t1:
        if not injected and emu.ticks >= 300_000_000:
            emu.mem[addr:addr + ln] = ap_img[0x800:0x800 + ln]
            sp = (emu.m.sp - 4) & 0xFFFF
            emu.mem[sp:sp + 4] = bytes(4)
            emu.m.sp, emu.m.pc = sp, 0x0800
            injected = True
        for t, k, ms in keys:
            if emu.ticks <= t < emu.ticks + 1_000_000:
                emu.press(k, ms)
        if emu.ticks < a.t0:
            emu.run(1_000_000)
            continue
        emu.run(2000)
        pc = emu.m.pc
        tab = mod if 0x4000 <= pc < 0x8000 else core
        i = bisect.bisect_right([s[0] for s in tab], pc) - 1
        hist[('mod:' if tab is mod else '') + (tab[i][1] if i >= 0 else '?%04x' % pc)] += 1
    tot = sum(hist.values())
    for n, c in hist.most_common(25):
        print('%5.1f%%  %s' % (100.0 * c / tot, n))


if __name__ == '__main__':
    main()
