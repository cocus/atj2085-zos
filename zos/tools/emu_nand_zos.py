"""Run zos in atj2085emu on the real NAND dump (zos' own NAND/FTL driver, no stock storage code).

usage: emu_nand_zos.py OUTDIR "t:KEY:ms,..." "t:shot.png,..." [--ticks N] [--overlay OVL] [--inject-at T]
                      [--tuner tea|rda|none] [--save-overlay OVL2]

Boots the stock firmware from the s1clone dump (+ overlay, e.g. from lisong_rebuild/tools/nandinject.py
holding the .ZM modules), and at --inject-at (default 300 M T-states, main menu) loads
zos/out/atj2085/ZOS.AP at 0x0800 and enters it like the homebrew launcher's stub does (the OS
stack gets a fake [bank 0][ret 0x0000] frame: leaving zos reboots the emulated player).
Times in the key/shot lists are absolute T-states.
"""
import os, sys, struct, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
EMU = os.path.join(ROOT, 'atj2085emu')
sys.path.insert(0, EMU)
from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu import pngout
from emu.dsp import DspHLE
from emu.audio import AudioSink


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('keys'); ap.add_argument('shots')
    ap.add_argument('--ticks', type=int, default=900_000_000)
    ap.add_argument('--overlay', default=None)
    ap.add_argument('--inject-at', type=int, default=300_000_000)
    ap.add_argument('--dump', default=os.path.join(ROOT, 'readback-my-ximpro.s1clone'))
    ap.add_argument('--wav', default=None, help='write the DSP model PCM output here')
    ap.add_argument('--tuner', choices=('tea', 'rda', 'none'), default=None,
                    help='FM tuner model on the GPIO C I2C bus (emu/i2cbus.py)')
    ap.add_argument('--save-overlay', default=None,
                    help='write the NAND overlay (zos writes, e.g. ZOS.CFG) here at the end')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, 'run.log'), 'w')
    emu = ATJ2085(FwImage(os.path.join(ROOT, 'my-ximpro.fw')), log=lambda s: logf.write(s + '\n'),
                  nand=a.dump, nand_overlay=a.overlay, nand_cache=os.path.join(EMU, 'work', 'nandcache'), tuner=a.tuner)
    sink = AudioSink(a.wav, live=False, log=lambda s: logf.write(s + '\n')) if a.wav else None
    emu.dsp = DspHLE(emu, sink, pacing='emu', log=lambda s: logf.write(s + '\n'))
    ap_img = open(os.path.join(HERE, '..', 'out', 'atj2085', 'ZOS.AP'), 'rb').read()
    ln, addr = struct.unpack_from('<HH', ap_img, 12)
    text = ap_img[0x800:0x800 + ln]
    keys = [(int(t), k, int(ms)) for t, k, ms in (x.split(':') for x in a.keys.split(',') if x)]
    shots = [(int(t), os.path.join(a.out, f)) for t, f in (x.split(':', 1) for x in a.shots.split(',') if x)]
    step, injected = 1_000_000, False
    while emu.ticks < a.ticks:
        if not injected and emu.ticks >= a.inject_at:
            m, mem = emu.m, emu.mem
            mem[addr:addr + len(text)] = text
            sp = (m.sp - 4) & 0xFFFF                         # [bank word][ret] like an AP call
            mem[sp:sp + 4] = bytes([0, 0, 0, 0])
            m.sp, m.pc = sp, 0x0800
            injected = True
            logf.write('injected ZOS.AP (%d bytes) at %d, sp=%04x\n' % (len(text), emu.ticks, sp))
        for t, k, ms in keys:
            if emu.ticks <= t < emu.ticks + step:
                emu.press(k, ms)
        r = emu.run(step)
        for t, f in shots:
            if emu.ticks - step < t <= emu.ticks:
                pngout.save_lcd(emu.lcd, f)
        if r:
            logf.write('stopped: %s\n' % r)
            break
    logf.write('end pc=%04x ticks=%d nand=%s\n' % (emu.m.pc, emu.ticks, emu.nand.stats()))
    logf.write('dsp=%s\n' % (emu.dsp.stats,))
    for t, ev in emu.dsp.history[-40:]:
        logf.write('  %9d %s\n' % (t, ev))
    if sink is not None:
        sink.close()
    if a.save_overlay:
        emu.nand.save_overlay(a.save_overlay)
    logf.close()
    pngout.save_lcd(emu.lcd, os.path.join(a.out, 'end.png'))


if __name__ == '__main__':
    main()
