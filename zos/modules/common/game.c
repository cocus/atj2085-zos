/* game module helpers (see game.h). Include from one module source, after skin.c. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "skin.h"
#include "beep.h"
#include "game.h"

static uint16_t gm_c, gm_n;
void gm_flush(void)
{
    if (gm_n) hal_lcd_fill(gm_c, gm_n);
    gm_n = 0;
}
void gm_win(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    gm_flush();
    hal_lcd_window(x, y, w, h);
}
void gm_run(uint16_t c, uint8_t n)
{
    if (c != gm_c) { gm_flush(); gm_c = c; }
    gm_n += n;
}
void gm_bits(uint8_t bits, uint8_t nb, uint16_t fg, uint16_t bg)
{
    uint8_t m = 1 << (nb - 1);
    for (; m; m >>= 1) gm_run(bits & m ? fg : bg, 1);
}

static uint16_t gm_rnd = 0xACE1;
void gm_seed(uint16_t s) { gm_rnd ^= s; if (!gm_rnd) gm_rnd = 0xACE1; }
uint8_t gm_rand(void)
{
    gm_rnd ^= gm_rnd << 7; gm_rnd ^= gm_rnd >> 9; gm_rnd ^= gm_rnd << 8;
    return (uint8_t)gm_rnd;
}
uint8_t gm_randn(uint8_t n)
{
    uint8_t lim = (uint8_t)(256 - 256 % n), r;     /* 256 % n: no bias */
    if (lim == 0) return gm_rand() % n;
    do r = gm_rand(); while (r >= lim);
    return r % n;
}

static char gm_nb[6];
char *gm_num(uint16_t v)
{
    uint8_t i = 5;
    gm_nb[5] = 0;
    do { gm_nb[--i] = '0' + v % 10; v /= 10; } while (v);
    return gm_nb + i;
}

void gm_text_c(uint8_t y, const char *s, uint16_t fg, uint16_t bg)
{
    gfx_text((LCD_W - (uint8_t)strlen(s) * FONT_W) / 2, y, s, 1, fg, bg);
}

#define BOX_W 124
void gm_box(uint8_t n, const char *l0, const char *l1, const char *l2, const char *l3, uint16_t hi)
{
    uint8_t h = n * 11 + 7, y = (LCD_H - h) / 2 + 6, i;
    const char *l[4];
    l[0] = l0; l[1] = l1; l[2] = l2; l[3] = l3;
    gfx_fill((LCD_W - BOX_W) / 2, y, BOX_W, h, sk.sel_bot);
    gfx_fill((LCD_W - BOX_W) / 2 + 1, y + 1, BOX_W - 2, h - 2, sk.bg);
    for (i = 0; i < n; i++)
        if (l[i]) gm_text_c(y + 5 + i * 11, l[i], i ? sk.fg : hi, sk.bg);
}

uint8_t gm_key(void)
{
    event_t e;
    for (;;) {
        skin_wait(&e, 0);
        gm_seed(hal_ticks());
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return GM_POWER; }
        if (e.type == EV_KEY_DOWN) { beep(); return e.code; }
    }
}

static __at(GSCORE_ADDR) gscore_t gsc;          /* absolute: no storage allocated */
#define GS_MAGIC 0x6A
static uint8_t gs_sum(void)
{
    uint8_t s = 0x5A, i;
    const uint8_t *p = (const uint8_t *)&gsc + 2;
    for (i = 2; i < sizeof gsc; i++) s = (uint8_t)((s << 1) | (s >> 7)) ^ *p++;
    return s;
}
gscore_t *gm_scores(void)
{
    if (gsc.magic != GS_MAGIC || gsc.sum != gs_sum()) {
        memset(&gsc, 0, sizeof gsc);
        gsc.magic = GS_MAGIC;
        gsc.sum = gs_sum();
    }
    return &gsc;
}
void gm_scores_save(void) { gsc.magic = GS_MAGIC; gsc.sum = gs_sum(); }
