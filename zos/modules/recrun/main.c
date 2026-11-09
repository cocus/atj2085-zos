/* RECRUN: the recording part of REC (ZRAM2 module, chained from REC with the file prepared in
 * the hand-over block, chains back to REC to commit; ../rec/rechand.h). Loads ADENCM.DSP, runs
 * the microphone ADC + the DSP encoder (recio.c, protocol = the stock RECORD.AP driver) and
 * streams the encoded 512-byte sectors into the prepared cluster run through the NAND write path
 * (ftlw.inc with the busy-wait hook). Stop: PLAY / MENU / power key / run full -> the DSP is told
 * to stop at its next PCM request, sends its last blocks and then the WAV header sector (HDR4 =
 * 0x99), whose rate fields are patched (ADENCM's template says 8000 Hz) and written to sector 0.
 * The screen (drawn by REC) is updated in pieces of <= ~0.3 ms so the ADC FIFO keeps draining. */
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
#include "../common/vu.h"
#include "../rec/rechand.h"
#include "../rec/recui.h"
#include "rec.h"
#define ZS_PROVIDE
#include "strings.h"

extern const uint16_t rate_hz[NRATES];
uint8_t ri;
static uint32_t nsec;
void patch_header(uint8_t *h);
void mmss(char *t, uint16_t s);
/* every piece is cheap (the ADC FIFO holds 12 samples = 545 us at 22.05 kHz): the clocks count in
 * place as digits (no divisions), the lost counter takes one digit per piece */
static char t_el[6] = "00:00", t_fr[6];
static uint8_t piece;
static uint16_t free0, ui_t;
static uint32_t consumed;
#define N_PIECES (1 + 10)                    /* (the VU has its own path: vu_step) */
void tick(char *t, int8_t d);                       /* screen.c (_CODE2) */
static void ui_step(void)
{
    uint8_t i;
    uint16_t c;
    if (!piece) {
        if ((uint16_t)(hal_ticks() - ui_t) < 20) return;          /* 5 frames/s */
        ui_t = hal_ticks();
        if (nsamples - consumed >= rate_hz[ri]) {
            consumed += rate_hz[ri];
            rh.secs++;
            tick(t_el, 1);
            if (free0 < 6000 && (t_fr[0] != '0' || t_fr[1] != '0' || t_fr[3] != '0' || t_fr[4] != '0'))
                tick(t_fr, -1);                 /* (>= 100 min: shows 99:59 until below) */
        }
    } else if (piece < 6) gfx_char(REC_X_VAL + (piece - 1) * 6, REC_Y_TIME, t_el[piece - 1], 1, C_OK, C_BG);
    else if (piece < 11) gfx_char(REC_X_VAL + (piece - 6) * 6, REC_Y_FREE, t_fr[piece - 6], 1, C_FG, C_BG);
    if (++piece == N_PIECES) piece = 0;
}

/* VU: a new level every 50 ms (rises at once, falls 6 px per update); only the difference to
 * the bar on screen is drawn, at most 16 px x VU_H per call (as cheap as one old row piece),
 * in fixed colour zones, so the main loop keeps draining the 12-sample ADC FIFO */
static const uint8_t vu_z[3] = { VU_W * 2 / 3, VU_W * 26 / 30, VU_W };
static const uint16_t vu_c[3] = { VU_C_LOW, VU_C_MID, VU_C_HIGH };
static uint8_t vu_tgt, vu_cur;
static uint16_t vu_t;
void vu_step(void)
{
    uint8_t a, b, z;
    uint16_t c;
    if ((uint16_t)(hal_ticks() - vu_t) >= 5) {
        vu_t = hal_ticks();
        c = vcnt ? (vsum / vcnt) << 5 : 0;
        vsum = vcnt = 0;
        a = vu_px(c);
        vu_tgt = a + 6 < vu_tgt ? vu_tgt - 6 : a;
    }
    if (vu_cur < vu_tgt) {
        a = vu_cur;
        for (z = 0; a >= vu_z[z]; z++) ;
        b = vu_tgt;
        if (b > vu_z[z]) b = vu_z[z];
        if (b > a + 16) b = a + 16;
        gfx_fill(4 + a, REC_Y_VU, b - a, VU_H, vu_c[z]);
        vu_cur = b;
    } else if (vu_cur > vu_tgt) {
        a = vu_cur > vu_tgt + 16 ? vu_cur - 16 : vu_tgt;
        gfx_fill(4 + a, REC_Y_VU, vu_cur - a, VU_H, C_DIM);
        vu_cur = a;
    }
}

static uint8_t put(const uint8_t *buf, uint32_t sec)
{
    uint8_t r;
    __critical {                               /* interrupts off: ftlw_idle serves ADC + DSP */
        r = ftlw_write(rh.lba0 + sec, buf);
    }
    return r;
}

static void record(void)
{
    event_t e;
    uint8_t r, slot;
    uint16_t t_stop = 0;
    uint32_t v;
    for (v = (rh.room - 1) * 1024 - (rh.room - 1) * 7, free0 = 0; v >= rate_hz[ri] && free0 < 0xFFFF; v -= rate_hz[ri])
        free0++;                               /* (room - 1) * 1017 / rate, without the division library */
    mmss(t_fr, free0);
    gfx_fill(0, REC_Y_STATE, LCD_W, 10, C_BG);
    gfx_text(4, REC_Y_STATE, T(S_REC), 1, C_SEL, C_BG);
    gfx_fill(0, LCD_H - 10, LCD_W, 10, C_BG);
    gfx_text(4, LCD_H - 9, T(S_FT_REC), 1, C_DIM, C_BG);
    ui_t = hal_ticks();
    rec_start(ri);
    for (;;) {
        rec_service();
        while (ev_get(&e)) {
            if (e.type == EV_DSP) rec_dsp(e.code);
            else if ((e.type == EV_KEY_DOWN && (e.code == KEY_PLAY || e.code == KEY_MENU)) || e.type == EV_POWER) {
                if (e.type == EV_POWER) rh.flags |= RH_F_POWER;
                if (!stop_req) { stop_req = 1; t_stop = hal_ticks(); }
            }
        }
        if (q_count) {
            slot = q_tail;
            if (slot == hdr_idx) {             /* the final header: everything before it is written */
                rec_stop();
                patch_header(rb.q[slot]);
                if ((r = put(rb.q[slot], 0))) rh.err = r;
                else rh.flags |= RH_F_HEADER;
                return;
            }
            if ((r = put(rb.q[slot], nsec))) { rh.err = r; break; }
            nsec++;
            if (++q_tail == NQ) q_tail = 0;
            q_count--;
            if (nsec + NQ + 2 > rh.room && !stop_req) { rh.flags |= RH_F_FULL; stop_req = 1; t_stop = hal_ticks(); }
        }
        if (stop_req && (uint16_t)(hal_ticks() - t_stop) > 300) break;    /* no header in 3 s */
        rec_capture();                         /* again: the FIFO is 12 samples (375 us at 32 kHz) */
        ui_step();
        vu_step();
        hal_watchdog();
    }
    rec_stop();
}

/* the cold path (_CODE2) lives in ZRAM1; build.py stores it at offset 0x1000 of the .ZM */
extern const uint16_t code2_addr, code2_len;
#define CODE2_FILE_OFFSET 0x1000
static uint8_t code2_load(void)
{
    fat_file_t f;
    fat_dirent_t e;
    if (code2_addr + code2_len > REC_HAND_ADDR) return 0;      /* must not reach the hand-over block */
    fat_root(&f);
    if (!fat_find(&f, "RECRUN  ZM ", &e)) return 0;
    fat_open(&f, &e);
    return fat_seek(&f, CODE2_FILE_OFFSET) && fat_read(&f, (void *)code2_addr, code2_len) == code2_len;
}

uint16_t mod_main(void)
{
    if (rh.magic != REC_MAGIC || rh.state != RH_RECORD) return 0;
    ri = rh.rate < NRATES ? rh.rate : RATE_22K;
    nsec = rh.nsec;
    if (!code2_load()) rh.err = 0xFD;
    else {
        rec_dsp_reset(ri);
        if (!rec_load_dsp()) rh.err = 0xFE;
        else record();
        if (!rh.err && ftlw_flush()) rh.err = ftlw_error();
    }
    rh.nsec = nsec;
    rh.drops = drops;
    rh.state = RH_DONE;
    memcpy((void *)MOD_CHAIN_NAME, "REC     ZM ", 12);
    return MOD_CHAIN;
}
