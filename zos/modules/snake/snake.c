/* SNAKE on zos events: << >> turn, PLAY pause, MENU back */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include <string.h>
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "strings.h"

#define CELL 4
#define GW 32
#define GH 28
#define TOP SKIN_TITLE_H
#define MAXLEN 255
#define S_BG   C_BG
#define S_BODY RGB(80, 255, 80)
#define S_HEAD RGB(255, 255, 0)
#define S_FOOD RGB(255, 40, 40)

static uint8_t sx[MAXLEN + 1], sy[MAXLEN + 1], head, len, dir, fx, fy;
static uint16_t score, rnd;
static const int8_t dx[4] = { 1, 0, -1, 0 };
static const int8_t dy[4] = { 0, 1, 0, -1 };

static uint8_t rand8(void)
{
    rnd ^= rnd << 7; rnd ^= rnd >> 9; rnd ^= rnd << 8;
    return (uint8_t)rnd;
}

static void cell(uint8_t x, uint8_t y, uint16_t c) { gfx_fill(x * CELL, TOP + y * CELL, CELL, CELL, c); }

static uint8_t on_snake(uint8_t x, uint8_t y)
{
    uint8_t i, p = head;
    for (i = 0; i < len; i++, p--)
        if (sx[p] == x && sy[p] == y) return 1;
    return 0;
}

static void food(void)
{
    do { fx = rand8() % GW; fy = rand8() % GH; } while (on_snake(fx, fy));
    cell(fx, fy, S_FOOD);
}

/* title + score right-aligned before the battery icon (skin_text: transparent over TITLE.ZI) */
static void hud(void)
{
    char b[6];
    uint8_t i = 5;
    uint16_t v = score;
    b[5] = 0;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    skin_title(T(S_TITLE));
    skin_text(106 - (5 - i) * FONT_W, 3, b + i, C_FG);
}

static uint8_t centre(uint8_t y, uint8_t s)     /* -> text width */
{
    const char *t = T(s);
    uint8_t w = (uint8_t)strlen(t) * FONT_W;
    gfx_text((LCD_W - w) / 2, y, t, 1, C_FG, S_BG);
    return w;
}

static uint8_t wait_play_or_menu(void)
{
    event_t e;
    for (;;) {
        ev_wait(&e, 0);
        rnd += hal_ticks();
        if (e.type == EV_KEY_DOWN && (e.code == KEY_PLAY || e.code == KEY_MENU)) return e.code;
    }
}

void app_snake(void)
{
    event_t e;
    uint8_t i, nx, ny, t;
    uint16_t next;
    rnd = 0xACE1 ^ hal_ticks();
    skin_load();
    gfx_fill(0, 0, LCD_W, TOP, C_BG);
again:
    gfx_fill(0, TOP, LCD_W, LCD_H - TOP, S_BG);
    len = 4; head = 3; dir = 0; score = 0;
    for (i = 0; i < len; i++) { sx[i] = 10 + i; sy[i] = 14; cell(sx[i], sy[i], S_BODY); }
    cell(sx[head], sy[head], S_HEAD);
    hud();
    food();
    next = hal_ticks();
    for (;;) {
        next += 16 - (score > 18 ? 9 : score / 2);          /* 160 .. 70 ms per move */
        while ((int16_t)(next - hal_ticks()) > 0) {
            if (!ev_wait(&e, next - hal_ticks())) break;
            if (e.type != EV_KEY_DOWN) continue;
            if (e.code == KEY_MENU) return;
            if (e.code == KEY_RIGHT) dir = (dir + 1) & 3;
            if (e.code == KEY_LEFT) dir = (dir + 3) & 3;
            if (e.code == KEY_PLAY) {
                i = centre(60, S_PAUSE);
                if (wait_play_or_menu() == KEY_MENU) return;
                gfx_fill((LCD_W - i) / 2, 60, i, 8, S_BG);
                cell(fx, fy, S_FOOD);
                next = hal_ticks();
            }
        }
        nx = sx[head] + dx[dir];
        ny = sy[head] + dy[dir];
        if (nx >= GW || ny >= GH || on_snake(nx, ny)) {
            centre(52, S_GAMEOVER);
            centre(64, S_AGAIN);
            if (wait_play_or_menu() == KEY_PLAY) goto again;
            return;
        }
        cell(sx[head], sy[head], S_BODY);
        head++;
        sx[head] = nx; sy[head] = ny;
        cell(nx, ny, S_HEAD);
        if (nx == fx && ny == fy) {
            if (len < MAXLEN) len++;
            score++;
            hud();
            food();
        } else {
            t = head - len;
            cell(sx[t], sy[t], S_BG);
        }
    }
}
