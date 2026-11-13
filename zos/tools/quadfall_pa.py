import sys, os
import numpy as np


def scenario(h):
    emu = h.emu
    emu.watch = {0x9E, 0xDD, 0x80, 0x81, 0x8B}
    ev = lambda name: h.log('EVT %s %d' % (name, emu.now()))
    h.press('PLAY', 150); h.sec(3)
    ev('title')
    h.press('PLAY', 150)
    ev('start')
    h.sec(8)
    ev('pause')
    h.press('MENU', 100); h.sec(4)
    ev('resume')
    h.press('MENU', 100); h.sec(8)
    ev('quit_menu')
    h.press('MENU', 100); h.sec(0.5)
    for _ in range(3):
        h.press('RIGHT', 100); h.sec(0.3)
    ev('quit')
    h.press('PLAY', 100); h.sec(0.4)       # QUIT -> title screen, music fading
    ev('title2')
    h.press('MENU', 100); h.sec(0.1)       # leave the module at once: snd_exit with the amplifier still at volume
    ev('exit')
    h.sec(3)
    h.shot('after_exit')
    ev('end')
    h.log('watch: ' + ' '.join('%d:%02x=%02x' % (t, p, v) for t, p, v in emu.watch_log))
