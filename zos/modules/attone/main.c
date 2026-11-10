/* ATTONE: AUDIOTST's tone generator (test module, chained from the AUDIOTST launcher which drew the
 * title; MENU chains back). Sine / square tones at 8 kHz through the Z80 -> DAC FIFO path, << >> volume,
 * VOL +3 steps, PLAY next waveform; FIFO depth / measured words per second / writes per frame. */
#include "../common/audiotst/atbase.c"
#include "../common/skinlite.c"
#include "../../core/module.h"
#include <string.h>
#include "sintab.h"

/* ---- TONE screen ------------------------------------------------------------------------------ */
static const char *const tone_name[6] = { "SINE 440 HZ", "SINE 1 KHZ", "SQUARE 440 HZ", "SQUARE 1 KHZ", "SINE 250 HZ", "SINE 2 KHZ" };
/* phase increment per 8 kHz frame: f * 65536 / 8000 */
static const uint16_t tone_inc[6] = { 3605, 8192, 3605, 8192, 2048, 16384 };

static void tone_info(void)
{
    clr(70, 36);
    txt(4, 72, "FIFO DEPTH", C_DIM);   num(76, 72, fifo_depth, C_FG);
    txt(4, 82, "WORDS/S", C_DIM);      num(76, 82, wps, C_FG);
    txt(4, 92, "WRITES/FRAME", C_DIM); num(86, 92, wpf, wpf_mode ? C_SEL : C_OK);
    if (dac_dead) txt(4, 102, "DAC STALLED", RGB(255, 60, 40));
}

static void tone_screen(void)
{
    uint16_t ph = 0, inc;
    uint8_t shape = 0, k = 0, key, redraw = 1;
    int16_t s;

    clr(SKIN_TITLE_H, LCD_H - SKIN_TITLE_H);            /* (the launcher drew the title) */
    skin_footer("<< >> VOL  PLAY:WAVE");
    dac_on(0);
    for (;;) {
        if (redraw) {
            clr(16, 12);
            txt(4, 18, tone_name[shape], C_FG);
            draw_vol(34);
            tone_info();
            inc = tone_inc[shape];
            redraw = 0;
        }
        s = (shape == 2 || shape == 3) ? ((ph & 0x8000) ? -9000 : 9000) : ((int16_t)sintab[ph >> 8] << 7);
        ph += inc;
        dac_put(s);
        if (wpf == 2) dac_put(s);
        if (++k & 127) continue;
        hal_watchdog();
        key = getkey();
        if (!key) continue;
        if (key == KEY_MENU) break;
        if (key == KEY_LEFT && vol) vol--;
        else if (key == KEY_RIGHT && vol < 31) vol++;
        else if (key == KEY_VOL) vol = vol < 28 ? vol + 3 : 0;
        else if (key == KEY_PLAY && !key_rep) { shape = shape < 5 ? shape + 1 : 0; ph = 0; }
        else continue;
        vol_apply();
        redraw = 1;
    }
    dac_off();
    regs_restore();
}


uint16_t mod_main(void)
{
    if (ATS->magic == AT_MAGIC) tone_screen();
    ATS->back = 1;
    memcpy(ATS->next, AT_ZM, 12);
    return MOD_CHAIN;
}
