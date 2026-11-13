import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quadfall_logic_test import State, W, H


def scenario(h):
    emu = h.emu
    emu.watch = {0x9E, 0xDD, 0x80, 0x81}
    ev = lambda name: h.log('EVT %s %d' % (name, emu.now()))
    h.press('PLAY', 150); h.sec(3)
    ev('title')
    h.press('PLAY', 150); h.sec(1.0)
    ev('game_start')
    S = State(h)
    # music off: pause menu, MUSIC row, A -> B -> C -> OFF
    h.press('MENU', 100); h.sec(0.5)
    ev('menu_open')
    h.press('RIGHT', 100); h.sec(0.5)
    ev('menu_key')
    for _ in range(3):
        h.press('PLAY', 100); h.sec(0.4)
    h.press('MENU', 100); h.sec(1.5)
    ev('resumed_music_off')
    empty = [[0] * W for _ in range(H)]
    S.set_board(empty)
    S.set_piece(2, 0, 3, 3)
    h.sec(0.3)
    ev('move'); h.press('LEFT', 60); h.sec(2.0)
    ev('rot'); h.press('PLAY', 60); h.sec(2.0)
    S.set_piece(1, 0, 3, 18)
    ev('lock'); h.press('K0D', 60); h.sec(2.0)
    S.set_piece(2, 0, 3, 0)
    ev('hard'); h.press('K0D', 900); h.sec(2.5)
    for n in (1, 2, 3, 4):
        bd = [[0] * W for _ in range(H)]
        for r in range(H - 4, H):
            for c in range(W):
                bd[r][c] = 1 + (c + r) % 7
        # a gap in the bottom n rows at column 9, the piece I fills it
        for r in range(H - n, H):
            bd[r][9] = 0
        for r in range(H - 4, H - n):
            bd[r] = [0] * W
        S.set_board(bd)
        S.set_piece(0, 1, 7, 0)
        ev('clear%d' % n); h.press('K0D', 900); h.sec(3.0)
    # level up
    bd = [[0] * W for _ in range(H)]
    for c in range(9):
        bd[H - 1][c] = 3
    S.set_board(bd); S.wr('lines', 9, 2); S.set_piece(0, 1, 7, 0)
    ev('levelup'); h.press('K0D', 900); h.sec(5.0)
    # game over
    bd = [[0] * W for _ in range(H)]
    for r in range(2, 20):
        bd[r] = [(r + c) % 7 + 1 if (c + r) % 5 else 0 for c in range(W)]
    S.set_board(bd); S.set_piece(2, 0, 3, 0); S.wr('fall_at', 0, 2)
    ev('gameover')
    h.sec(6.0)
    ev('end')
    # leave: PLAY/AGAIN then MENU = back to title, MENU = quit the game
    h.press('MENU', 100); h.sec(1.0)
    ev('title_again')
    h.press('MENU', 100); h.sec(2.0)
    ev('left_game')
    import numpy as np
    h.log('watch: ' + ' '.join('%d:%02x=%02x' % (t, p, v) for t, p, v in emu.watch_log))
