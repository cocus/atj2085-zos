/* MINES: clear a field without opening a mine; numbers count the mines around a cell.
 * 9x9 / 10, 13x11 / 22, 16x11 / 35. The first cell opened never has a mine (nor its neighbours).
 * Keys (5): << >> move the cursor along the rows, wrapping into the next / previous row (held:
 * repeats); held VOL switches to moving along the columns (and back). PLAY (released) opens a cell
 * - on an opened number whose flags are all set, it opens the cells around it; VOL or PLAY held
 * 0.5 s sets / clears a flag. MENU: pause (continue / new game / quit). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/beep.h"
#include "../common/game.h"
#include "strings.h"

#define MAXW 16
#define MAXH 11
#define MAXN (MAXW * MAXH)
#define F_MINE 0x80
#define F_OPEN 0x40
#define F_FLAG 0x20
#define F_CNT  0x0F
#define STAT_Y 109

static const uint8_t sz_w[3] = { 9, 13, 16 }, sz_h[3] = { 9, 11, 11 }, sz_m[3] = { 10, 22, 35 };
static const uint8_t sz_c[3] = { 10, 8, 8 }, sz_y[3] = { 18, 19, 19 };
static uint8_t W, H, N, NM, CS, X0, Y0, size;
static uint8_t cell[MAXN], q[MAXN];
static uint8_t cur, axis, placed, over, flags, boom = 0xFF;
static uint16_t opened, secs, last, acc;

#define C_HI   RGB(175, 185, 215)
#define C_MID  RGB(115, 125, 165)
#define C_LO   RGB(55, 60, 90)
#define C_OPEN RGB(26, 30, 46)
#define C_GRID RGB(60, 66, 92)
#define C_CURS RGB(255, 210, 0)
#define C_BOOM RGB(220, 30, 30)
#define C_FLAG RGB(240, 40, 40)
#define C_POLE RGB(20, 20, 30)
#define C_MINE RGB(235, 235, 240)

static const uint16_t num_col[9] = { 0, RGB(110, 170, 255), RGB(90, 220, 110), RGB(255, 100, 90),
    RGB(210, 120, 255), RGB(255, 170, 60), RGB(80, 220, 220), RGB(240, 240, 240), RGB(160, 160, 170) };
static const uint8_t digit_g[9][5] = { { 0 }, { 2, 6, 2, 2, 7 }, { 6, 1, 2, 4, 7 }, { 6, 1, 2, 1, 6 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 6, 1, 6 }, { 3, 4, 7, 5, 7 }, { 7, 1, 2, 2, 2 }, { 7, 5, 7, 5, 7 } };
static const uint8_t mine_g[5] = { 0x15, 0x0E, 0x1F, 0x0E, 0x15 };
static const uint8_t flag_g[5] = { 0x0C, 0x0E, 0x0C, 0x00, 0x00 };   /* red pennant */
static const uint8_t pole_g[5] = { 0x10, 0x10, 0x10, 0x10, 0x1C };

static void tick(void)
{
    uint16_t n = hal_ticks();
    acc += n - last;
    last = n;
    while (acc >= HAL_TICK_HZ) { acc -= HAL_TICK_HZ; if (secs < 999) secs++; }
}

/* ---- drawing -------------------------------------------------------------------------------------- */
static void draw(uint8_t i)
{
    uint8_t v = cell[i], r, c, gr, gw, open = v & F_OPEN, iscur = i == cur && !over, b, m;
    uint8_t ox, oy = (CS - 5) / 2;
    uint16_t bg, fg = 0;
    const uint8_t *g = 0;
    bg = open ? (i == boom ? C_BOOM : C_OPEN) : C_MID;
    gw = 5;
    if (open && (v & F_MINE)) { g = mine_g; fg = i == boom ? C_POLE : C_MINE; }
    else if (open && (v & F_CNT)) { g = digit_g[v & F_CNT]; fg = num_col[v & F_CNT]; gw = 3; }
    else if (!open && (v & F_FLAG)) g = flag_g;
    ox = (CS - gw) / 2;
    gm_win(X0 + (i % W) * CS, Y0 + (i / W) * CS, CS, CS);
    for (r = 0; r < CS; r++) {
        if (iscur && (r == 0 || r == CS - 1)) { gm_run(C_CURS, CS); continue; }
        if (!open && !iscur && r == 0) { gm_run(C_HI, CS - 1); gm_run(C_LO, 1); continue; }
        if (!open && !iscur && r == CS - 1) { gm_run(C_LO, CS); continue; }
        if (open && !iscur && r == CS - 1) { gm_run(C_GRID, CS); continue; }
        gm_run(iscur ? C_CURS : open ? bg : C_HI, 1);
        gr = r - oy;
        if (g && gr < 5) {
            for (c = 1; c < CS - 1; c++) {
                b = c - ox;
                m = b < gw ? (uint8_t)(1 << (gw - 1 - b)) : 0;
                if (g == flag_g) gm_run(flag_g[gr] & m ? C_FLAG : pole_g[gr] & m ? C_POLE : bg, 1);
                else gm_run(g[gr] & m ? fg : bg, 1);
            }
        } else gm_run(bg, CS - 2);
        gm_run(iscur ? C_CURS : open ? C_GRID : C_LO, 1);
    }
    gm_flush();
}

static void status(void)
{
    char b[8];
    int16_t left = (int16_t)NM - flags;
    gfx_fill(0, STAT_Y, LCD_W, 8, sk.bg);
    b[0] = '*'; b[1] = 0;
    if (left < 0) { strcat(b, "-"); left = -left; }
    strcat(b, gm_num((uint16_t)left));
    gfx_text(2, STAT_Y, b, 1, C_SEL, sk.bg);
    gm_text_c(STAT_Y, T(axis ? S_COL : S_ROW), sk.dim, sk.bg);
    strcpy(b, gm_num(secs));
    gfx_text(LCD_W - 2 - (uint8_t)strlen(b) * FONT_W, STAT_Y, b, 1, sk.fg, sk.bg);
}

static void board(void)
{
    uint8_t i;
    gfx_fill(0, SKIN_TITLE_H, LCD_W, STAT_Y - SKIN_TITLE_H, sk.bg);
    for (i = 0; i < N; i++) draw(i);
    status();
    skin_footer(T(S_FOOT));
}

/* ---- rules ---------------------------------------------------------------------------------------- */
static uint8_t nb[8], nnb;
static void neighbours(uint8_t i)
{
    uint8_t x = i % W, y = i / W;
    nnb = 0;
    if (y) { if (x) nb[nnb++] = i - W - 1; nb[nnb++] = i - W; if (x + 1 < W) nb[nnb++] = i - W + 1; }
    if (x) nb[nnb++] = i - 1;
    if (x + 1 < W) nb[nnb++] = i + 1;
    if (y + 1 < H) { if (x) nb[nnb++] = i + W - 1; nb[nnb++] = i + W; if (x + 1 < W) nb[nnb++] = i + W + 1; }
}

static void place(uint8_t safe)
{
    uint8_t m = 0, i, k;
    neighbours(safe);
    while (m < NM) {
        i = gm_randn(N);
        if (i == safe || (cell[i] & F_MINE)) continue;
        for (k = 0; k < nnb && nb[k] != i; k++) ;
        if (k < nnb) continue;
        cell[i] |= F_MINE;
        m++;
    }
    for (i = 0; i < N; i++) {
        neighbours(i);
        for (k = m = 0; k < nnb; k++) if (cell[nb[k]] & F_MINE) m++;
        cell[i] |= m;
    }
    placed = 1;
    secs = acc = 0;
    last = hal_ticks();
}

/* open cell i (flood fill over empty cells); 0 = it was a mine */
static uint8_t open(uint8_t i)
{
    uint8_t qn = 0, j, k;
    if (cell[i] & (F_OPEN | F_FLAG)) return 1;
    if (cell[i] & F_MINE) { boom = i; return 0; }
    q[qn++] = i;
    cell[i] |= F_OPEN;
    while (qn) {
        j = q[--qn];
        opened++;
        draw(j);
        if (cell[j] & F_CNT) continue;
        neighbours(j);
        for (k = 0; k < nnb; k++)
            if (!(cell[nb[k]] & (F_OPEN | F_FLAG))) { cell[nb[k]] |= F_OPEN; q[qn++] = nb[k]; }
    }
    return 1;
}

static uint8_t act_open(void)          /* 0 = boom */
{
    uint8_t k, f = 0, ok = 1;
    if (!placed) place(cur);
    if (!(cell[cur] & F_OPEN)) return open(cur);
    if (!(cell[cur] & F_CNT)) return 1;
    neighbours(cur);                    /* chord: flags around == number: open the rest */
    for (k = 0; k < nnb; k++) if (cell[nb[k]] & F_FLAG) f++;
    if (f != (cell[cur] & F_CNT)) return 1;
    for (k = 0; k < nnb; k++) {
        neighbours(cur);
        if (!open(nb[k])) ok = 0;
    }
    return ok;
}

static void act_flag(void)
{
    if (cell[cur] & F_OPEN) return;
    cell[cur] ^= F_FLAG;
    flags += cell[cur] & F_FLAG ? 1 : -1;
    draw(cur);
    status();
}

static void move(uint8_t right)
{
    uint8_t old = cur, x = cur % W, y = cur / W;
    if (!axis) cur = right ? (cur + 1 == N ? 0 : cur + 1) : (cur ? cur - 1 : N - 1);
    else if (right) { if (++y == H) { y = 0; if (++x == W) x = 0; } cur = y * W + x; }
    else { if (y-- == 0) { y = H - 1; x = x ? x - 1 : W - 1; } cur = y * W + x; }
    draw(old);
    draw(cur);
}

/* ---- screens -------------------------------------------------------------------------------------- */
static char l1[22];
static uint8_t options(void)
{
    uint8_t k, y;
    gscore_t *g = gm_scores();
    for (;;) {
        gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, sk.bg);
        gm_text_c(22, T(S_SIZE0 + size), C_SEL, sk.bg);
        for (y = 0; y < 5; y++) gfx_text(4, 40 + y * 10, T(S_H_MOVE + y), 1, sk.fg, sk.bg);
        if (g->mine_best[size]) {
            strcpy(l1, T(S_BEST)); strcat(l1, " "); strcat(l1, gm_num(g->mine_best[size])); strcat(l1, "S");
            gm_text_c(98, l1, sk.dim, sk.bg);
        }
        skin_footer(T(S_START));
        k = gm_key();
        if (k == KEY_RIGHT) size = size == 2 ? 0 : size + 1;
        else if (k == KEY_LEFT) size = size ? size - 1 : 2;
        else if (k == KEY_PLAY) return 1;
        else if (k == KEY_MENU || k == GM_POWER) return 0;
    }
}

static void new_game(void)
{
    W = sz_w[size]; H = sz_h[size]; NM = sz_m[size]; CS = sz_c[size];
    N = W * H;
    X0 = (LCD_W - W * CS) / 2; Y0 = sz_y[size];
    memset(cell, 0, sizeof cell);
    cur = (H / 2) * W + W / 2;
    placed = over = flags = 0;
    boom = 0xFF;
    opened = secs = acc = 0;
}

static void end_game(uint8_t won)
{
    uint8_t i, best = 0;
    gscore_t *g;
    over = 1;
    for (i = 0; i < N; i++) {
        if (!(cell[i] & F_MINE)) continue;
        if (won) cell[i] |= F_FLAG; else if (!(cell[i] & F_FLAG)) cell[i] |= F_OPEN;
    }
    if (won) flags = NM;
    for (i = 0; i < N; i++) if (cell[i] & F_MINE) draw(i);
    draw(cur);
    status();
    if (won) {
        g = gm_scores();
        if (!g->mine_best[size] || secs < g->mine_best[size]) { g->mine_best[size] = secs; best = 1; }
        gm_scores_save();
    }
    strcpy(l1, T(S_TIME)); strcat(l1, " "); strcat(l1, gm_num(secs)); strcat(l1, "S");
    if (best) { strcat(l1, " "); strcat(l1, T(S_BEST)); }
    gm_box(3, T(won ? S_WIN : S_BOOM), won ? l1 : 0, T(S_AGAIN), 0, won ? C_OK : C_BOOM);
}

/* -> 0 quit, 1 options again */
static uint8_t play(void)
{
    event_t e;
    uint8_t k, ppend = 0, vpend = 0, ok;
    uint16_t shown = 0xFFFF;
    board();
    for (;;) {
        if (placed && !over) {
            tick();
            if (secs != shown) { shown = secs; status(); }
        }
        if (!ev_wait(&e, HAL_TICK_HZ / 4)) continue;
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return 0; }
        gm_seed(hal_ticks());
        k = e.code;
        if (over) {                                   /* the board stays visible; any key: on */
            if (e.type != EV_KEY_DOWN) continue;
            beep();
            if (k == KEY_MENU) return 0;
            new_game();
            board();
            shown = 0xFFFF;
            continue;
        }
        ok = 1;
        if (e.type == EV_KEY_DOWN) {
            if (k != KEY_LEFT && k != KEY_RIGHT) beep();
            if (k == KEY_PLAY) ppend = 1;
            else if (k == KEY_VOL) vpend = 1;
            else if (k == KEY_MENU) {
                gm_box(4, T(S_PAUSE), T(S_CONT), T(S_NEW), T(S_QUIT), C_SEL);
                k = gm_key();
                last = hal_ticks();
                if (k == GM_POWER || k == KEY_MENU) return 0;
                if (k == KEY_VOL) return 1;
                board();
                continue;
            }
        } else if (e.type == EV_KEY_REPEAT) {
            if (k == KEY_PLAY && ppend) { ppend = 0; act_flag(); }
            else if (k == KEY_VOL && vpend) { vpend = 0; axis ^= 1; status(); }
        } else if (e.type == EV_KEY_UP) {
            if (k == KEY_PLAY && ppend) { ppend = 0; ok = act_open(); }
            else if (k == KEY_VOL && vpend) { vpend = 0; act_flag(); }
        }
        if ((e.type == EV_KEY_DOWN || e.type == EV_KEY_REPEAT) && (k == KEY_LEFT || k == KEY_RIGHT))
            move(k == KEY_RIGHT);
        if (!ok) end_game(0);
        else if (placed && opened == N - NM) end_game(1);
    }
}

uint16_t mod_main(void)
{
    skin_load();
    gm_seed(hal_ticks());
    skin_title(T(S_TITLE));
    while (options()) {
        new_game();
        if (!play()) break;
    }
    return 0;
}
