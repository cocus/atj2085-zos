"""Headless run: boot the firmware for N T-states, dump traces and the LCD as PNG.

usage: python headless.py [fw] [--ticks N] [--png out.png] [--log out.log] [--keys SCRIPT]
  SCRIPT: comma list of tick:KEY:ms, e.g. "300000000:MENU:200" (keys: MENU LEFT RIGHT PLAY K0D; USB+ / USB- plug or unplug the cable)
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu.fwimage import FwImage
from emu.soc import ATJ2085, PORT_NAMES
from emu import pngout
from emu.disk import UserDisk
from emu.dsp import DspHLE
from emu.audio import AudioSink

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FW = os.path.join(HERE, '..', 'my-ximpro.fw')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fw', nargs='?', default=DEFAULT_FW)
    ap.add_argument('--ticks', type=int, default=50_000_000)
    ap.add_argument('--png', default=os.path.join(HERE, 'lcd_headless.png'))
    ap.add_argument('--log', default=os.path.join(HERE, 'headless.log'))
    ap.add_argument('--keys', default='')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--disk', default=None, help='user disk: host folder (FAT16 built in RAM) or raw FAT image')
    ap.add_argument('--disk-mb', type=int, default=64, help='size of the FAT16 image built from a folder')
    ap.add_argument('--disk-save', default=None, help='write the (modified) user disk image here at exit')
    ap.add_argument('--wav', default=None, help='write the DSP model PCM output to this WAV (44.1 kHz stereo)')
    ap.add_argument('--audio', action='store_true', help='play the DSP model output live (sounddevice)')
    ap.add_argument('--no-dsp', action='store_true', help='disable the DSP HLE (ports become plain latches)')
    ap.add_argument('--dsp-verbose', action='store_true')
    ap.add_argument('--tuner', choices=('tea', 'rda', 'none'), default=None,
                    help='FM tuner model on the GPIO C I2C bus (emu/i2cbus.py)')
    ap.add_argument('--shots', default='', help='comma list of tick:file.png screenshots')
    ap.add_argument('--nand', default=None,
                    help='hardware-level NAND model backed by an s1clone dump (read-only); the real '
                         'BREC init, FW reads and UD driver run against it (replaces --disk)')
    ap.add_argument('--nand-overlay', default=None,
                    help='file holding pages the firmware programmed/erased (loaded if present, saved at exit)')
    a = ap.parse_args()

    logf = open(a.log, 'w')

    def log(s):
        logf.write(s + '\n')
        if not a.quiet:
            print(s)

    disk = UserDisk.open(a.disk, a.disk_mb, a.disk_save) if a.disk else None
    sink = AudioSink(a.wav, live=a.audio, log=log) if (a.wav or a.audio) else None
    if a.nand and disk is not None:
        ap.error('--nand and --disk are exclusive')
    emu = ATJ2085(FwImage(a.fw), log=log, disk=disk, nand=a.nand, nand_overlay=a.nand_overlay,
                  nand_cache=os.path.join(HERE, 'work', 'nandcache') if a.nand else None, tuner=a.tuner)
    if not a.no_dsp:
        emu.dsp = DspHLE(emu, sink, pacing='live' if a.audio else 'emu', log=log, verbose=a.dsp_verbose)
    shots = [(int(t), f) for t, f in (x.split(':', 1) for x in a.shots.split(',') if x)]
    script = []
    for item in filter(None, a.keys.split(',')):
        t, k, ms = item.split(':')
        script.append((int(t), k, int(ms)))
    t0 = time.time()
    step = 1_000_000
    while emu.ticks < a.ticks:
        for (t, k, ms) in script:
            if emu.ticks <= t < emu.ticks + step:
                if k.upper() in ('USB+', 'USB-'):
                    emu.set_usb(k.upper() == 'USB+')
                else:
                    emu.press(k, ms)
        r = emu.run(step)
        for (t, f) in shots:
            if emu.ticks - step < t <= emu.ticks:
                pngout.save_lcd(emu.lcd, f)
                log('shot %s at %d' % (f, emu.ticks))
        if r:
            log('stopped: ' + r)
            break
    dt = time.time() - t0
    log('ran %d ticks in %.1fs; pc=%04x' % (emu.ticks, dt, emu.m.pc))
    log('--- port statistics (dir port name count)')
    for (d, p), n in sorted(emu.port_stats.items(), key=lambda x: (x[0][1], x[0][0])):
        log('  %s %02x %-14s %d' % (d, p, PORT_NAMES.get(p, '?'), n))
    log('--- syscall statistics (vector, HL or A) count')
    for (v, k), n in sorted(emu.syscall_stats.items()):
        log('  rst %02x  %04x  %d' % (v, k, n))
    log('--- last syscalls: tick vec A HL DE BC caller')
    for t, v, A, HL, DE, BC, c in list(emu.syscalls)[-int(os.environ.get('NSYS', '60')):]:
        log('  %9d rst%02x A=%02x HL=%04x DE=%04x BC=%04x from %04x' % (t, v, A, HL, DE, BC, c))
    log('--- lcd: cmds %s pixels=%d datctl=%s win=%s' % (
        {('%02x' % k): v for k, v in emu.lcd.cmd_hist.items()}, emu.lcd.pixels_written,
        emu.lcd.datctl, (emu.lcd.x0, emu.lcd.x1, emu.lcd.y0, emu.lcd.y1)))
    pngout.save_lcd(emu.lcd, a.png)
    if emu.dsp is not None:
        log('--- DSP: %s' % emu.dsp.stats)
        for t, ev in emu.dsp.history[-60:]:
            log('  %9d %s' % (t, ev))
    if sink is not None:
        sink.close()
    if disk is not None and a.disk_save:
        disk.save(a.disk_save)
    if emu.nand is not None:
        log('--- NAND: %s' % emu.nand.stats())
        if a.nand_overlay:
            emu.nand.save_overlay(a.nand_overlay)
            log('NAND overlay saved to %s' % a.nand_overlay)
    log('wrote ' + a.png)
    logf.close()


if __name__ == '__main__':
    main()
