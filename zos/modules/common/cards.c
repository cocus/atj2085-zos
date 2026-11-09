/* playing card drawing (see cards.h): every card strip is one LCD window filled with colour runs
 * (game.c run streamer), row by row */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../apps/theme.h"
#include "game.h"
#include "cards.h"

/* 3x5 ranks (bit 2 = left), "10" 5x5 (bit 4 = left) */
static const uint8_t rank_g[13][5] = {
    { 2, 5, 7, 5, 5 },  { 6, 1, 2, 4, 7 },  { 6, 1, 2, 1, 6 },  { 5, 5, 7, 1, 1 },  { 7, 4, 6, 1, 6 },
    { 3, 4, 7, 5, 7 },  { 7, 1, 2, 2, 2 },  { 7, 5, 7, 5, 7 },  { 7, 5, 7, 1, 6 },
    { 0x17, 0x15, 0x15, 0x15, 0x17 },                                    /* 10 */
    { 1, 1, 1, 5, 2 },  { 2, 5, 5, 6, 3 },  { 5, 5, 6, 5, 5 } };         /* J Q K */
/* 5x5 suits: spade, heart, club, diamond */
static const uint8_t suit_g[4][5] = {
    { 0x04, 0x0E, 0x1F, 0x04, 0x0E },
    { 0x0A, 0x1F, 0x1F, 0x0E, 0x04 },
    { 0x0E, 0x0E, 0x1F, 0x1B, 0x04 },
    { 0x04, 0x0E, 0x1F, 0x0E, 0x04 } };

#define C_EDGE  RGB(110, 110, 120)
#define C_FACE  RGB(250, 250, 245)
#define C_HELD  RGB(255, 236, 140)
#define C_REDS  RGB(210, 20, 30)
#define C_BLKS  RGB(16, 16, 24)
#define C_BACK  RGB(40, 72, 170)
#define C_BACK2 RGB(110, 150, 240)
#define C_SLOT  RGB(40, 140, 80)
#define C_CUR   RGB(255, 210, 0)

static void edge_row(uint8_t w, uint16_t e)          /* top / bottom row, rounded corners */
{
    gm_run(CARD_TABLE, 1);
    gm_run(e, w - 2);
    gm_run(CARD_TABLE, 1);
}

static void bits2(uint8_t b, uint16_t fg, uint16_t bg)   /* 5 bits, each 2 px */
{
    uint8_t m;
    for (m = 0x10; m; m >>= 1) gm_run(b & m ? fg : bg, 2);
}

/* wide cards (Klondike, 16 px): 1 px edge left and right, the cursor colours the edge.
 * narrow cards (Spider, 11 px): no side edges (the table shows between the columns), the cursor
 * colours the top two rows and the bottom row. */
void card_draw(uint8_t x, uint8_t y, uint8_t w, uint8_t ch, uint8_t vh, uint8_t card, uint8_t fl)
{
    uint8_t r, wide = w >= 14, cur = fl & CD_CUR, up = card & CARD_UP, last = vh == ch;
    uint8_t iw = wide ? w - 2 : w, rk = CARD_RANK(card), rw, sx, g;
    uint16_t e = cur ? C_CUR : C_EDGE, face, col;
    const uint8_t *rg, *sg;
    if (!vh) return;
    face = !up ? C_BACK : fl & CD_SEL ? C_HELD : C_FACE;
    col = CARD_RED(card) ? C_REDS : C_BLKS;
    rg = rank_g[(rk ? rk : 1) - 1];
    sg = suit_g[CARD_SUIT(card)];
    rw = rk == 10 ? 5 : 3;
    sx = iw - 5 - wide;                              /* suit column in the interior */
    gm_win(x, y, w, vh);
    for (r = 0; r < vh; r++) {
        if (r == 0 || (last && r == vh - 1)) { edge_row(w, e); continue; }
        if (wide) gm_run(e, 1);
        if (cur && r == 1) gm_run(C_CUR, iw);
        else if (!up) {                              /* back: blue, light inner frame */
            if (r == 2 || r == ch - 3) { gm_run(C_BACK, 1); gm_run(C_BACK2, iw - 2); gm_run(C_BACK, 1); }
            else if (r > 2 && r < ch - 3) {
                gm_run(C_BACK, 1); gm_run(C_BACK2, 1); gm_run(C_BACK, iw - 4); gm_run(C_BACK2, 1); gm_run(C_BACK, 1);
            } else gm_run(C_BACK, iw);
        } else if (r >= 2 && r < 7) {                /* rank and small suit */
            g = r - 2;
            gm_run(face, 1);
            gm_bits(rg[g], rw, col, face);
            gm_run(face, sx - 1 - rw);
            gm_bits(sg[g], 5, col, face);
            gm_run(face, iw - sx - 5);
        } else if (last && wide && ch >= 20 && r >= 9 && r < 19) {   /* big suit */
            gm_run(face, (iw - 10) / 2);
            bits2(sg[(r - 9) >> 1], col, face);
            gm_run(face, iw - 10 - (iw - 10) / 2);
        } else if (last && !wide && r >= ch - 7 && r < ch - 2) {
            gm_run(face, (iw - 5) / 2);
            gm_bits(sg[r - (ch - 7)], 5, col, face);
            gm_run(face, iw - 5 - (iw - 5) / 2);
        } else gm_run(face, iw);
        if (wide) gm_run(e, 1);
    }
    gm_flush();
}

void card_slot(uint8_t x, uint8_t y, uint8_t w, uint8_t ch, uint8_t suit, uint8_t fl)
{
    uint8_t r, sy = (ch - 5) / 2;
    uint16_t e = fl & CD_CUR ? C_CUR : C_SLOT;
    gm_win(x, y, w, ch);
    for (r = 0; r < ch; r++) {
        if (r == 0 || r == ch - 1) { edge_row(w, e); continue; }
        gm_run(e, 1);
        if (suit < 4 && r >= sy && r < sy + 5) {
            gm_run(CARD_TABLE, (w - 7) / 2);
            gm_bits(suit_g[suit][r - sy], 5, C_SLOT, CARD_TABLE);
            gm_run(CARD_TABLE, w - 7 - (w - 7) / 2);
        } else gm_run(CARD_TABLE, w - 2);
        gm_run(e, 1);
    }
    gm_flush();
}

void card_layout(uint8_t nd, uint8_t nu, uint8_t avail, uint8_t ch, uint8_t *dd, uint8_t *du)
{
    uint8_t d = 3, u = 7, room;
    if (nu) nu--;                                   /* the last card shows whole */
    else if (nd) nd--;
    if (avail < ch) avail = ch;
    room = avail - ch;
    if (nd * d + nu * u > room) d = 2;
    if (nd * d + nu * u > room && nu) {
        u = (room > nd * d ? room - nd * d : 0) / nu;
        if (u < 3) u = 3;
    }
    *dd = d;
    *du = u;
}
