/* SPIDER solitaire, 1, 2 or 4 suits (104 cards, 10 columns, 5 deals of 10). Cards descending in one
 * suit form runs; a full run K..A leaves the table; 8 runs win. Controls as KLONDIKE: one cursor over
 * the 10 columns and the DEAL spot (top right), << >> move it (wraps, repeats when held).
 *   PLAY  DEAL: one card on every column (none may be empty); a column: its longest run goes
 *         where it fits best (same suit first, then any suit, then an empty column)
 *   VOL   picks up the top card; again on the same column: one card more (then none); with cards
 *         picked up, VOL or PLAY on another column puts them there
 *   MENU  drops what is picked up, else pause (continue / new game / quit) */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/beep.h"
#include "../common/game.h"
#include "../common/cards.h"
#include "strings.h"

#define CW 11
#define CH 16
#define COLX(i) ((i) * 13)
#define STAT_Y (SKIN_TITLE_H + 1)
#define TAB_Y 28
#define NCOL 10
#define SP_DEAL NCOL
#define NSP (NCOL + 1)
#define MAXC 104
#define NONE 0xFF
#define DEAL_X 84
#define BIT(s) ((uint16_t)1 << (s))

static uint8_t tab[NCOL][MAXC], ntab[NCOL];
static uint8_t stock[50], nstock, done, smode;
static uint8_t cur, hsrc = NONE, hn;
static uint16_t dirty, moves, secs, last, acc;
#define DIRTY_STAT BIT(15)

static void tick(void)
{
    uint16_t n = hal_ticks();
    acc += n - last;
    last = n;
    while (acc >= HAL_TICK_HZ) { acc -= HAL_TICK_HZ; if (secs < 59999) secs++; }
}

/* ---- rules -------------------------------------------------------------------------------------- */
static uint8_t run_len(uint8_t col)               /* face-up cards of one suit in sequence at the top */
{
    uint8_t n = ntab[col], k, a, b;
    if (!n || !(tab[col][n - 1] & CARD_UP)) return 0;
    for (k = 1; k < n; k++) {
        a = tab[col][n - 1 - k]; b = tab[col][n - k];
        if (!(a & CARD_UP) || CARD_RANK(a) != CARD_RANK(b) + 1 || CARD_SUIT(a) != CARD_SUIT(b)) break;
    }
    return k;
}

static uint8_t top(uint8_t col) { return ntab[col] ? tab[col][ntab[col] - 1] : 0; }

static void flip(uint8_t col) { if (ntab[col]) tab[col][ntab[col] - 1] |= CARD_UP; }

static void complete(uint8_t col)                 /* K..A of one suit on top: off the table */
{
    if (run_len(col) >= 13 && CARD_RANK(top(col)) == 1) {
        ntab[col] -= 13;
        flip(col);
        done++;
        dirty |= BIT(col) | DIRTY_STAT;
    }
}

/* how well n cards from s fit on d: 0 no, 1 empty column, 2 any suit, 3 same suit */
static uint8_t fit(uint8_t s, uint8_t n, uint8_t d)
{
    uint8_t c, t;
    if (s == d || d >= NCOL || !n || n > run_len(s)) return 0;
    c = tab[s][ntab[s] - n];
    if (!ntab[d]) return 1;
    t = top(d);
    if (CARD_RANK(t) != CARD_RANK(c) + 1) return 0;
    return CARD_SUIT(t) == CARD_SUIT(c) ? 3 : 2;
}

static uint8_t do_move(uint8_t s, uint8_t n, uint8_t d)
{
    if (!fit(s, n, d)) return 0;
    memcpy(&tab[d][ntab[d]], &tab[s][ntab[s] - n], n);
    ntab[d] += n;
    ntab[s] -= n;
    flip(s);
    hsrc = NONE;
    moves++;
    dirty |= BIT(s) | BIT(d);
    complete(d);
    return 1;
}

static uint8_t smart(uint8_t s)
{
    uint8_t want, n, k, d, f;
    if (s >= NCOL) return 0;
    for (want = 3; want; want--)
        for (n = run_len(s); n; n--) {
            if (want == 1 && n != run_len(s)) break;          /* empty column: the whole run */
            if (want == 1 && n == ntab[s]) break;             /* ... and not a whole column */
            for (k = 1; k < NCOL; k++) {
                d = (s + k) % NCOL;
                f = fit(s, n, d);
                if (f == want || (want == 2 && f == 3)) return do_move(s, n, d);
            }
        }
    return 0;
}

static uint8_t deal_row(void)
{
    uint8_t c;
    if (!nstock) return 1;
    for (c = 0; c < NCOL; c++) if (!ntab[c]) return 0;
    for (c = 0; c < NCOL; c++) {
        tab[c][ntab[c]++] = stock[--nstock] | CARD_UP;
        complete(c);
    }
    hsrc = NONE;
    moves++;
    dirty |= 0x3FF | DIRTY_STAT;
    return 1;
}

static void new_game(void)
{
    static uint8_t deck[MAXC];
    uint8_t i, j, t, c, n;
    static const uint8_t suits[3][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 3 } };
    for (i = 0; i < MAXC; i++) deck[i] = CARD(i % 13 + 1, suits[smode][(i / 13) & 3]);
    for (i = MAXC - 1; i; i--) { j = gm_randn(i + 1); t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    i = 0;
    for (c = 0; c < NCOL; c++) {
        n = c < 4 ? 6 : 5;
        for (j = 0; j < n; j++) tab[c][j] = deck[i++];
        tab[c][n - 1] |= CARD_UP;
        ntab[c] = n;
    }
    for (nstock = 0; i < MAXC; ) stock[nstock++] = deck[i++];
    done = 0;
    cur = 0; hsrc = NONE; moves = secs = acc = 0;
    last = hal_ticks();
}

/* ---- drawing -------------------------------------------------------------------------------------- */
static char l1[22], l2[22];
static const char *lab(char *b, uint8_t s, uint16_t v)
{
    strcpy(b, T(s));
    strcat(b, " ");
    strcat(b, gm_num(v));
    return b;
}

static void status(void)
{
    uint8_t c = cur == SP_DEAL;
    gfx_fill(0, STAT_Y, DEAL_X, 10, CARD_TABLE);
    gfx_text(2, STAT_Y + 1, lab(l1, S_DONE, done), 1, RGB(200, 230, 200), CARD_TABLE);
    gfx_fill(DEAL_X, STAT_Y, LCD_W - DEAL_X, 10, c ? RGB(255, 210, 0) : RGB(40, 72, 170));
    lab(l1, S_DEAL, nstock / NCOL);
    gfx_text(DEAL_X + (LCD_W - DEAL_X - (uint8_t)strlen(l1) * FONT_W) / 2, STAT_Y + 1, l1, 1,
             c ? RGB(0, 0, 0) : RGB(255, 255, 255), c ? RGB(255, 210, 0) : RGB(40, 72, 170));
}

static void draw_col(uint8_t col)
{
    uint8_t fl = col == cur ? CD_CUR : 0, x = COLX(col), i, n = ntab[col], y = TAB_Y, dd, du, nd, c, f, vh;
    uint8_t *t = tab[col];
    if (!n) { card_slot(x, y, CW, CH, NONE, fl); y += CH; }
    for (nd = 0; nd < n && !(t[nd] & CARD_UP); nd++) ;
    card_layout(nd, n - nd, LCD_H - TAB_Y, CH, &dd, &du);
    for (i = 0; i < n && y < LCD_H; i++) {
        c = t[i];
        vh = i == n - 1 ? CH : (c & CARD_UP) ? du : dd;
        if (y + vh > LCD_H) vh = LCD_H - y;
        f = hsrc == col && i >= n - hn ? CD_SEL : 0;
        if (i == n - 1) f |= fl;
        card_draw(x, y, CW, CH, vh, c, f);
        y += vh;
    }
    if (y < LCD_H) gfx_fill(x, y, CW, LCD_H - y, CARD_TABLE);
}

static void redraw(void)
{
    uint8_t s;
    for (s = 0; s < NCOL; s++) if (dirty & BIT(s)) draw_col(s);
    if (dirty & (BIT(SP_DEAL) | DIRTY_STAT)) status();
    dirty = 0;
}

static void full_redraw(void)
{
    gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, CARD_TABLE);
    dirty = 0xFFFF;
    redraw();
}

/* ---- screens -------------------------------------------------------------------------------------- */
static uint8_t options(void)
{
    uint8_t k, y;
    gscore_t *g = gm_scores();
    for (;;) {
        gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, sk.bg);
        gm_text_c(22, T(S_SUIT1 + smode), C_SEL, sk.bg);
        for (y = 0; y < 6; y++) gfx_text(4, 38 + y * 10, T(S_H_MOVE + y), 1, sk.fg, sk.bg);
        lab(l1, S_WON, g->spid_wins);
        if (g->spid_best[smode]) { strcat(l1, "  "); strcat(l1, T(S_BEST)); strcat(l1, " "); strcat(l1, gm_num(g->spid_best[smode])); strcat(l1, "S"); }
        gm_text_c(102, l1, sk.dim, sk.bg);
        skin_footer(T(S_START));
        k = gm_key();
        if (k == KEY_RIGHT) smode = smode == 2 ? 0 : smode + 1;
        else if (k == KEY_LEFT) smode = smode ? smode - 1 : 2;
        else if (k == KEY_PLAY) return 1;
        else if (k == KEY_MENU || k == GM_POWER) return 0;
    }
}

static uint8_t play(void)
{
    event_t e;
    uint8_t old, k;
    gscore_t *g;
    full_redraw();
    for (;;) {
        tick();
        ev_wait(&e, 1000);
        tick();
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return 0; }
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        gm_seed(hal_ticks());
        k = e.code;
        if (k == KEY_LEFT || k == KEY_RIGHT) {
            old = cur;
            cur = k == KEY_RIGHT ? (cur + 1 == NSP ? 0 : cur + 1) : (cur ? cur - 1 : NSP - 1);
            dirty |= BIT(old) | BIT(cur);
            redraw();
            continue;
        }
        if (e.type != EV_KEY_DOWN) continue;
        beep();
        if (k == KEY_MENU) {
            if (hsrc != NONE) { dirty |= BIT(hsrc); hsrc = NONE; redraw(); continue; }
            gm_box(4, T(S_PAUSE), T(S_CONT), T(S_NEW), T(S_QUIT), C_SEL);
            k = gm_key();
            last = hal_ticks();
            if (k == GM_POWER || k == KEY_MENU) return 0;
            if (k == KEY_VOL) return 1;
            full_redraw();
            continue;
        }
        if (cur == SP_DEAL && (k == KEY_PLAY || k == KEY_VOL)) {
            if (hsrc != NONE) dirty |= BIT(hsrc);
            hsrc = NONE;
            if (!deal_row()) {                       /* an empty column: say so for a moment */
                gfx_fill(0, STAT_Y, LCD_W, 10, CARD_TABLE);
                gfx_text(2, STAT_Y + 1, T(S_FILL), 1, RGB(255, 210, 0), CARD_TABLE);
                ev_wait(&e, 150);
                dirty |= DIRTY_STAT;
            }
        } else if (hsrc != NONE && cur != hsrc && (k == KEY_PLAY || k == KEY_VOL)) {
            do_move(hsrc, hn, cur);
        } else if (k == KEY_VOL) {
            if (hsrc == NONE) { if (run_len(cur)) { hsrc = cur; hn = 1; } }
            else if (++hn > run_len(cur)) hsrc = NONE;
            dirty |= BIT(cur);
        } else if (k == KEY_PLAY) {
            if (hsrc != NONE) { hsrc = NONE; dirty |= BIT(cur); }
            else smart(cur);
        }
        redraw();
        if (done == 8) {
            g = gm_scores();
            g->spid_wins++;
            k = !g->spid_best[smode] || secs < g->spid_best[smode];
            if (k) g->spid_best[smode] = secs;
            gm_scores_save();
            lab(l1, S_TIME, secs); strcat(l1, "S");
            if (k) { strcat(l1, " "); strcat(l1, T(S_BEST)); }
            lab(l2, S_MOVES, moves);
            gm_box(4, T(S_WIN), l1, l2, T(S_AGAIN), C_OK);
            k = gm_key();
            return k == KEY_PLAY || k == KEY_VOL;
        }
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
