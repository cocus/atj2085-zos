import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quadfall_logic_test import State, W, H


def stack():
    # a hand-made, plausible stack (piece indices 1..7 = I O T S Z J L colours)
    rows = {
        19: "1112221177", 18: "1122221377", 17: "4222335337", 16: "4.22335333", 15: "44.2263.3.", 14: ".4..2663..",
        13: "....66.6..", 12: ".......5..", 11: ".......55.",
    }
    bd = [[0] * W for _ in range(H)]
    for r, s in rows.items():
        for c, ch in enumerate(s):
            bd[r][c] = 0 if ch == '.' else int(ch)
    return bd


def scenario(h):
    h.press('PLAY', 150); h.sec(3)
    h.shot('01_title')
    h.press('PLAY', 150); h.sec(1.0)
    S = State(h)
    S.set_board(stack())
    S.wr('score', 1240, 2); S.wr('lines', 12, 2); S.wr('level', 1)
    S.set_piece(5, 0, 3, 3)
    h.press('MENU', 100); h.sec(0.6)
    h.shot('04_pause_menu')
    h.press('RIGHT', 100); h.sec(0.3)
    h.shot('04b_pause_menu_music_row')
    h.press('PLAY', 100); h.sec(0.3)       # music: next track
    h.shot('04c_pause_menu_track_b')
    h.press('MENU', 100); h.sec(1.0)        # resume: full redraw from the board
    S.set_piece(5, 0, 3, 3)
    h.sec(0.5)
    h.shot('02_midgame')
    # line clear effect: make rows 17, 18 nearly full, drop an I vertical into column 9... use a ready board
    bd = stack()
    for r in (17, 18, 19):
        bd[r] = [1 + (c % 7) for c in range(W)]
        bd[r][8] = 0
    S.set_board(bd)
    h.press('MENU', 100); h.sec(0.5); h.press('MENU', 100); h.sec(0.8)   # redraw the new board
    S.set_piece(0, 1, 6, 0)                 # I vertical at column 8, rows 0-3
    h.press('K0D', 1200)
    h.sec(0.40)
    for i in range(14):
        h.run(int(0.015 * h.emu.CPU_HZ))
        h.shot('03_trail_%02d' % i)
    h.sec(0.1)
    for i in range(8):
        h.run(int(0.045 * h.emu.CPU_HZ))
        h.shot('03_clear_%02d' % i)
    h.sec(2.0)
    # game over: stack up to the top
    bd = stack()
    for r in range(2, 20):
        bd[r] = [(r + c) % 7 + 1 if (c + r) % 5 else 0 for c in range(W)]
    S.set_board(bd)
    S.set_piece(2, 0, 3, 0)
    S.wr('fall_at', 0, 2)                    # gravity due now: the piece cannot move, locks, the next spawn collides
    h.sec(0.5)
    for i in range(6):
        h.sec(0.2)
        h.shot('05_over_fill_%d' % i)
    h.sec(2.5)
    h.shot('05_gameover')
def count(h, pred, x0=0, x1=60, y0=16, y1=120):
    w, hh, rgb = h.emu.lcd.to_rgb(False)
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * w + x) * 3
            if pred(rgb[i], rgb[i + 1], rgb[i + 2]):
                n += 1
    return n


def scenario_effects(h):
    h.press('PLAY', 150); h.sec(3)
    h.press('PLAY', 150); h.sec(1.0)
    S = State(h)
    bd = stack()
    for r in range(11, 20):
        bd[r][9] = 0
    for r in (16, 17, 18, 19):
        bd[r] = [1 + ((c + r) % 7) for c in range(W)]
        bd[r][9] = 0
    S.set_board(bd)
    h.press('MENU', 100); h.sec(0.5); h.press('MENU', 100); h.sec(1.0)   # redraw the new board
    S.set_piece(0, 1, 7, 0)                 # I vertical at column 9
    h.press('K0D', 1500)
    got = {}
    for i in range(400):
        h.run(int(0.004 * h.emu.CPU_HZ))
        tr = count(h, lambda r, g, b: b > 170 and 90 < r < 140 and 80 < g < 120)
        fl = count(h, lambda r, g, b: r > 225 and g > 230 and b > 245)
        if tr > 8 and 'trail' not in got:
            got['trail'] = i; h.shot('03_trail_hit')
        if fl > 30 and 'flash' not in got:
            got['flash'] = i; h.shot('03_clear_flash')
        if fl > 30 and 'flash' in got and 'flash2' not in got and i > got['flash'] + 35:
            got['flash2'] = i; h.shot('03_clear_flash2')
    h.log('events: %s' % got)
    h.sec(2)
    h.shot('03_after')
