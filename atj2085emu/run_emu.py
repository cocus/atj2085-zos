"""ATJ2085 ("Ximpro" / LISONG LSD_501_04) emulator - GUI entry point.

usage: python run_emu.py [firmware.fw] [--vm vm.bin] [--run] [--disk DIR|IMG] [--audio] [--wav out.wav]
  firmware: AFI image (default ..\\my-ximpro.fw)
  --vm:     file used to persist the emulated VM (settings) NAND sectors
  --run:    start running immediately
  --disk:   user disk (host folder -> FAT16 image built in RAM, or a raw FAT image file)
  --audio:  play the HLE DSP output on the PC speakers (sounddevice)
  --wav:    also write the DSP output to a WAV file
  --nand:   s1clone NAND dump -> hardware-level NAND model (real BREC init + UD driver), read-only
            dump with an in-memory overlay (--nand-overlay FILE to keep it across runs)
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fw', nargs='?', default=os.path.join(HERE, '..', 'my-ximpro.fw'))
    ap.add_argument('--vm', default=None)
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--disk', default=None)
    ap.add_argument('--disk-mb', type=int, default=64)
    ap.add_argument('--audio', action='store_true')
    ap.add_argument('--wav', default=None)
    ap.add_argument('--nand', default=None, help='s1clone NAND dump: hardware-level NAND (no storage HLE)')
    ap.add_argument('--nand-overlay', default=None, help='programmed/erased pages file (load + save at exit)')
    a = ap.parse_args()
    if a.nand and a.disk:
        ap.error('--nand and --disk are exclusive')
    from emu.gui import App
    from emu.disk import UserDisk
    from emu.audio import AudioSink
    disk_f = (lambda: UserDisk.open(a.disk, a.disk_mb)) if a.disk else None
    sink_f = (lambda: AudioSink(a.wav, live=a.audio)) if (a.audio or a.wav) else None
    app = App(a.fw, vm_path=a.vm, disk_factory=disk_f, sink_factory=sink_f, live_audio=a.audio,
              nand=a.nand, nand_overlay=a.nand_overlay)
    if a.run:
        app.toggle()
    app.mainloop()
    if a.nand and a.nand_overlay and app.emu.nand is not None:
        app.emu.nand.save_overlay(a.nand_overlay)


if __name__ == '__main__':
    main()
