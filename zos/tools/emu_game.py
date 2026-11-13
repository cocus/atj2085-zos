"""Scriptable zos session in atj2085emu (real NAND dump), with the optional DAC FIFO model - for the games.

usage: emu_game.py OUTDIR SCRIPT.py [--scenario NAME] [--no-dac] [--overlay OVL] [--cpu-mhz 24.576]

SCRIPT.py defines scenario(h); h offers
    h.run(tstates)            run the emulator (keys/shots are exact to ~2000 T-states)
    h.sec(s)                  run s emulated seconds (soc.CPU_HZ T-states each: 24.576 M, the real core clock)
    h.press(key, ms)          MENU LEFT RIGHT PLAY K0D(=VOL); pressed now, released after ms
    h.shot(name)              LCD screenshot OUTDIR/name.png
    h.emu / h.dac / h.log(s)  the emulator, the DAC model (None with --no-dac), the run log
    h.peek(addr, n)           RAM bytes
    h.sym / h.sym_local / h.dsym   addresses of module functions / static functions / static variables (build map + .sym)
    h.hook(addr, fn)          call fn() whenever the CPU reaches addr (breakpoint, passes through)
The first PLAY at 360 M T-states opens the launcher (like emu_nand_zos.py); h starts at 380 M (SHELL up).
At the end OUTDIR gets dac.wav (8 kHz stereo, the DAC model's output), dac_pushes.npy and run.log.
Overlay: built from zos/out/atj2085 (SHELL + the game modules + SKIN + ZOS.CFG) with lisong_rebuild/tools/nandinject.py
unless --overlay is given (so rebuild before a run).
h.sym_local / h.dsym need the .sym files of the module objects: the build only writes them for files it really compiled
(its cache does not keep them), e.g. after editing the sources.
"""
import os, sys, struct, argparse, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
EMU = os.path.join(ROOT, 'atj2085emu')
sys.path.insert(0, EMU)
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu import pngout
from emu.dac import DacModel

OUT = os.path.join(ROOT, 'zos', 'out', 'atj2085')
PY = sys.executable                      # (run this script with a python that has numpy; nandinject.py needs none)


def make_overlay(path, mods):
    files = [os.path.join(OUT, m + '.ZM') for m in mods] + [os.path.join(OUT, 'SKIN'), os.path.join(OUT, 'ZOS.CFG')]
    subprocess.check_call([PY, os.path.join(ROOT, 'lisong_rebuild', 'tools', 'nandinject.py'),
                           os.path.join(ROOT, 'readback-my-ximpro.s1clone'), path] + files)


class Session:
    def __init__(self, outdir, overlay, dac, cpu_mhz, mods):
        self.outdir = outdir
        os.makedirs(outdir, exist_ok=True)
        self.logf = open(os.path.join(outdir, 'run.log'), 'w')
        if overlay is None:
            overlay = os.path.join(outdir, 'game.ovl')
            make_overlay(overlay, mods)
        self.emu = ATJ2085(FwImage(os.path.join(ROOT, 'my-ximpro.fw')), log=lambda s: self.logf.write(s + '\n'),
                           nand=os.path.join(ROOT, 'readback-my-ximpro.s1clone'), nand_overlay=overlay,
                           nand_cache=os.path.join(EMU, 'work', 'nandcache'))
        ATJ2085.CPU_HZ = int(cpu_mhz * 1_000_000)
        self.emu.rtc_fast_period = ATJ2085.CPU_HZ // 100
        self.dac = None
        if dac:
            self.dac = DacModel(self.emu, cpu_hz=ATJ2085.CPU_HZ)
            self.emu.dac = self.dac
        self._boot()

    def _boot(self):
        emu = self.emu
        ap = open(os.path.join(OUT, 'ZOS.AP'), 'rb').read()
        ln, addr = struct.unpack_from('<HH', ap, 12)
        text = ap[0x800:0x800 + ln]
        self.run(300_000_000)
        m, mem = emu.m, emu.mem
        mem[addr:addr + len(text)] = text
        sp = (m.sp - 4) & 0xFFFF
        mem[sp:sp + 4] = bytes([0, 0, 0, 0])
        m.sp, m.pc = sp, 0x0800
        self.log('injected ZOS.AP at %d' % emu.ticks)
        self.run(60_000_000)
        self.press('PLAY', 150)                           # first PLAY = MODULES (launcher)
        self.run(20_000_000)

    def log(self, s):
        self.logf.write(s + '\n')

    def run(self, n, step=1_000_000):
        end = self.emu.ticks + n
        while self.emu.ticks < end:
            r = self.emu.run(min(step, end - self.emu.ticks))
            if r:
                self.log('stopped: %s' % r)
                raise RuntimeError(r)

    def sec(self, s):
        self.run(int(s * self.emu.CPU_HZ))

    def press(self, key, ms=150):
        self.emu.press(key, ms)

    def shot(self, name):
        pngout.save_lcd(self.emu.lcd, os.path.join(self.outdir, name + '.png'))

    def peek(self, addr, n=1):
        return bytes(self.emu.mem[addr:addr + n])

    def sym(self, mod, name):
        """address of a function of a page module from its linker map (build output)"""
        import re
        txt = open(os.path.join(OUT, 'mod_' + mod.lower(), mod.lower() + '.map')).read()
        m = re.search(r'([0-9A-F]{8})[ ]+_' + re.escape(name) + r'(?![A-Za-z0-9_])', txt)
        return int(m.group(1), 16)

    def sym_local(self, mod, name, src=None):
        """address of a static function of the module's own source file (<mod>/<mod>.c): its object's .sym offset
        + the linked address of that object's first global (_beep for QUADFALL: see snd_analyze.make_resolver)"""
        import re
        d = os.path.join(OUT, 'mod_' + mod.lower())
        mp = open(os.path.join(d, mod.lower() + '.map')).read()
        base = int(re.search(r'([0-9A-F]{8})[ ]+_beep', mp).group(1), 16)
        if src:                                      # another object of the module: its code starts at its first symbol
            return self._obj_sym(d, mod, src, name, mp)
        for ln in open(os.path.join(d, '%s_%s_c.sym' % (mod.lower(), src or mod.lower()))):
            m = re.match(r'[ ]+0 +_' + re.escape(name) + r' +([0-9A-F]{8})', ln)
            if m:
                return base + int(m.group(1), 16)
        raise KeyError(name)

    def dsym(self, mod, name):
        """RAM address of a static variable of <mod>/<mod>.c (page module data lives after the code in the page:
        the object's _DATA offset from its .sym + s__DATA from the map; the module's own file is linked first)"""
        import re
        d = os.path.join(OUT, 'mod_' + mod.lower())
        mp = open(os.path.join(d, mod.lower() + '.map')).read()
        base = int(re.search(r'([0-9A-F]{8})[ ]+s__DATA', mp).group(1), 16)
        for ln in open(os.path.join(d, '%s_%s_c.sym' % (mod.lower(), mod.lower()))):
            m = re.match(r'[ ]+1 +_' + re.escape(name) + r' +([0-9A-F]{8})', ln)
            if m:
                return base + int(m.group(1), 16)
        raise KeyError(name)

    def _obj_sym(self, d, mod, src, name, mp):
        import re
        # skinlib.c's object is linked after quadfall.c's: its _CODE starts at the global with the lowest address from it
        # (first global defined in skinlib: _skin_load) - the offset of that symbol in the .sym gives the base
        syms = {}
        for ln in open(os.path.join(d, '%s_%s_c.sym' % (mod.lower(), src))):
            m = re.match(r'[ ]+0 +(_\w+) +([0-9A-F]{8})( GR)?', ln)
            if m:
                syms[m.group(1)] = (int(m.group(2), 16), bool(m.group(3)))
        for nm, (off, glob) in syms.items():
            if glob:
                m = re.search(r'([0-9A-F]{8})[ ]+' + re.escape(nm) + r'(?![A-Za-z0-9_])', mp)
                if m:
                    return int(m.group(1), 16) - off + syms['_' + name][0]
        raise KeyError(name)

    def hook(self, addr, fn):
        self.emu.add_hle(addr, lambda: (fn(), False)[1], name='hook', passthru=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('script')
    ap.add_argument('--no-dac', action='store_true')
    ap.add_argument('--overlay', default=None)
    ap.add_argument('--cpu-mhz', type=float, default=24.576)
    ap.add_argument('--mods', default='SHELL,QUADFALL,HOPBOT,MINES')
    ap.add_argument('--scenario', default='scenario', help='name of the function in SCRIPT to run')
    a = ap.parse_args()
    s = Session(a.out, a.overlay, not a.no_dac, a.cpu_mhz, a.mods.split(','))
    ns = {'__file__': os.path.abspath(a.script)}
    exec(compile(open(a.script).read(), a.script, 'exec'), ns)
    try:
        ns[a.scenario](s)
    finally:
        if s.dac is not None:
            s.dac.flush()
            s.dac.to_wav(os.path.join(a.out, 'dac.wav'))
            import numpy as np
            np.savez(os.path.join(a.out, 'dac_out.npz'), t=np.array(s.dac.t), l=np.array(s.dac.l), r=np.array(s.dac.r), u=np.array(s.dac.u))
            np.save(os.path.join(a.out, 'dac_pushes.npy'), np.array(s.dac.pushes, dtype=np.int64).reshape(-1, 2))
            s.log('dac: %d periods, %d words pushed, %d overruns, max level %d' % (
                len(s.dac.t), len(s.dac.pushes), s.dac.overruns, s.dac.max_level))
        pngout.save_lcd(s.emu.lcd, os.path.join(a.out, 'end.png'))
        s.logf.close()


if __name__ == '__main__':
    main()
