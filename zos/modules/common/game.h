/* small helpers shared by the game modules (HOPBOT, KLONDIKE, SPIDER, MINES, QUADFALL): include game.c from one
 * module source (next to skin.c / beep.c), this header from the others.
 *   run streamer  gm_win() opens an LCD window, gm_run() streams runs of one colour into it (runs of the
 *                 same colour are merged into one hal_lcd_fill), gm_bits() a row of a 1-bit glyph;
 *                 gm_flush() ends it. Sprites, cards and cells are drawn whole (background included),
 *                 one window each: no flicker, no per-pixel windows, no pixel buffer.
 *   gm_box        centred message panel (pause / game over / win), gm_key() waits for a key
 *   scores        best scores / times in RAM (gscore_t): survive leaving and restarting a game, not a
 *                 power-off; a module that uses the ZRAM1 scratch (music players, USB) may overwrite
 *                 them (checksum: they then start from zero again). No disk writes. */
#ifndef ZOS_GAME_H
#define ZOS_GAME_H
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"

void gm_win(uint8_t x, uint8_t y, uint8_t w, uint8_t h);  /* flushes, then sets the window */
void gm_run(uint16_t c, uint8_t n);
void gm_bits(uint8_t bits, uint8_t nb, uint16_t fg, uint16_t bg);   /* bit nb-1 = leftmost */
void gm_flush(void);

void    gm_seed(uint16_t s);
uint8_t gm_rand(void);
uint8_t gm_randn(uint8_t n);                      /* 0 .. n-1 */

/* panel of n lines (n <= 4) centred on the screen, first line in hi */
void gm_box(uint8_t n, const char *l0, const char *l1, const char *l2, const char *l3, uint16_t hi);
/* waits for a key press (also mixes the time into the random numbers). EV_POWER: MOD_POWER_REQ set,
 * returns GM_POWER - the game then returns from mod_main (SHELL powers off) */
#define GM_POWER 0xFF
uint8_t gm_key(void);
char   *gm_num(uint16_t v);                       /* decimal, static buffer */
void    gm_text_c(uint8_t y, const char *s, uint16_t fg, uint16_t bg);   /* centred line */

typedef struct {
    uint8_t  magic, sum;
    uint16_t hop_best;                            /* HOPBOT best score */
    uint16_t mine_best[3];                        /* MINES best time (s) per size, 0 = none */
    uint16_t klon_best[2], klon_wins;             /* KLONDIKE best time (s) draw 1 / 3, games won */
    uint16_t spid_best[3], spid_wins;             /* SPIDER best time (s) 1 / 2 / 4 suits, games won */
    uint16_t quad_best;                           /* QUADFALL best score */
    uint8_t  quad_trk, quad_nofx;                 /* QUADFALL music track (0..2, 3 = off) / effects off */
} gscore_t;
#define GSCORE_ADDR (HAL_MODRAM_END - 32)         /* ZRAM1 scratch, its last 32 bytes */
typedef char gscore_fits[sizeof(gscore_t) <= 32 ? 1 : -1];
gscore_t *gm_scores(void);                        /* valid block (zeroed if it was not) */
void      gm_scores_save(void);                   /* after changing it */
#endif
