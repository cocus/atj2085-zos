import numpy as np, os


def scenario(h):
    trk = int(os.environ.get('QF_TRK', '0'))
    secs = float(os.environ.get('QF_SECS', '30'))
    emu = h.emu
    h.press('PLAY', 150)                  # run QUADFALL
    h.sec(3)
    pump = h.sym('QUADFALL', 'snd_pump')
    rec = []

    def hook():
        sp = emu.m.sp
        ret = emu.mem[sp] | (emu.mem[(sp + 1) & 0xFFFF] << 8)
        row = [emu.now(), emu.lcd_writes, ret, 0]
        rec.append(row)
        emu.add_hle(ret, lambda: (row.__setitem__(3, emu.now()), False)[1], oneshot=True, name='x', passthru=True)
    h.hook(pump, hook)
    h.hook(h.sym_local('QUADFALL', 'snd_music', src='skinlib'), lambda: h.log('MUSICSTART %d' % emu.now()))
    for _ in range(int(os.environ.get('QF_LVL', '0'))):
        h.press('RIGHT', 100); h.sec(0.3)      # start level on the title screen
    h.press('PLAY', 150)                  # start
    t0 = emu.now()
    h.log('game start t0=%d' % t0)
    h.sec(1.5)
    if trk:
        h.press('MENU', 100); h.sec(0.5)
        h.press('RIGHT', 100); h.sec(0.3)
        for i in range(trk):
            h.press('PLAY', 100); h.sec(0.3)
        h.shot('pause_menu')
        h.press('MENU', 100); h.sec(0.5)
        h.log('music started at about %d' % emu.now())
    # a few key presses: sfx over the music (move, rotate)
    if os.environ.get('QF_KEYS', '1') == '1':
        for k in ['LEFT', 'PLAY', 'RIGHT', 'PLAY']:
            h.press(k, 80); h.sec(0.7)
    h.shot('mid1')
    h.sec(secs)
    h.shot('mid2')
    np.save(os.path.join(h.outdir, 'pump_times.npy'), np.array(rec, dtype=np.int64))
    h.log('pumps=%d' % len(rec))
