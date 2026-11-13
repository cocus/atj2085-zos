"""QUADFALL rules check in the emulator: emu_game.py OUTDIR quadfall_logic_test.py

The game runs for real (keys, timers); between emulator steps the test writes the board / the falling
piece into the module's RAM (the game is idle in its polling loop then), presses keys and compares the result
with an independent Python implementation of the rules (collision, rotation with the wall kicks, line
clears, scoring, level) and checks invariants while a pseudo-random player hammers the keys.
Screenshots of the interesting moments go to OUTDIR.
"""
import random

W, H = 10, 20
# piece masks, 4 rotations, row 0 in the high nibble (the standard tetromino shapes)
SHAPES = [
    [0x0F00, 0x2222, 0x00F0, 0x4444], [0x6600] * 4, [0x4E00, 0x4640, 0x0E40, 0x4C40], [0x6C00, 0x4620, 0x06C0, 0x8C40],
    [0xC600, 0x2640, 0x0C60, 0x4C80], [0x8E00, 0x6440, 0x0E20, 0x44C0], [0x2E00, 0x4460, 0x0E80, 0xC440]]
PTS = [0, 100, 300, 500, 800]


def cells(t, r, x, y):
    m = SHAPES[t][r]
    return [(x + (i % 4), y + (i // 4)) for i in range(16) if m & (0x8000 >> i)]


def collide(bd, t, r, x, y):
    for cx, cy in cells(t, r, x, y):
        if cx < 0 or cx >= W or cy >= H:
            return True
        if cy >= 0 and bd[cy][cx]:
            return True
    return False


def rotate(bd, t, r, x, y):
    kicks = [0] if t == 1 else [0, -1, 1, -2, 2] if t == 0 else [0, -1, 1]
    r2 = (r + 1) & 3
    for k in kicks:
        if not collide(bd, t, r2, x + k, y):
            return r2, x + k
    return r, x


def clear_lines(bd, level):
    keep = [row for row in bd if not all(row)]
    n = H - len(keep)
    return [[0] * W for _ in range(n)] + keep, n, PTS[n] * (level + 1)


class State:
    def __init__(self, h):
        self.h = h
        g = lambda n: h.dsym('QUADFALL', n)
        self.a = {n: g(n) for n in ('bd', 'pt', 'pr', 'px', 'py', 'nxt', 'score', 'lines', 'level', 'dead', 'fall_at')}

    def rd(self, n, sz=1):
        m = self.h.emu.mem
        a = self.a[n]
        v = m[a] | (m[a + 1] << 8) if sz == 2 else m[a]
        return v - 256 if sz == 1 and n in ('px', 'py') and v > 127 else v

    def wr(self, n, v, sz=1):
        m = self.h.emu.mem
        a = self.a[n]
        m[a] = v & 255
        if sz == 2:
            m[a + 1] = v >> 8

    def board(self):
        m = self.h.emu.mem
        b = self.a['bd']
        return [[m[b + r * W + c] for c in range(W)] for r in range(H)]

    def set_board(self, bd):
        m = self.h.emu.mem
        b = self.a['bd']
        for r in range(H):
            for c in range(W):
                m[b + r * W + c] = bd[r][c]

    def piece(self):
        return self.rd('pt'), self.rd('pr'), self.rd('px'), self.rd('py')

    def set_piece(self, t, r, x, y):
        self.wr('pt', t); self.wr('pr', r); self.wr('px', x); self.wr('py', y)
        self.wr('fall_at', (self.h.emu.ticks // (self.h.emu.CPU_HZ // 100) + 50000) & 0xFFFF, 2)   # no gravity for a while


def scenario(h):
    log = h.log
    ok = [0, 0]

    def check(cond, msg):
        ok[0 if cond else 1] += 1
        log(('PASS ' if cond else 'FAIL ') + msg)
        print(('PASS ' if cond else 'FAIL ') + msg)

    h.press('PLAY', 150); h.sec(3)            # QUADFALL
    h.press('PLAY', 150); h.sec(1.0)          # start
    S = State(h)
    empty = [[0] * W for _ in range(H)]

    # --- A: walls and collisions ------------------------------------------------------------------------
    S.set_board(empty)
    S.set_piece(1, 0, 3, 5)                    # O: mask columns 1-2
    for _ in range(8):
        h.press('LEFT', 60); h.sec(0.15)
    check(S.piece()[2] == -1, 'O stops at the left wall: px = %d (expected -1)' % S.piece()[2])
    for _ in range(14):
        h.press('RIGHT', 60); h.sec(0.15)
    check(S.piece()[2] == 7, 'O stops at the right wall: px = %d (expected 7)' % S.piece()[2])
    bd = [[0] * W for _ in range(H)]
    bd[6][5] = bd[7][5] = 3                    # an obstacle in the way of a piece to the left of it
    S.set_board(bd)
    S.set_piece(1, 0, 2, 5)                    # O cells x 3,4 rows 5,6
    h.press('RIGHT', 60); h.sec(0.3)
    exp = collide(bd, 1, 0, 3, 5)
    check(S.piece()[2] == (2 if exp else 3), 'piece blocked by a block on its right: px = %d (expected %d)' % (S.piece()[2], 2 if exp else 3))

    # --- B: rotation, wall kicks ----------------------------------------------------------------------------
    S.set_board(empty)
    for t in range(7):
        for r in range(4):
            for x in range(-2, 9):
                S.set_board(empty)
                if collide(empty, t, r, x, 4):
                    continue
                S.set_piece(t, r, x, 4)
                h.press('PLAY', 50); h.sec(0.12)
                exp = rotate(empty, t, r, x, 4)
                got = (S.piece()[1], S.piece()[2])
                if got != exp:
                    check(False, 'rotate piece %d rot %d x %d: got %s expected %s' % (t, r, x, got, exp))
                    break
            else:
                continue
            break
    else:
        check(True, 'rotation + wall kicks agree with the reference for all 7 pieces x 4 rotations x all columns (empty well)')
    # rotation blocked by the stack: piece next to a column of blocks
    bd = [[0] * W for _ in range(H)]
    for rr in range(2, 12):
        bd[rr][4] = 2
    S.set_board(bd)
    bad = 0
    for t in range(7):
        for x in range(-1, 8):
            for r in range(4):
                if collide(bd, t, r, x, 5):
                    continue
                S.set_piece(t, r, x, 5)
                h.press('PLAY', 50); h.sec(0.12)
                exp = rotate(bd, t, r, x, 5)
                if (S.piece()[1], S.piece()[2]) != exp:
                    bad += 1
    check(bad == 0, 'rotation against a column of blocks: %d mismatches' % bad)
    pt, pr, px, py = S.piece()
    check(not collide(bd, pt, pr, px, py), 'the piece never ends up inside a block')

    # --- C: line clears, scoring, level up -----------------------------------------------------------------------
    for nlines, label in ((4, 'tetris-style 4 rows'), (1, 'single row'), (2, 'double'), (3, 'triple')):
        bd = [[0] * W for _ in range(H)]
        for rr in range(H - nlines, H):
            for c in range(W - 1):
                bd[rr][c] = 1 + (c % 7)
        bd[10][0] = 5                           # something above that must fall
        S.set_board(bd)
        S.wr('score', 0, 2); S.wr('lines', 0, 2); S.wr('level', 0)
        S.set_piece(0, 1, 7, 0)                 # I vertical: its column is px + 2 = 9, rows py .. py + 3
        sc0 = S.rd('score', 2)
        h.press('K0D', 900); h.sec(1.5)         # soft drop, then the hold -> hard drop
        h.shot('lineclear_%d' % nlines) if nlines == 4 else None
        h.sec(2.0)
        got = S.board()
        # the I piece drops 4 cells in a column: it fills rows (H-4)..(H-1) of column 9
        full = [r[:] for r in bd]
        for rr in range(H - 4, H):
            full[rr][9] = 1
        exp_bd, n, pts = clear_lines(full, 0)
        # the next piece may already be on its way (spawned after the lock): compare the locked cells only
        diff = [(r, c) for r in range(H) for c in range(W) if got[r][c] != exp_bd[r][c]]
        # the new piece is not in the board array; only the rows the clear touched must match
        check(not diff, '%s: board after the lock + clear matches the reference (%d differing cells)' % (label, len(diff)))
        lines = S.rd('lines', 2)
        sc = S.rd('score', 2)
        # score = line points + 2 per hard-dropped cell + 1 per soft-dropped cell
        extra = sc - pts
        check(lines == n and 0 <= extra <= 2 * 16 + 20, '%s: lines %d (expected %d), score %d = %d line points + %d drop points' % (label, lines, n, sc, pts, extra))
    # level up: 9 lines -> one more row = level 1, the speed table / the tempo follow
    bd = [[0] * W for _ in range(H)]
    for c in range(W - 1):
        bd[H - 1][c] = 2
    S.set_board(bd)
    S.wr('lines', 9, 2); S.wr('level', 0); S.wr('score', 0, 2)
    S.set_piece(0, 1, 7, 0)
    h.press('K0D', 900); h.sec(1.8)
    h.shot('levelup_banner')
    h.sec(0.3)
    check(S.rd('level') == 1 and S.rd('lines', 2) == 10, 'level up after 10 lines: level %d lines %d' % (S.rd('level'), S.rd('lines', 2)))
    h.sec(3.0)

    # --- D: pseudo-random player: invariants ------------------------------------------------------------------------
    rnd = random.Random(1)
    S.set_board(empty)
    S.wr('lines', 0, 2); S.wr('level', 0); S.wr('score', 0, 2); S.wr('dead', 0)
    keys = ['LEFT', 'RIGHT', 'PLAY', 'PLAY', 'K0D']
    bad = 0
    nchk = 0
    last_score = 0
    for i in range(400):
        k = rnd.choice(keys)
        h.press(k, rnd.choice([40, 60, 80, 120] if k != 'K0D' else [40, 60, 600]))
        h.sec(rnd.choice([0.1, 0.2, 0.4]))
        if i % 3 == 0:
            h.sec(0.25)                         # settle: the game is idle (not mid-handler)
            bd = S.board()
            pt, pr, px, py = S.piece()
            cnt = sum(1 for row in bd for v in row if v)
            nchk += 1
            if S.rd('dead'):
                break
            locked = pt <= 6 and pr <= 3 and all(0 <= cy < H and 0 <= cx < W and bd[cy][cx] == pt + 1 for cx, cy in cells(pt, pr, px, py))
            spawn_fail = py == 0 and px == 3 and pr == 0          # game over: the new piece did not fit
            if any(v > 7 for row in bd for v in row) or pt > 6 or pr > 3 or                     (collide(bd, pt, pr, px, py) and not locked and not spawn_fail):
                bad += 1
                log('invariant violated at step %d: piece %s' % (i, (pt, pr, px, py)))
                h.shot('bad_%d' % i)
    sc = S.rd('score', 2)
    ln = S.rd('lines', 2)
    bd = S.board()
    cnt = sum(1 for row in bd for v in row if v)
    check(bad == 0, 'random player: %d samples, falling piece never overlaps the board, valid cell values' % nchk)
    check((cnt + 10 * ln) % 4 == 0, 'blocks on the board (%d) + 10 x cleared rows (%d) is a multiple of 4: whole pieces only' % (cnt, ln))
    h.shot('random_end')
    log('SUMMARY pass=%d fail=%d' % tuple(ok))
    print('SUMMARY pass=%d fail=%d' % tuple(ok))
