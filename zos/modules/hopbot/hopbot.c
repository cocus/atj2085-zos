/* HOPBOT: an endless side-scrolling runner. A little robot runs over a field; crates, stacks of
 * crates and traffic cones come along the ground, drones fly at head height (and, later, higher up).
 * PLAY (or <<) jumps - held, the jump goes higher; >> ducks (in the air: drops faster). The speed
 * grows with the score. MENU / VOL pause. Best score kept in RAM (common/game.h).
 * Drawing: every sprite (robot, obstacle, cloud) is one LCD window streamed as colour runs with its
 * background (and the strip it just left) included, so nothing flickers and no frame buffer is needed. */
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

#define FRAME     4                  /* ticks per frame: 25 fps */
#define GROUND    108                /* first ground row */
#define PX        14                 /* robot left edge */
#define PW        14
#define HUD_Y     19
#define JUMP_V    100                /* 1/16 px per frame */
#define GRAV      12
#define GRAV_HOLD 5
#define HOLD_MAX  9                  /* frames the held key keeps gravity low */
#define SPD0      40                 /* 2.5 px per frame */
#define SPD_MAX   100

#define SKY    RGB(120, 176, 232)
#define GRASS  RGB(70, 165, 70)
#define DIRT   RGB(150, 102, 58)
#define PEBBLE RGB(105, 68, 38)
#define INK    RGB(20, 34, 70)

/* ---- sprites: one character per pixel ('.' = sky) ----------------------------------------------- */
static const char bot_head[12][PW + 1] = {
    "........r.....",
    "........d.....",
    "...ddddddddd..",
    "...dwwwwwwwd..",
    "...dwwwwcccd..",
    "...dwwwwcccd..",
    "...dwwwwwwwd..",
    "...ddddddddd..",
    ".....dwwwd....",
    "....dwwwwwd...",
    "...ddwyywwdd..",
    "....dwwwwwd..." };
static const char bot_legs[3][3][PW + 1] = {
    { ".....dd.dd....", "....dd...dd...", "...dd.....dd.." },     /* stride */
    { "......dd......", ".....dddd.....", ".....d..d....." },     /* passing */
    { ".....dddd.....", "....dd..dd....", ".............." } };   /* jump */
static const char bot_duck[9][PW + 1] = {
    "r.............",
    ".d............",
    "..dddddddddd..",
    "..dwwwwwwccd..",
    "..dddddddddd..",
    "...dwwyywwd...",
    "...dwwwwwwd...",
    "...dd....dd...",
    "..dd......dd.." };
static const char crate[10][11] = {
    "kkkkkkkkkk", "kknnnnnnkk", "knknnnnknk", "knnknnknnk", "knnnkknnnk",
    "knnnkknnnk", "knnknnknnk", "knknnnnknk", "kknnnnnnkk", "kkkkkkkkkk" };
static const char cones[9][18] = {                          /* up to 3 cones: the first 6n-1 columns */
    "..o.....o.....o..", "..o.....o.....o..", ".oWo...oWo...oWo.", ".ooo...ooo...ooo.", ".ooo...ooo...ooo.",
    "oWWWo.oWWWo.oWWWo", "ooooo.ooooo.ooooo", "ooooo.ooooo.ooooo", "kkkkk.kkkkk.kkkkk" };
static const char drone[8][13] = {
    "ddddd..ddddd", ".ddd....ddd.", "..d......d..", ".gggggggggg.",
    "grrggggggggg", ".gggggggggg.", "..g......g..", ".gg......gg." };
static const char cloud[5][15] = {
    "....WWWW......", "..WWWWWWWW....", ".WWWWWWWWWWW..", "WWWWWWWWWWWWWW", ".WWWWWWWWWWWW." };

static uint16_t pal(char c)
{
    switch (c) {
    case 'd': return RGB(30, 35, 50);
    case 'w': return RGB(235, 240, 245);
    case 'c': return RGB(40, 200, 255);
    case 'r': return RGB(230, 40, 40);
    case 'y': return RGB(255, 210, 0);
    case 'k': return RGB(100, 60, 25);
    case 'n': return RGB(190, 130, 64);
    case 'o': return RGB(255, 128, 20);
    case 'W': return RGB(255, 255, 255);
    case 'g': return RGB(150, 160, 185);
    default:  return SKY;
    }
}

/* ---- state -------------------------------------------------------------------------------------- */
enum { T_NONE, T_CLOUD, T_CRATE, T_TALL, T_CONES, T_DRONE, T_HDRONE };
typedef struct { int16_t x; uint8_t type, y, w, h, dx, n; } obj_t;
#define NOBJ 6                       /* 2 clouds + 4 obstacles */
static obj_t ob[NOBJ];
static int16_t alt, vel;             /* robot height above the ground, speed: 1/16 px */
static uint8_t held, duck, jbuf, hold_n, anim, fc, prop;
static uint8_t pid, ptop, ph;        /* robot as drawn */
static uint16_t spd, sacc, dist, score, shown, gap, since;
static uint8_t scroll;

/* ---- drawing -------------------------------------------------------------------------------------- */
/* columns c0..c1-1 of a sprite row (w wide, sky beyond), one run per stretch of equal characters */
static void row_runs(const char *s, uint8_t c0, uint8_t c1, uint8_t w)
{
    uint8_t c = c0, n;
    char ch;
    while (c < c1) {
        if (c >= w) { gm_run(SKY, c1 - c); return; }
        ch = s[c];
        for (n = 1; c + n < c1 && c + n < w && s[c + n] == ch; n++) ;
        gm_run(pal(ch), n);
        c += n;
    }
}

static void draw_bot(void)
{
    uint8_t id, top, h, y0, y1, r, c;
    const char *row;
    if (!alt && duck) { id = 3; h = 9; }
    else if (alt) { id = 2; h = 15; }
    else { id = anim; h = 15; }
    top = GROUND - h - (uint8_t)(alt >> 4);
    if (id == pid && top == ptop) return;
    y0 = top < ptop ? top : ptop;
    y1 = top + h > ptop + ph ? top + h : ptop + ph;
    gm_win(PX, y0, PW, y1 - y0);
    for (r = y0; r < y1; r++) {
        if (r < top || r >= top + h) { gm_run(SKY, PW); continue; }
        c = r - top;
        row = id == 3 ? bot_duck[c] : c < 12 ? bot_head[c] : bot_legs[id][c - 12];
        row_runs(row, 0, PW, PW);
    }
    gm_flush();
    pid = id; ptop = top; ph = h;
}

static const char *orow(const obj_t *o, uint8_t r)
{
    switch (o->type) {
    case T_CLOUD: return cloud[r];
    case T_CRATE: case T_TALL: return crate[r % 10];
    case T_CONES: return cones[r];
    default:      return drone[r == 0 && prop ? 1 : r == 1 && prop ? 0 : r];
    }
}

/* the object and the strip it left on its right (o->dx columns) */
static void draw_obj(const obj_t *o)
{
    int16_t x0 = o->x, x1 = o->x + o->w + o->dx;
    uint8_t r;
    if (x0 < 0) x0 = 0;
    if (x1 > LCD_W) x1 = LCD_W;
    if (x0 >= x1) return;
    gm_win((uint8_t)x0, o->y, (uint8_t)(x1 - x0), o->h);
    for (r = 0; r < o->h; r++) row_runs(orow(o, r), (uint8_t)(x0 - o->x), (uint8_t)(x1 - o->x), o->w);
    gm_flush();
}

static const uint8_t peb_len[7] = { 6, 2, 9, 1, 7, 3, 4 };   /* dirt, pebble, ... : 32 px */
static void pebbles(uint8_t y, uint8_t off)
{
    uint8_t i = 0, left = LCD_W, n;
    off &= 31;
    while (off >= peb_len[i]) off -= peb_len[i++];
    n = peb_len[i] - off;
    gm_win(0, y, LCD_W, 1);
    while (left) {
        if (n > left) n = left;
        gm_run(i & 1 ? PEBBLE : DIRT, n);
        left -= n;
        if (++i == 7) i = 0;
        n = peb_len[i];
    }
    gm_flush();
}

static void hud(void)
{
    char b[12];
    gscore_t *g = gm_scores();
    strcpy(b, T(S_HI));
    strcat(b, " ");
    strcat(b, gm_num(g->hop_best > score ? g->hop_best : score));
    gfx_text(3, HUD_Y, b, 1, INK, SKY);
    strcpy(b, gm_num(score));
    gfx_text(LCD_W - 3 - (uint8_t)strlen(b) * FONT_W, HUD_Y, b, 1, INK, SKY);
    shown = score;
}

static void scene(void)
{
    uint8_t i;
    gfx_fill(0, SKIN_TITLE_H, LCD_W, GROUND - SKIN_TITLE_H, SKY);
    gfx_fill(0, GROUND, LCD_W, 2, GRASS);
    gfx_fill(0, GROUND + 2, LCD_W, LCD_H - GROUND - 2, DIRT);
    pebbles(GROUND + 5, scroll);
    pebbles(GROUND + 12, scroll + 13);
    for (i = 0; i < NOBJ; i++) if (ob[i].type) { ob[i].dx = 0; draw_obj(&ob[i]); }
    pid = 0xFF; ptop = GROUND - 15; ph = 15;
    draw_bot();
}

/* ---- game ----------------------------------------------------------------------------------------- */
static void spawn(void)
{
    uint8_t i, r, lvl = score / 150;
    obj_t *o = 0;
    for (i = 2; i < NOBJ; i++) if (!ob[i].type) { o = &ob[i]; break; }
    if (!o) return;
    r = gm_randn(10);
    o->x = LCD_W;
    o->dx = 0;
    if (r >= 8 && lvl >= 3)      { o->type = T_HDRONE; o->w = 12; o->h = 8; o->y = GROUND - 38; }
    else if (r >= 6 && lvl >= 1) { o->type = T_DRONE;  o->w = 12; o->h = 8; o->y = GROUND - 17; }
    else if (r == 5 && lvl >= 1) { o->type = T_TALL;   o->w = 10; o->h = 20; o->y = GROUND - 20; }
    else if (r >= 3)             { o->type = T_CONES;  o->n = 1 + gm_randn(lvl >= 2 ? 3 : 2);
                                   o->w = o->n * 6 - 1; o->h = 9; o->y = GROUND - 9; }
    else                         { o->type = T_CRATE;  o->w = 10; o->h = 10; o->y = GROUND - 10; }
    since = 0;
    gap = 30 + (spd * 18 >> 4) + gm_randn(60) + o->w;
}

static uint8_t hit(const obj_t *o)
{
    int16_t bx0, bx1, by0, by1;
    if (o->type < T_CRATE) return 0;
    if (!alt && duck) { bx0 = PX + 3; bx1 = PX + 11; by0 = GROUND - 7; }
    else { bx0 = PX + 4; bx1 = PX + 11; by0 = GROUND - 13 - (alt >> 4); }
    by1 = GROUND - (alt >> 4);
    return o->x + 1 < bx1 && o->x + o->w - 1 > bx0 && o->y + 1 < by1 && o->y + o->h > by0;
}

static void reset(void)
{
    memset(ob, 0, sizeof ob);
    ob[0].type = ob[1].type = T_CLOUD;
    ob[0].w = ob[1].w = 14; ob[0].h = ob[1].h = 5;
    ob[0].x = 30; ob[0].y = 28;
    ob[1].x = 96; ob[1].y = 34;
    alt = vel = 0;
    held = duck = jbuf = hold_n = anim = fc = 0;
    spd = SPD0; sacc = dist = score = 0; scroll = 0;
    since = 0; gap = 80;
}

/* paused: 0 = on, 1 = quit, 2 = power */
static uint8_t pause(void)
{
    uint8_t k;
    gm_box(3, T(S_PAUSE), T(S_CONT), T(S_QUIT), 0, C_SEL);
    k = gm_key();
    if (k == GM_POWER) return 2;
    if (k == KEY_MENU) return 1;
    scene();
    hud();
    return 0;
}

/* -> 0 title, 1 again, 2 leave (power) */
static uint8_t game(void)
{
    event_t e;
    uint8_t i, k, dx;
    uint16_t next;
    gscore_t *g;
    obj_t *o;
    reset();
    scene();
    hud();
    next = hal_ticks();
    for (;;) {
        next += FRAME;
        hal_watchdog();
        for (;;) {                       /* events until the next frame is due (late: the queued ones) */
            int16_t w = (int16_t)(next - hal_ticks());
            if (w > 0 ? !skin_wait(&e, (uint16_t)w) : !ev_get(&e)) break;
            if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return 2; }
            k = e.code;
            if (e.type == EV_KEY_DOWN) {
                if (k == KEY_PLAY || k == KEY_LEFT) { held = 1; jbuf = 4; }
                else if (k == KEY_RIGHT) duck = 1;
                else if (k == KEY_MENU || k == KEY_VOL) {
                    beep();
                    if ((k = pause()) != 0) return k == 2 ? 2 : 0;
                    held = duck = 0;
                    next = hal_ticks();
                }
            } else if (e.type == EV_KEY_UP) {
                if (k == KEY_PLAY || k == KEY_LEFT) held = 0;
                else if (k == KEY_RIGHT) duck = 0;
            }
        }
        if ((int16_t)(hal_ticks() - next) > 4 * FRAME) next = hal_ticks();   /* fell behind: no catch-up */
        fc++;
        /* robot */
        if (jbuf) {
            if (!alt) { vel = JUMP_V; hold_n = HOLD_MAX; jbuf = 0; }
            else jbuf--;
        }
        if (alt || vel > 0) {
            alt += vel;
            if (duck) vel -= 3 * GRAV;
            else if (held && vel > 0 && hold_n) { vel -= GRAV_HOLD; hold_n--; }
            else vel -= GRAV;
            if (alt <= 0) alt = vel = 0;
        }
        if (!(fc & 3)) anim ^= 1;
        prop = fc & 1;
        /* world */
        sacc += spd;
        dx = (uint8_t)(sacc >> 4);
        sacc &= 15;
        dist += dx;
        scroll += dx;
        since += dx;
        for (i = 0; i < NOBJ; i++) {
            o = &ob[i];
            if (!o->type) continue;
            o->dx = o->type == T_CLOUD ? !(fc & 7) : dx;
            o->x -= o->dx;
            if (o->x + o->w + o->dx <= 0) {
                if (o->type == T_CLOUD) { o->x = LCD_W + gm_randn(40); o->dx = 0; }
                else { o->type = T_NONE; continue; }
            }
            if (o->dx || o->type >= T_DRONE) draw_obj(o);
        }
        if (since >= gap) spawn();
        pebbles(GROUND + 5, scroll);
        pebbles(GROUND + 12, scroll + 13);
        draw_bot();
        score = dist >> 3;
        spd = SPD0 + score / 24;
        if (spd > SPD_MAX) spd = SPD_MAX;
        if (score != shown && !(fc & 3)) hud();
        for (i = 2; i < NOBJ; i++) if (hit(&ob[i])) break;
        if (i < NOBJ) break;
    }
    /* crash */
    beep();
    hud();
    g = gm_scores();
    k = score > g->hop_best;
    if (k) { g->hop_best = score; gm_scores_save(); }
    {
        char b[20];
        strcpy(b, T(S_SCORE)); strcat(b, " "); strcat(b, gm_num(score));
        gm_box(4, T(S_OUCH), b, k ? T(S_NEWBEST) : 0, T(S_AGAIN), RGB(255, 80, 60));
    }
    next = hal_ticks();                                /* half a second to let go of the keys */
    while ((uint16_t)(hal_ticks() - next) < HAL_TICK_HZ / 2) {
        skin_wait(&e, HAL_TICK_HZ / 2);
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return 2; }
    }
    k = gm_key();
    if (k == GM_POWER) return 2;
    return k == KEY_MENU ? 0 : 1;
}

/* title screen: -> 1 play, 0 leave */
static uint8_t title(void)
{
    uint8_t k, y;
    reset();
    ob[0].type = ob[1].type = T_NONE;            /* no clouds behind the title */
    scene();
    hud();
    gfx_text((LCD_W - 6 * FONT_W * 2) / 2, 30, T(S_TITLE), 2, RGB(255, 255, 255), SKY);
    for (y = 0; y < 5; y++) gfx_text(4, 50 + y * 9, T(S_H_JUMP + y), 1, INK, SKY);
    skin_footer(T(S_START));
    for (;;) {
        k = gm_key();
        if (k == KEY_PLAY) return 1;
        if (k == KEY_MENU || k == GM_POWER) return 0;
    }
}

uint16_t mod_main(void)
{
    uint8_t r;
    skin_load();
    gm_seed(hal_ticks());
    skin_title(T(S_TITLE));
    while (title()) {
        do r = game(); while (r == 1);
        if (r == 2) break;
    }
    return 0;
}
