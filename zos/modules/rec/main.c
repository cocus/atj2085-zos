/* REC: voice recorder, UI + file part (page module). Microphone -> stock ADENCM.DSP (IMA ADPCM
 * WAV, 512-byte blocks of 1017 samples, mono) -> RECnnn.WAV in the disk ROOT (choice: the root
 * needs no directory cluster; a VOICE folder would have to be created first).
 * Rate 16000 / 22050 (default) / 32000 Hz with << >>: the ADC rate, and the WAV header's rate +
 * byte-rate fields (ADENCM is rate-agnostic, its header template says 8000 Hz).
 * PLAY: the largest free cluster run (common/fatw.c), the next free RECnnn name, sector 0 as a
 * placeholder, then RECRUN (ZRAM2 module, see rechand.h for why) records; RECRUN comes back here
 * and the file is committed (FAT1, FAT2, root entry). Needs ADENCM.DSP in the disk root (the
 * build extracts it from the user's own firmware, like MP3DEC.DSP). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/fat.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/ftlw.h"
#include "../common/fatw.h"
#include "../common/vu.h"
#define ZS_PROVIDE
#include "strings.h"
#include "rechand.h"
#include "recui.h"

#define BUF ((uint8_t *)HAL_MODRAM_BASE)          /* ZRAM1: fatw / NAND DMA buffer */
static const char *const rate_s[NRATES] = { "16000 HZ", "22050 HZ", "32000 HZ" };
static const uint16_t rate_hz[NRATES] = { 16000, 22050, 32000 };
static uint8_t ri = RATE_22K;

static void value(uint8_t y, const char *s, uint16_t c)
{
    gfx_fill(REC_X_VAL, y, LCD_W - REC_X_VAL, 8, C_BG);
    gfx_text(REC_X_VAL, y, s, 1, c, C_BG);
}
static void state(uint8_t s, uint16_t c)
{
    gfx_fill(0, REC_Y_STATE, LCD_W, 10, C_BG);
    gfx_text(4, REC_Y_STATE, T(s), 1, c, C_BG);
}
static void name_line(const char *n, uint16_t c)
{
    static char t[13];
    uint8_t i, k = 0;
    for (i = 0; i < 8 && n[i] != ' '; i++) t[k++] = n[i];
    t[k++] = '.';
    for (i = 8; i < 11 && n[i] != ' '; i++) t[k++] = n[i];
    t[k] = 0;
    gfx_fill(0, REC_Y_STATE + 12, LCD_W, 8, C_BG);
    gfx_text(4, REC_Y_STATE + 12, t, 1, c, C_BG);
}
static void mmss(char *t, uint16_t s)
{
    uint16_t m = s / 60;
    if (m > 99) m = 99;
    t[0] = '0' + m / 10; t[1] = '0' + m % 10; t[2] = ':';
    s %= 60; t[3] = '0' + s / 10; t[4] = '0' + s % 10; t[5] = 0;
}

static void screen(void)
{
    char t[6];
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    gfx_text(4, REC_Y_RATE, T(S_RATE), 1, C_DIM, C_BG);
    gfx_text(4, REC_Y_TIME, T(S_TIME), 1, C_DIM, C_BG);
    gfx_text(4, REC_Y_FREE, T(S_FREE), 1, C_DIM, C_BG);
    gfx_text(4, REC_Y_LOST, T(S_DROPS), 1, C_DIM, C_BG);
    value(REC_Y_RATE, rate_s[ri], C_FG);
    mmss(t, 0);
    value(REC_Y_TIME, t, C_FG);
    gfx_fill(4, REC_Y_VU, VU_W, VU_H, C_DIM);
}

static void err_line(uint8_t s, uint8_t code)
{
    static char t[] = "CODE 00";
    state(s, C_SEL);
    t[5] = "0123456789ABCDEF"[code >> 4];
    t[6] = "0123456789ABCDEF"[code & 15];
    gfx_fill(0, REC_Y_STATE + 12, LCD_W, 8, C_BG);
    gfx_text(4, REC_Y_STATE + 12, t, 1, C_SEL, C_BG);
}

/* PLAY: prepare the file and hand over to RECRUN; 1 = chain */
static uint8_t prepare(void)
{
    uint8_t r;
    fat_file_t f;
    fat_dirent_t e;
    fat_root(&f);
    if (!fat_find(&f, "ADENCM  DSP", &e)) { state(S_NODSP, C_SEL); return 0; }
    state(S_PREP, C_FG);
    memcpy(rh.name, "REC000  WAV", 12);
    if ((r = fatw_open(BUF, 2))) { err_line(r == FATW_EFULL ? S_NOROOM : S_ERROR, r); return 0; }
    if (!fatw_next_name(rh.name)) { state(S_ERROR, C_SEL); return 0; }
    memset(BUF, 0, 512);                       /* sector 0: placeholder, the DSP sends the header last */
    if ((r = fatw_put(BUF)) || (r = ftlw_flush())) { err_line(S_ERROR, r); return 0; }
    rh.rate = ri;
    rh.flags = 0;
    rh.err = 0;
    rh.c0 = fatw_c0;
    rh.n = fatw_n;
    rh.lba0 = fatw_lba0();
    rh.room = fatw_room() + 1;
    rh.nsec = 1;
    rh.drops = rh.secs = 0;
    rh.state = RH_RECORD;
    rh.magic = REC_MAGIC;
    name_line(rh.name, C_FG);
    memcpy((void *)MOD_CHAIN_NAME, "RECRUN  ZM ", 12);
    return 1;
}

/* back from RECRUN: commit what it wrote */
static void finish(void)
{
    uint8_t r;
    char t[6];
    rh.magic = 0;
    mmss(t, rh.secs);
    value(REC_Y_TIME, t, C_FG);
    if (rh.drops) { gfx_fill(REC_X_VAL, REC_Y_LOST, 40, 8, C_BG); gfx_num(REC_X_VAL, REC_Y_LOST, rh.drops, 1, C_SEL, C_BG); }
    name_line(rh.name, C_FG);
    if (rh.nsec < 2) { err_line(S_ERROR, rh.err); return; }   /* nothing recorded: the run stays free */
    state(S_SAVING, C_FG);
    if ((r = fatw_resume(BUF, rh.c0, rh.n, rh.nsec)) ||
        (r = fatw_commit(rh.name, rh.nsec << 9))) { err_line(S_ERROR, r); return; }
    if (rh.err == 0xFE) err_line(S_NODSP, rh.err);
    else if (rh.err) err_line(S_ERROR, rh.err);
    else if (!(rh.flags & RH_F_HEADER)) state(S_NOHDR, C_SEL);   /* data kept, sector 0 empty */
    else {
        state(rh.flags & RH_F_FULL ? S_FULLSAVED : S_SAVED, C_OK);
        name_line(rh.name, C_OK);
    }
}

uint16_t mod_main(void)
{
    event_t e;
    skin_load();
    if (rh.magic == REC_MAGIC && rh.state == RH_DONE) {
        ri = rh.rate < NRATES ? rh.rate : RATE_22K;
        screen();
        finish();
        if (rh.flags & RH_F_POWER) { MOD_POWER_REQ = 1; return 0; }
    } else {
        rh.magic = 0;
        screen();
        state(S_READY, C_OK);
    }
    skin_footer(T(S_FT_IDLE));
    for (;;) {
        ev_wait(&e, 0);
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return 0; }
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) return 0;
        if (e.code == KEY_LEFT || e.code == KEY_RIGHT) {
            ri = e.code == KEY_RIGHT ? (ri + 1) % NRATES : (ri + NRATES - 1) % NRATES;
            value(REC_Y_RATE, rate_s[ri], C_FG);
        } else if (e.code == KEY_PLAY) {
            if (prepare()) return MOD_CHAIN;
        }
    }
}
