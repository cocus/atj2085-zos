/* SNAKE - first homebrew AP for the LISONG/Ximpro ATJ2085 player.
 * Replaces GAME.AP ("JUEGOS" in the main menu).
 * LEFT / RIGHT (<< >>): turn   PLAY: start / pause / again   MENU: back to the main menu
 * Timing from the OS timer (os_now ticks), not busy loops.
 */
#include <stdint.h>
#include "actos.h"
#include "gfx.h"

#define CELL   4
#define GW     32
#define GH     30
#define TOP    8           /* status bar height */
#define MAXLEN 255

#define C_BG    RGB(0, 40, 0)
#define C_SNAKE RGB(80, 255, 80)
#define C_HEAD  RGB(255, 255, 0)
#define C_FOOD  RGB(255, 40, 40)
#define C_BAR   RGB(0, 0, 90)
#define C_TEXT  RGB(255, 255, 255)

static uint8_t sx[MAXLEN + 1], sy[MAXLEN + 1];   /* ring buffer of body cells */
static uint8_t head, len, dir, fx, fy;
static uint16_t score, rnd;

static const int8_t dx[4] = {1, 0, -1, 0};
static const int8_t dy[4] = {0, 1, 0, -1};

static uint8_t random8(void)
{
    rnd ^= rnd << 7; rnd ^= rnd >> 9; rnd ^= rnd << 8;     /* xorshift16 */
    return (uint8_t)rnd;
}

static void cell(uint8_t x, uint8_t y, uint16_t c)
{
    gfx_fill(x * CELL, TOP + y * CELL, CELL, CELL, c);
}

static uint8_t on_snake(uint8_t x, uint8_t y)
{
    uint8_t i, p = head;
    for (i = 0; i < len; i++, p--)
        if (sx[p] == x && sy[p] == y) return 1;
    return 0;
}

static void place_food(void)
{
    do { fx = random8() % GW; fy = random8() % GH; } while (on_snake(fx, fy));
    cell(fx, fy, C_FOOD);
}

static void draw_score(void)
{
    gfx_fill(0, 0, SCR_W, TOP, C_BAR);
    gfx_text(2, 0, "SNAKE", 1, C_TEXT, C_BAR);
    gfx_num(80, 0, score, 1, C_TEXT, C_BAR);
}

/* wait `ticks` timer ticks while polling keys; returns the last key pressed or 0 */
static uint8_t frame_wait(uint8_t ticks)
{
    uint8_t k = 0, m;
    uint16_t t0 = os_now();
    while ((uint16_t)(os_now() - t0) < ticks) {
        wdt_kick();
        m = os_get_msg();
        if (m == KEY_MENU || m == KEY_PLAY || m == KEY_LEFT || m == KEY_RIGHT) k = m;
        rnd++;
    }
    return k;
}

/* waits for PLAY or MENU */
static uint8_t wait_key(void)
{
    uint8_t m;
    for (;;) {
        wdt_kick();
        m = os_get_msg();
        rnd++;
        if (m == KEY_PLAY || m == KEY_MENU) return m;
    }
}

static void new_game(void)
{
    uint8_t i;
    gfx_fill(0, TOP, SCR_W, SCR_H - TOP, C_BG);
    len = 4; head = 3; dir = 0; score = 0;
    for (i = 0; i < len; i++) { sx[i] = 10 + i; sy[i] = 15; cell(sx[i], sy[i], C_SNAKE); }
    cell(sx[head], sy[head], C_HEAD);
    draw_score();
    place_food();
}

/* returns 1 when the player wants to leave */
static uint8_t play(void)
{
    uint8_t k, nx, ny, tail;
    new_game();
    for (;;) {
        k = frame_wait(16 - (score > 18 ? 9 : score / 2));      /* 160 ms .. 70 ms per move */
        if (k == KEY_MENU) return 1;
        if (k == KEY_RIGHT) dir = (dir + 1) & 3;       /* turn right */
        if (k == KEY_LEFT)  dir = (dir + 3) & 3;       /* turn left  */
        if (k == KEY_PLAY) {                           /* pause */
            gfx_text(40, 60, "PAUSE", 1, C_TEXT, C_BG);
            if (wait_key() == KEY_MENU) return 1;
            gfx_fill(40, 60, 30, 8, C_BG);
            cell(fx, fy, C_FOOD);
        }
        nx = sx[head] + dx[dir];
        ny = sy[head] + dy[dir];
        if (nx >= GW || ny >= GH || on_snake(nx, ny)) {   /* wall or self: game over */
            gfx_text(22, 50, "GAME OVER", 1, C_TEXT, C_BG);
            gfx_text(10, 64, "PLAY: AGAIN", 1, C_TEXT, C_BG);
            return wait_key() == KEY_MENU;
        }
        cell(sx[head], sy[head], C_SNAKE);
        head++;
        sx[head] = nx; sy[head] = ny;
        cell(nx, ny, C_HEAD);
        if (nx == fx && ny == fy) {
            if (len < MAXLEN) len++;
            score++;
            draw_score();
            place_food();
        } else {
            tail = head - len;
            cell(sx[tail], sy[tail], C_BG);
        }
    }
}

uint16_t main(uint16_t param)
{
    uint8_t timer;
    (void)param;
    rnd = 0xACE1;
    timer = os_timer_start(1);
    for (;;) {
        gfx_fill(0, 0, SCR_W, SCR_H, C_BG);
        gfx_text(19, 30, "SNAKE", 3, C_HEAD, C_BG);
        gfx_text(16, 70, "PLAY: START", 1, C_TEXT, C_BG);
        gfx_text(16, 82, "<< >>: TURN", 1, C_TEXT, C_BG);
        gfx_text(16, 94, "MENU: EXIT", 1, C_TEXT, C_BG);
        if (wait_key() == KEY_MENU) break;
        if (play()) break;
    }
    os_timer_stop(timer);       /* the OS must not call our handler after we are gone */
    return RESULT_NULL;    /* back to the main menu */
}
