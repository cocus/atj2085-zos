/* MP3PLAY: the zos MP3 player screen (ZRAM2 module, started by MUSIC.ZM with a playlist in the
 * shared block). PLAY pause/resume, << >> (short press) previous/next track, << >> held = scrub
 * (the target time runs while held, playback restarts there on release), VOL toggles volume mode
 * (<< >> = volume), MENU back to the MUSIC browser. Plays the playlist through. Title/artist from
 * the tags (id3.c). The resume point (file + position) and the volume go to ZCFG for MUSIC to save.
 * EV_POWER (PLAY held 2 s): leaves like MENU with MOD_POWER_REQ set (MUSIC saves, SHELL powers off).
 * Also the screen/keys of DECPLAY (OGG/WAV): modules/decplay/main.c includes this file with its own
 * PLAYER_* settings and its own backend behind mp3.h. A playlist track of the other player's format
 * (shared.h TRK_KIND) chains to that player, which starts it. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../music/shared.h"
#include "mp3.h"
#include "../common/draw.h"
#define ZS_PROVIDE
#include "strings.h"
#include "ui.h"

#ifndef PLAYER_ZM                       /* MP3PLAY (defaults); DECPLAY defines its own */
#define PLAYER_ZM    MP3PLAY_ZM         /* this module's file (code2_load) */
#define PLAYER_OTHER DECPLAY_ZM         /* plays the tracks this one does not */
#define PLAYER_NOT_MINE(idx) ((idx) & TRK_KIND)   /* not an MP3 */
#define PLAYER_NODSP "NO MP3DEC.DSP"
#endif


#ifdef MP3_DIAG
void mp3_diag(uint8_t slot, char c)            /* breadcrumbs at the top right of the counter line */
{
    gfx_char(104 + slot * 6, 30, c, 1, C_SEL, C_BG);
}
#endif

#ifdef MP3_STEP
/* bring-up: show the next stage and wait for PLAY (a reboot right after = that stage) */
void mp3_step(uint8_t n, const char *what)
{
    event_t ev;
    gfx_fill(0, 104, LCD_W, 24, C_BAR);
    gfx_char(2, 106, '0' + n, 1, C_SEL, C_BAR);
    gfx_text(12, 106, what, 1, C_FG, C_BAR);
    gfx_text(2, 117, "PLAY: DO IT", 1, C_SEL, C_BAR);
    do ev_wait(&ev, 0); while (ev.type != EV_KEY_DOWN || ev.code != KEY_PLAY);
    gfx_fill(0, 104, LCD_W, 24, C_BG);
}
#endif

/* The DSP asks for 512 bytes every ~13 ms (320 kbps): the screen is drawn in small pieces and
 * pump() answers pending DSP requests between them (key events are kept for the main loop). */
static event_t key;
static void pump(void)
{
    event_t e;
    while (ev_get(&e)) {
        if (e.type == EV_DSP) mp3_service(e.code);
        else if (e.type != EV_SECOND_HALF) key = e;      /* keys, EV_POWER */
    }
}


static uint8_t held, reps;                              /* << >> held */
static char req_sec;



/* "mm:ss", or "h:mm:ss" for files of an hour or more (hrs) */
void tfmt(char *t, uint16_t s, uint8_t hrs)
{
    uint8_t m = s / 60 % 60;
    if (hrs) { *t++ = '0' + s / 3600 % 10; *t++ = ':'; }
    else m = s / 60 > 99 ? 99 : s / 60;
    s %= 60;
    t[0] = '0' + m / 10; t[1] = '0' + m % 10; t[2] = ':';
    t[3] = '0' + s / 10; t[4] = '0' + s % 10; t[5] = 0;
}

#ifdef FOOTER_IN_MAIN
void footer(void)
{
    fill(0, LCD_H - 10, LCD_W, 10, C_BG);
    txt(4, LCD_H - 9, T(volmode ? S_FT_VOL : S_FT_PLAY), C_DIM);
    vol_shown = 0xFF;
}
#endif

/* redraw only what changed, one small piece at a time */
static void update(void)
{
    char t[8];
    uint16_t now = scrub ? target : mp3.base_s + mp3.now_s, tot = mp3.total_s;
    uint8_t i, w, hrs = tot >= 3600, n = hrs ? 7 : 5;
    tfmt(t, now, hrs);
    for (i = 0; i < n; i++)
        if (t[i] != big[i]) {
            big[i] = t[i];
            gfx_char((hrs ? 1 : 4) + i * 18, 50, t[i], 3, scrub ? C_SEL : C_FG, C_BG);
            pump();
        }
    if (tot != tot_shown) { tot_shown = tot; tfmt(t, tot, hrs); txt(12, 78, t, C_DIM); pump(); }
    if (memcmp(kb, mp3.kbps, 3)) {
        memcpy(kb, mp3.kbps, 3);
        for (i = 0; i < 3; i++) gfx_char(64 + i * 6, 78, kb[i] | 0x20, 1, C_DIM, C_BG);
        pump();
    }
    while (tot > 500) { tot >>= 1; now >>= 1; }        /* 16-bit progress */
    w = tot ? (uint8_t)(now * 120 / tot) : 0;
    if (w > 120) w = 120;
    if (w > bar_w) fill(4 + bar_w, 92, w - bar_w, 4, C_OK);
    else if (w < bar_w) fill(4 + w, 92, bar_w - w, 4, C_BAR);
    bar_w = w;
    if (mp3.vol != vol_shown) {
        vol_shown = mp3.vol;
        fill(28, 103, 93, 6, C_BAR);
        fill(28, 103, mp3.vol * 3, 6, volmode ? C_SEL : C_FG);
        pump();
    }
#ifdef MP3_STEP
    if (mp3.nreq != req_shown && t[4] != req_sec) {    /* once per second */
        req_sec = t[4];                       /* DSP requests answered + last code */
        req_shown = mp3.nreq;
        gfx_text(4, 40, "R", 1, C_DIM, C_BG);
        gfx_num(12, 40, mp3.nreq, 1, C_DIM, C_BG);
        gfx_char(48, 40, "0123456789ABCDEF"[mp3.last >> 4], 1, C_DIM, C_BG);
        gfx_char(54, 40, "0123456789ABCDEF"[mp3.last & 15], 1, C_DIM, C_BG);
        pump();
    }
#endif
    if (mp3.state != paused_shown) {
        paused_shown = mp3.state;
        txt(4, 40, mp3.state == MP3_PAUSED ? T(S_PAUSED) : "      ", C_SEL);
    }
    if (mp3.mode != mode_shown && mp3.total_s) {        /* speaker: 1 wave mono, 2 stereo */
        mode_shown = mp3.mode;
        spk_icon(116, 40, mp3.mode != MP3_MODE_MONO, C_DIM, C_BG);
    }
}

static fat_dirent_t cur_e;
static mus_trk_t *cur_trk(void) { return &S->trk[S->cur]; }   /* the playlist track playing */
static uint8_t restart(uint32_t off)                    /* current track from byte off */
{
    scrub = 0;
    loading();
    if (!mp3_start(&cur_e, off)) return 0;
    screen();
    return 1;
}


void track_info(void)                                   /* track S->cur: file + tags */
{
    memset(&cur_e, 0, sizeof cur_e);
    cur_e.clus = cur_trk()->clus;
    cur_e.size = cur_trk()->size;
    mp3_stop();                                        /* id3_read borrows the bitstream buffer */
    loading();
    id3_read(&cur_e, ti, ar);
    if (!ti[0]) strcpy(ti, cur_trk()->name);
    ZCFG->res_file = cur_e.clus;
}

static uint8_t play(uint32_t off)                       /* track S->cur */
{
    S->start_paused = 0;                                /* playing from now on */
    if (PLAYER_NOT_MINE(cur_trk()->idx)) return 0;  /* the main loop chains to the other player */
    track_info();
    return restart(off);
}


/* << >> released: short press = previous/next track, after a hold = seek to the target */
static void lr_release(uint8_t code)
{
    if (scrub) {
        if (mp3.bps) restart(mp3_offset(target));
        scrub = 0;
    } else if (code == KEY_RIGHT && S->cur + 1 < S->ntrk) { S->cur++; play(0); }
    else if (code == KEY_LEFT && S->cur) { S->cur--; play(0); }
    held = 0;
}

static void lr_hold(uint8_t code)                       /* auto-repeat: move the target */
{
    uint8_t step = reps < 10 ? 5 : reps < 30 ? 15 : 60;
    if (!scrub) { scrub = 1; target = mp3.base_s + mp3.now_s; memset(big, 0, sizeof big); }   /* redraw all digits highlighted */
    if (reps < 255) reps++;
    if (code == KEY_RIGHT) target = target + step < mp3.total_s ? target + step : mp3.total_s;
    else target = target > step ? target - step : 0;
}

/* the cold path (mp3cold.c, section _CODE2) lives in ZRAM1; build.py stores it in this module's
 * file at offset 0x1000, behind the 4 KB page image the core loader copies */
extern const uint16_t code2_addr, code2_len;
#define CODE2_FILE_OFFSET 0x1000
static uint8_t code2_load(void)
{
    fat_file_t f;
    fat_dirent_t e;
    fat_root(&f);
    if (!fat_find(&f, PLAYER_ZM, &e)) return 0;
    fat_open(&f, &e);
    return fat_seek(&f, CODE2_FILE_OFFSET) &&
           fat_read(&f, (void *)code2_addr, code2_len) == code2_len;
}

uint16_t mod_main(void)
{
    event_t ev;
    if (S->magic != MUS_MAGIC || !S->ntrk || !code2_load()) return 0;
    mp3.vol = S->vol;
    mp3_cpu(0);

    /* show loading in app (play() calls loading())*/
    screen();

    //cpu_fast();
#ifdef MP3_STEP
    screen();                                          /* reaching this = ZRAM2 code runs */
    mp3_step(1, "PLAYER IN ZRAM2 OK");
#endif
    if (S->start_paused) {                             /* resume from the shell: shown paused, the */
        track_info();                                  /*   first PLAY / << >> starts (main loop) */
        screen();
        mp3.state = MP3_PAUSED;
    } else if (!play((uint32_t)S->start_pos << RES_POS_SHIFT)) {
        gfx_text(4, 64, PLAYER_NODSP, 1, C_SEL, C_BG);
        ev_wait(&ev, 200);
        goto out;
    }
    for (;;) {
        if (PLAYER_NOT_MINE(cur_trk()->idx)) break;   /* next/previous: the other player's format */
        ev_wait(&ev, 50);
        if (ev.type == EV_DSP) mp3_service(ev.code);
        else if (ev.type != EV_SECOND_HALF) key = ev;
        pump();
#ifdef PLAYER_POLL
        PLAYER_POLL();
#endif
        if (mp3.state == MP3_DONE) {                   /* next track */
            if (S->cur + 1 >= S->ntrk) { ZCFG->res_file = 0; break; }   /* list played through */
            S->cur++;
            if (!play(0)) break;
            continue;
        }
        ev = key;
        key.type = EV_NONE;
        if (ev.type == EV_POWER) { MOD_POWER_REQ = 1; break; }
        if (ev.type == EV_KEY_UP) {
            if (!volmode && (ev.code == KEY_LEFT || ev.code == KEY_RIGHT)) lr_release(ev.code);
        } else if (ev.type == EV_KEY_DOWN || ev.type == EV_KEY_REPEAT) {
            if (ev.code == KEY_MENU) break;
            if (ev.code == KEY_VOL) { if (ev.type == EV_KEY_DOWN) { volmode ^= 1; footer(); } }
            else if (volmode)
                mp3_volume(ev.code == KEY_RIGHT ? mp3.vol + 1 : (mp3.vol ? mp3.vol - 1 : 0));
            else if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) {
                if (ev.type == EV_KEY_DOWN) { held = ev.code; reps = 0; }
                else lr_hold(ev.code);
            } else if (ev.type == EV_KEY_DOWN && ev.code == KEY_PLAY) {
                if (S->start_paused) play((uint32_t)S->start_pos << RES_POS_SHIFT);
                else mp3_pause(mp3.state == MP3_PLAYING);
            }
        }
        update();
        scroll();
    }
out:
    if (ZCFG->res_file && !S->start_paused)  /* never started: keep the point */
        ZCFG->res_pos = (uint16_t)(mp3_file.pos >> RES_POS_SHIFT);
    S->start_paused = 0;                              /* the browser starts tracks playing */
    ZCFG->vol = mp3.vol;
    S->from_player = 1;
    held = PLAYER_NOT_MINE(cur_trk()->idx) != 0;   /* (reused) chaining to the other player */
    mp3_off(held);
    mp3_cpu(0);
    S->vol = mp3.vol;
    S->start_pos = 0;
    S->sel = cur_trk()->idx & TRK_IDX;             /* the browser shows the last track */
    if (S->sel >= S->top + 9) S->top = S->sel - 8;
    memcpy(S->next, held ? PLAYER_OTHER : "MUSIC   ZM ", 12);
    return MOD_CHAIN;
}
