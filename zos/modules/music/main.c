/* MUSIC: folder browser of the zos music players (folders + *.MP3 *.OGG *.WAV, long names).
 * << >> move, PLAY open folder / play, MENU up / exit. Playing hands over to MP3PLAY.ZM (MP3) or
 * DECPLAY.ZM (OGG, WAV; both ZRAM2): the playlist (this folder's tracks from the chosen one on, each
 * with its format, shared.h TRK_*) and the browse state go to the shared
 * block (shared.h); MP3PLAY chains back here, and the browser comes back where it was.
 * Started from the shell with a resume point in ZCFG (last file played, ZOS.CFG): goes straight
 * back into that file at the saved position; MENU in the player then opens this browser.
 * Coming back from the player saves ZOS.CFG (resume point, volume). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/fat.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "shared.h"
#include "strings.h"
#include "../common/skin.h"
#include "../common/beep.h"
#include "../common/cfg.h"

#define MAXE 48
#define ROWS 9
#define NAMEW 19                                   /* shown: 12 + 19 x 6 px = 126 */
#define NAMEMAX FAT_LFN_MAX                        /* kept: longer names scroll on the selected row */
typedef struct { uint32_t clus, size; uint8_t dir; char name[NAMEMAX + 1]; } ent_t;   /* dir: FAT_ATTR_DIR or TRK_* */
static ent_t ents[MAXE];
static uint8_t n;
static fat_dirent_t de;
#define S MUS

static void upname(char *d, const fat_dirent_t *e, uint8_t w)
{
    uint8_t i, j = 0;
    if (e->lfn[0]) strncpy(d, e->lfn, w);
    else {
        for (i = 0; i < 8 && e->name[i] != ' '; i++) d[j++] = e->name[i];
        if (e->name[8] != ' ') { d[j++] = '.'; for (i = 8; i < 11 && e->name[i] != ' '; i++) d[j++] = e->name[i]; }
        d[j] = 0;
    }
    d[w] = 0;
    for (i = 0; d[i]; i++) {
        if (d[i] >= 'a' && d[i] <= 'z') d[i] -= 32;
        else if (d[i] < 32 || d[i] > 90) d[i] = '?';   /* font covers ASCII 32..90 */
    }
}

static void dir_open(fat_file_t *d)               /* the folder at S->depth */
{
    if (!S->depth) { fat_root(d); return; }
    memset(&de, 0, sizeof de);
    de.clus = S->dclus[S->depth];
    de.attr = FAT_ATTR_DIR;
    fat_open(d, &de);
}

static void load(void)
{
    fat_file_t d;
    uint8_t k;
    dir_open(&d);
    n = 0;
    while (n < MAXE && fat_readdir(&d, &de)) {
        if (de.name[0] == '.' && de.name[1] == ' ') continue;
        if (de.attr & 0x0A) continue;                  /* hidden, volume */
        k = de.attr & FAT_ATTR_DIR;
        if (!k) {                                      /* playable formats only */
            if (!memcmp(de.name + 8, "MP3", 3)) k = TRK_MP3 | 1;
            else if (!memcmp(de.name + 8, "OGG", 3)) k = TRK_OGG;
            else if (!memcmp(de.name + 8, "WAV", 3)) k = TRK_WAV;
            else continue;
        }
        ents[n].clus = de.clus;
        ents[n].size = de.size;
        ents[n].dir = k;
        upname(ents[n].name, &de, NAMEMAX);
        n++;
    }
}

#define TOP_Y 17                                   /* below the skin title bar */
#define ROW_H 11

/* type icons, 10x9, compiled in (no disk reads per row): bit 9 = left column */
enum { TY_DIR, TY_MP3, TY_OGG, TY_WAV };
static const uint16_t ty_bits[4][9] = {
    { 0x3C0, 0x3FF, 0x3FF, 0x201, 0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x000 },   /* folder */
    { 0x03F, 0x03F, 0x021, 0x021, 0x021, 0x0E7, 0x1EF, 0x1EF, 0x0C6 },   /* beamed notes */
    { 0x078, 0x1FE, 0x1FE, 0x3CF, 0x3CF, 0x1FE, 0x1FE, 0x078, 0x000 },   /* disc */
    { 0x010, 0x050, 0x054, 0x155, 0x3FF, 0x155, 0x054, 0x050, 0x010 } }; /* waveform */
static const uint16_t ty_col[4] = { RGB(240, 190, 60), RGB(90, 220, 110), RGB(255, 140, 40), RGB(80, 200, 255) };

static uint8_t type_of(uint8_t i)
{
    uint8_t d = ents[i].dir;
    if (d == FAT_ATTR_DIR) return TY_DIR;
    if ((d & TRK_KIND) == TRK_OGG) return TY_OGG;
    if ((d & TRK_KIND) == TRK_WAV) return TY_WAV;
    return TY_MP3;
}

static void type_icon(uint8_t t, uint8_t x, uint8_t y)   /* set pixels only, one fill per run */
{
    uint8_t r, c, c0;
    uint16_t v;
    for (r = 0; r < 9; r++) {
        v = ty_bits[t][r];
        for (c = 0; c < 10; ) {
            if (!(v & (0x200 >> c))) { c++; continue; }
            for (c0 = c; c < 10 && (v & (0x200 >> c)); c++) ;
            gfx_fill(x + c0, y + r, c - c0, 1, ty_col[t]);
        }
    }
}

/* horizontal scroll of the selected row's name (as MP3PLAY's title: one step per 300 ms, a pause
 * at both ends); only the text part of that row is redrawn */
#define SCR_TICKS 30
#define SCR_PAUSE 4
static uint8_t soff, spause;
static uint16_t selbar[ROW_H];                     /* SELBAR.ZI colour of each row (skinlib.c) */
static uint8_t selbar_ok;

static void name_text(uint8_t i, uint8_t y, uint8_t s)
{
    char b[NAMEW + 1];
    uint8_t r;
    memcpy(b, ents[i].name + (s ? soff : 0), NAMEW);
    b[NAMEW] = 0;
    if (s && selbar_ok) {                          /* the bar's gradient under the text, then the text */
        for (r = 0; r < ROW_H; r++) gfx_fill(12, y + r, LCD_W - 12, 1, selbar[r]);
        skin_text(12, y + 2, b, sk.sel_text);
    } else {
        r = (uint8_t)strlen(b);
        gfx_text(12, y + 2, b, 1, s ? sk.sel_text : sk.fg, s ? sk.sel_bot : sk.bg);
        gfx_fill(12 + r * FONT_W, y + 2, (NAMEW - r) * FONT_W + 2, 8, s ? sk.sel_bot : sk.bg);
    }
}

static void row(uint8_t i)
{
    uint8_t y = TOP_Y + (i - S->top) * ROW_H, s = i == S->sel;
    if (s) { soff = 0; spause = SCR_PAUSE; }
    if (!(s && selbar_ok && skin_draw("SELBAR  ZI ", 0, y, 0)))
        gfx_fill(0, y, LCD_W, ROW_H, s ? sk.sel_bot : sk.bg);
    type_icon(type_of(i), 1, y + 1);
    name_text(i, y, s);
}

static uint8_t scroll(void)                       /* 1 = the selected name is longer than the row */
{
    uint8_t len;
    if (!n) return 0;
    len = (uint8_t)strlen(ents[S->sel].name);
    if (len <= NAMEW) return 0;
    if (spause) { spause--; return 1; }
    if (++soff + NAMEW > len) { soff = 0; spause = SCR_PAUSE; }
    else if (soff + NAMEW == len) spause = SCR_PAUSE;
    name_text(S->sel, TOP_Y + (S->sel - S->top) * ROW_H, 1);
    return 1;
}

static void list(void)
{
    uint8_t i;
    gfx_fill(0, 0, LCD_W, LCD_H, sk.bg);
    skin_title(T(S_TITLE));
    for (i = S->top; i < n && i < S->top + ROWS; i++) row(i);
    if (!n) gfx_text(4, TOP_Y + 4, T(S_EMPTY), 1, sk.dim, sk.bg);
    gfx_text(4, LCD_H - 9, T(S_FOOTER), 1, sk.dim, sk.bg);
}

/* the players' static chrome (shared.h): skinned title bar, and the colours of their progress / volume
 * bars row by row (one column of PROGBAR.ZI / VOLBAR.ZI; empty part = the same at 1/4 brightness,
 * volume mode = halfway to white). No bar image: the theme's flat colours, the progress bar 4 rows
 * high and the volume bar 6 (bg rows) as before. */
uint8_t skin_column(const char *name11, uint16_t *col, uint8_t h);   /* skinlib.c */
#define DARK(c)  (((c) >> 2) & 0x39E7)
#define LIGHT(c) ((((c) >> 1) & 0x7BEF) + 0x7BEF)
static void player_chrome(void)
{
    uint16_t (*b)[BAR_H] = S->bar, c;
    uint8_t r, p = skin_column("PROGBAR ZI ", b[BAR_PON], BAR_H),
            v = skin_column("VOLBAR  ZI ", b[BAR_VON], BAR_H), edge;
    for (r = 0; r < BAR_H; r++) {
        edge = r < 2 || r == BAR_H - 1;
        if (p) b[BAR_POFF][r] = DARK(b[BAR_PON][r]);
        else {
            b[BAR_PON][r] = edge ? sk.bg : sk.sel_top;
            b[BAR_POFF][r] = edge ? sk.bg : sk.bar;
        }
        if (v) {
            c = b[BAR_VON][r];
            b[BAR_VOFF][r] = DARK(c);
            b[BAR_VSEL][r] = LIGHT(c);
        } else {
            b[BAR_VON][r] = r ? sk.fg : sk.bg;
            b[BAR_VOFF][r] = r ? sk.bar : sk.bg;
            b[BAR_VSEL][r] = r ? sk.sel_bot : sk.bg;
        }
    }
    gfx_fill(0, 0, LCD_W, SKIN_TITLE_H, sk.bg);       /* (no TITLE.ZI: a 12-row bar) */
    skin_title(T(S_PLAYING));
}

/* playlist = tracks of this folder from the selected one on, then chain to the first one's player */
static uint16_t play(void)
{
    uint8_t i;
    mus_trk_t *t;
    S->ntrk = S->cur = 0;
    ZCFG->res_dir = S->depth ? S->dclus[S->depth] : 0;
    for (i = S->sel; i < n && S->ntrk < MUS_TRACKS; i++) {
        if (ents[i].dir == FAT_ATTR_DIR) continue;
        t = &S->trk[S->ntrk++];
        t->clus = ents[i].clus;
        t->size = ents[i].size;
        t->idx = i | (ents[i].dir & TRK_KIND);
        memcpy(t->name, ents[i].name, MUS_NAMEW);
        t->name[MUS_NAMEW] = 0;
    }
    memcpy(S->next, S->trk[0].idx & TRK_KIND ? DECPLAY_ZM : MP3PLAY_ZM, 12);
    player_chrome();

    return MOD_CHAIN;
}

uint16_t mod_main(void)
{
    event_t e;
    uint8_t old;
    if (!skin_load() && !fat_mount()) {
        gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
        gfx_text(4, 20, T(S_NODISK), 1, C_SEL, C_BG);
        ev_wait(&e, 200);
        return 1;
    }
    if (S->magic != MUS_MAGIC) {                       /* from the shell: root folder ... */
        memset(S, 0, sizeof *S);
        S->magic = MUS_MAGIC;
        S->vol = ZCFG->magic == ZCFG_MAGIC && ZCFG->vol <= 31 ? ZCFG->vol : VOL_DEF;
        if (ZCFG->magic == ZCFG_MAGIC && ZCFG->res_file) {   /* ... or straight back into the last file */
            if (ZCFG->res_dir) S->dclus[++S->depth] = ZCFG->res_dir;
            load();
            for (old = 0; old < n && (ents[old].dir == FAT_ATTR_DIR || ents[old].clus != ZCFG->res_file); old++) ;
            if (old < n) {
                S->sel = old;
                S->top = old >= ROWS ? old - ROWS + 1 : 0;
                S->start_pos = ZCFG->res_pos;
                S->start_paused = 1;               /* do not start playing by itself */
                return play();
            }
            ZCFG->res_file = 0;                        /* gone: browse from the root */
            S->depth = 0;
        }
    } else if (S->from_player) {                       /* back from a player: keep the resume point */
        S->from_player = 0;
        cfg_save();
        if (MOD_POWER_REQ) { S->magic = 0; return 0; }  /* player left for power-off: on to SHELL */
    }
    selbar_ok = skin_column("SELBAR  ZI ", selbar, ROW_H);
    load();
    if (S->sel >= n) S->sel = S->top = 0;
    list();
    for (;;) {
        if (!ev_wait(&e, n && strlen(ents[S->sel].name) > NAMEW ? SCR_TICKS : 0)) { scroll(); continue; }
        if (e.type == EV_KEY_DOWN) beep();             /* key beep (SETTINGS) */
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; break; }
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        old = S->sel;
        if (e.code == KEY_RIGHT && S->sel + 1 < n) S->sel++;
        if (e.code == KEY_LEFT && S->sel) S->sel--;
        if (S->sel != old) {
            if (S->sel < S->top || S->sel >= S->top + ROWS) {
                S->top = S->sel < S->top ? S->sel : S->sel - ROWS + 1;
                list();
            } else { row(old); row(S->sel); }
        }
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) {
            if (!S->depth) break;
            S->depth--;
        } else if (e.code == KEY_PLAY && n) {
            if (ents[S->sel].dir != FAT_ATTR_DIR) return play();
            if (S->depth >= 5) continue;
            S->dclus[++S->depth] = ents[S->sel].clus;
        } else continue;
        S->sel = S->top = 0;
        load();
        list();
    }
    S->magic = 0;                                      /* next MUSIC start: from the root */
    return 0;
}
