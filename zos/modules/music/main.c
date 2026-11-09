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
#define NAMEW 19                                   /* 12 + 19 x 6 px = 126 */
typedef struct { uint32_t clus, size; uint8_t dir; char name[NAMEW + 1]; } ent_t;   /* dir: FAT_ATTR_DIR or TRK_* */
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

static void load(void)
{
    fat_file_t d = S->dirs[S->depth];
    uint8_t k;
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
        upname(ents[n].name, &de, NAMEW);
        n++;
    }
}

#define TOP_Y 17                                   /* below the skin title bar */
#define ROW_H 11

static void row(uint8_t i)
{
    uint8_t y = TOP_Y + (i - S->top) * ROW_H, s = i == S->sel;
    uint8_t icon = ents[i].dir == FAT_ATTR_DIR ? ICON_FOLDER : ICON_AUDIO;
    if (s && skin_draw("SELBAR  ZI ", 0, y, 0)) {         /* gradient bar, transparent text */
        skin_draw("ICONSEL ZI ", 1, y + 1, icon);
        skin_text(12, y + 2, ents[i].name, sk.sel_text);
        return;
    }
    gfx_fill(0, y, LCD_W, ROW_H, s ? sk.sel_bot : sk.bg);
    if (!skin_draw("ICONS   ZI ", 1, y + 1, icon) && ents[i].dir == FAT_ATTR_DIR)
        gfx_text(1, y + 2, ">", 1, sk.sel_bot, sk.bg);
    gfx_text(12, y + 2, ents[i].name, 1, s ? sk.sel_text : sk.fg, s ? sk.sel_bot : sk.bg);
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

/* playlist = tracks of this folder from the selected one on, then chain to the first one's player */
static uint16_t play(void)
{
    uint8_t i;
    mus_trk_t *t;
    S->ntrk = S->cur = 0;
    ZCFG->res_dir = S->dirs[S->depth].start;
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
        fat_root(&S->dirs[0]);
        if (ZCFG->magic == ZCFG_MAGIC && ZCFG->res_file) {   /* ... or straight back into the last file */
            if (ZCFG->res_dir) {
                memset(&de, 0, sizeof de);
                de.clus = ZCFG->res_dir;
                de.attr = FAT_ATTR_DIR;
                fat_open(&S->dirs[++S->depth], &de);
            }
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
    load();
    if (S->sel >= n) S->sel = S->top = 0;
    list();
    for (;;) {
        ev_wait(&e, 0);
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
            memset(&de, 0, sizeof de);
            de.clus = ents[S->sel].clus;
            de.attr = FAT_ATTR_DIR;
            fat_open(&S->dirs[++S->depth], &de);
        } else continue;
        S->sel = S->top = 0;
        load();
        list();
    }
    S->magic = 0;                                      /* next MUSIC start: from the root */
    return 0;
}
