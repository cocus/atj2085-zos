/* KLONDIKE solitaire (draw 1 or 3). One cursor walks the piles in reading order: stock, waste,
 * the 4 foundations (one per suit), the 7 columns; << >> move it (wraps, repeats when held).
 *   PLAY  stock: deal (empty: turn the waste over); a card: moves it (or the longest run that can
 *         go somewhere) to a foundation, else to another column
 *   VOL   picks up the top card; again on the same column: one card more (then none); with cards
 *         picked up, VOL or PLAY on another pile puts them there
 *   MENU  drops what is picked up, else pause (continue / new game / quit)
 * When the stock and waste are empty and every card is face up, the rest goes up by itself. */
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

#define CW 16
#define CH 22
#define ROW_Y 18
#define TAB_Y 42
#define COLX(i) (2 + (i) * 18)
#define WASTE_X 20
#define FAN 6
enum { SP_STOCK, SP_WASTE, SP_F0, SP_T0 = 6, NSP = 13 };
#define NONE 0xFF
#define BIT(s) ((uint16_t)1 << (s))

static uint8_t stock[24], nstock, waste[24], nwaste, fnd[4];
static uint8_t tab[7][20], ntab[7];
static uint8_t cur, hsrc = NONE, hn, draw3;
static uint16_t dirty, moves, secs, last, acc;

/* ---- time (hal_ticks wraps after 655 s: summed up while playing) ------------------------------ */
static void tick(void)
{
    uint16_t n = hal_ticks();
    acc += n - last;
    last = n;
    while (acc >= HAL_TICK_HZ) { acc -= HAL_TICK_HZ; if (secs < 59999) secs++; }
}

/* ---- rules -------------------------------------------------------------------------------------- */
static uint8_t is_tab(uint8_t s) { return s >= SP_T0; }

static uint8_t run_len(uint8_t col)                /* face-up cards in sequence at the top */
{
    uint8_t n = ntab[col], k = 0, a, b;
    if (!n || !(tab[col][n - 1] & CARD_UP)) return 0;
    for (k = 1; k < n; k++) {
        a = tab[col][n - 1 - k]; b = tab[col][n - k];
        if (!(a & CARD_UP) || CARD_RANK(a) != CARD_RANK(b) + 1 || !CARD_RED(a) == !CARD_RED(b)) break;
    }
    return k;
}

static uint8_t avail(uint8_t s)                    /* cards that can be taken from s */
{
    if (s == SP_WASTE) return nwaste ? 1 : 0;
    if (s >= SP_F0 && s < SP_T0) return fnd[s - SP_F0] ? 1 : 0;
    if (is_tab(s)) return run_len(s - SP_T0);
    return 0;
}

static uint8_t bottom_of(uint8_t s, uint8_t n)     /* the card that lands first */
{
    if (s == SP_WASTE) return waste[nwaste - 1];
    if (!is_tab(s)) return CARD(fnd[s - SP_F0], s - SP_F0) | CARD_UP;
    return tab[s - SP_T0][ntab[s - SP_T0] - n];
}

static uint8_t fits(uint8_t c, uint8_t n, uint8_t d)
{
    uint8_t t, col;
    if (d >= SP_F0 && d < SP_T0)
        return n == 1 && CARD_SUIT(c) == d - SP_F0 && fnd[d - SP_F0] + 1 == CARD_RANK(c);
    if (!is_tab(d)) return 0;
    col = d - SP_T0;
    if (!ntab[col]) return CARD_RANK(c) == 13;
    t = tab[col][ntab[col] - 1];
    return (t & CARD_UP) && CARD_RANK(t) == CARD_RANK(c) + 1 && !CARD_RED(t) != !CARD_RED(c);
}

static uint8_t do_move(uint8_t s, uint8_t n, uint8_t d)
{
    uint8_t i, c, *p, col;
    if (s == d || !n || n > avail(s) || !fits(bottom_of(s, n), n, d)) return 0;
    for (i = n; i; i--) {
        c = bottom_of(s, i);
        if (d >= SP_F0 && d < SP_T0) fnd[d - SP_F0] = CARD_RANK(c);
        else { col = d - SP_T0; tab[col][ntab[col]++] = c; }
    }
    if (s == SP_WASTE) nwaste--;
    else if (!is_tab(s)) fnd[s - SP_F0]--;
    else {
        col = s - SP_T0;
        ntab[col] -= n;
        if (ntab[col]) { p = &tab[col][ntab[col] - 1]; *p |= CARD_UP; }   /* turn the new top card */
    }
    hsrc = NONE;
    moves++;
    dirty |= BIT(s) | BIT(d);
    return 1;
}

/* PLAY on a pile: foundation first, else the longest run that fits on another column */
static uint8_t smart(uint8_t s)
{
    uint8_t n, k, d, start;
    if (s == SP_STOCK || !avail(s)) return 0;
    if (s == SP_WASTE || is_tab(s))
        for (d = SP_F0; d < SP_T0; d++) if (do_move(s, 1, d)) return 1;
    start = is_tab(s) ? s - SP_T0 : 6;
    for (n = avail(s); n; n--)
        for (k = 1; k <= 7; k++) {
            d = SP_T0 + (start + k) % 7;
            if (is_tab(s) && n == ntab[s - SP_T0] && !ntab[d - SP_T0]) continue;   /* pointless */
            if (do_move(s, n, d)) return 1;
        }
    return 0;
}

static void deal_stock(void)
{
    uint8_t k = draw3 ? 3 : 1;
    if (nstock) while (k-- && nstock) waste[nwaste++] = stock[--nstock] | CARD_UP;
    else if (nwaste) while (nwaste) stock[nstock++] = waste[--nwaste] & ~CARD_UP;
    else return;
    if (hsrc == SP_WASTE) hsrc = NONE;
    moves++;
    dirty |= BIT(SP_STOCK) | BIT(SP_WASTE);
}

static void new_game(void)
{
    uint8_t deck[52], i, j, t, c;
    for (i = 0; i < 52; i++) deck[i] = CARD(i % 13 + 1, i / 13);
    for (i = 51; i; i--) { j = gm_randn(i + 1); t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    i = 0;
    for (c = 0; c < 7; c++) {
        for (j = 0; j <= c; j++) tab[c][j] = deck[i++];
        tab[c][c] |= CARD_UP;
        ntab[c] = c + 1;
    }
    for (nstock = 0; i < 52; ) stock[nstock++] = deck[i++];
    nwaste = 0;
    memset(fnd, 0, sizeof fnd);
    cur = SP_STOCK; hsrc = NONE; moves = secs = acc = 0;
    last = hal_ticks();
}

/* ---- drawing -------------------------------------------------------------------------------------- */
static uint8_t spot_x(uint8_t s)
{
    if (s == SP_STOCK) return COLX(0);
    if (s == SP_WASTE) return WASTE_X;
    if (s < SP_T0) return COLX(s - SP_F0 + 3);
    return COLX(s - SP_T0);
}

static void draw_spot(uint8_t s)
{
    uint8_t fl = s == cur ? CD_CUR : 0, x = spot_x(s), i, n, y, dd, du, nd, c, f, vh, *t;
    if (s == SP_STOCK) {
        if (nstock) card_draw(x, ROW_Y, CW, CH, CH, stock[0], fl);
        else card_slot(x, ROW_Y, CW, CH, NONE, fl);
    } else if (s == SP_WASTE) {
        n = nwaste < (draw3 ? 3 : 1) ? nwaste : (draw3 ? 3 : 1);
        if (n < 3) gfx_fill(x + n * FAN, ROW_Y, CW + (2 - n) * FAN, CH, CARD_TABLE);   /* shorter fan */
        if (!n) card_slot(x, ROW_Y, CW, CH, NONE, fl);
        for (i = 0; i < n; i++)
            card_draw(x + i * FAN, ROW_Y, CW, CH, CH, waste[nwaste - n + i],
                      i == n - 1 ? fl | (hsrc == s ? CD_SEL : 0) : 0);
    } else if (s < SP_T0) {
        i = s - SP_F0;
        if (fnd[i]) card_draw(x, ROW_Y, CW, CH, CH, CARD(fnd[i], i) | CARD_UP, fl | (hsrc == s ? CD_SEL : 0));
        else card_slot(x, ROW_Y, CW, CH, i, fl);
    } else {
        t = tab[s - SP_T0];
        n = ntab[s - SP_T0];
        y = TAB_Y;
        if (!n) { card_slot(x, y, CW, CH, NONE, fl); y += CH; }
        for (nd = 0; nd < n && !(t[nd] & CARD_UP); nd++) ;
        card_layout(nd, n - nd, LCD_H - TAB_Y, CH, &dd, &du);
        for (i = 0; i < n; i++) {
            c = t[i];
            vh = i == n - 1 ? CH : (c & CARD_UP) ? du : dd;
            f = hsrc == s && i >= n - hn ? CD_SEL : 0;
            if (i == n - 1) f |= fl;
            if (y + vh > LCD_H) vh = LCD_H - y;        /* (cannot happen with 19 cards at most) */
            card_draw(x, y, CW, CH, vh, c, f);
            y += vh;
        }
        if (y < LCD_H) gfx_fill(x, y, CW, LCD_H - y, CARD_TABLE);
    }
}

static void redraw(void)
{
    uint8_t s;
    for (s = 0; s < NSP; s++) if (dirty & BIT(s)) draw_spot(s);
    dirty = 0;
}

static void full_redraw(void)
{
    gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, CARD_TABLE);
    dirty = 0xFFFF;
    redraw();
}

/* ---- screens -------------------------------------------------------------------------------------- */
static char l1[22], l2[22];
static const char *lab(char *b, uint8_t s, uint16_t v)
{
    strcpy(b, T(s));
    strcat(b, " ");
    strcat(b, gm_num(v));
    return b;
}

/* -> 1 start, 0 leave */
static uint8_t options(void)
{
    uint8_t k;
    gscore_t *g = gm_scores();
    for (;;) {
        gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, sk.bg);
        gm_text_c(24, T(draw3 ? S_DRAW3 : S_DRAW1), C_SEL, sk.bg);
        gfx_text(4, 42, T(S_H_MOVE), 1, sk.fg, sk.bg);
        gfx_text(4, 52, T(S_H_PLAY), 1, sk.fg, sk.bg);
        gfx_text(4, 62, T(S_H_VOL1), 1, sk.fg, sk.bg);
        gfx_text(4, 72, T(S_H_VOL2), 1, sk.fg, sk.bg);
        gfx_text(4, 82, T(S_H_MENU), 1, sk.fg, sk.bg);
        lab(l1, S_WON, g->klon_wins);
        if (g->klon_best[draw3]) { strcat(l1, "  "); strcat(l1, T(S_BEST)); strcat(l1, " "); strcat(l1, gm_num(g->klon_best[draw3])); strcat(l1, "S"); }
        gm_text_c(100, l1, sk.dim, sk.bg);
        skin_footer(T(S_START));
        k = gm_key();
        if (k == KEY_LEFT || k == KEY_RIGHT) draw3 ^= 1;
        else if (k == KEY_PLAY) return 1;
        else if (k == KEY_MENU || k == GM_POWER) return 0;
    }
}

static void autofinish(void)
{
    uint8_t c, d, any;
    event_t e;
    if (nstock || nwaste) return;
    for (c = 0; c < 7; c++) if (ntab[c] && !(tab[c][0] & CARD_UP)) return;
    do {
        any = 0;
        for (c = 0; c < 7; c++)
            for (d = SP_F0; d < SP_T0; d++)
                if (do_move(SP_T0 + c, 1, d)) { any = 1; redraw(); skin_wait(&e, 4); }
    } while (any);
}

/* -> 0 quit, 1 new game */
static uint8_t play(void)
{
    event_t e;
    uint8_t old, k, won;
    gscore_t *g;
    full_redraw();
    for (;;) {
        tick();
        skin_wait(&e, 1000);
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
        if (cur == SP_STOCK && (k == KEY_PLAY || k == KEY_VOL)) {
            if (hsrc != NONE) dirty |= BIT(hsrc);
            hsrc = NONE;
            deal_stock();
        } else if (hsrc != NONE && cur != hsrc && (k == KEY_PLAY || k == KEY_VOL)) {
            do_move(hsrc, hn, cur);                     /* does not fit: stays picked up */
        } else if (k == KEY_VOL) {
            if (hsrc == NONE) { if (avail(cur)) { hsrc = cur; hn = 1; } }
            else if (++hn > avail(cur)) hsrc = NONE;
            dirty |= BIT(cur);
        } else if (k == KEY_PLAY) {
            if (hsrc != NONE) { hsrc = NONE; dirty |= BIT(cur); }
            else smart(cur);
        }
        redraw();
        autofinish();
        for (won = 0; won < 4 && fnd[won] == 13; won++) ;
        if (won == 4) {
            g = gm_scores();
            g->klon_wins++;
            k = !g->klon_best[draw3] || secs < g->klon_best[draw3];
            if (k) g->klon_best[draw3] = secs;
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
