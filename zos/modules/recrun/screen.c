/* RECRUN time format + WAV header patch (_CODE2, ZRAM1: ZRAM2 is full). Called from the main
 * loop with port 05 = ZRAM2 (never from rec_dma's window switch). */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/vu.h"
#include "../rec/rechand.h"
#include "../rec/recui.h"
#include "rec.h"
#include "regs.h"

SFR(G_AICTL, P_AICTL);

extern uint8_t ri;
const uint16_t rate_hz[NRATES] = { 16000, 22050, 32000 };
static const uint16_t byte_rate[NRATES] = { 8055, 11101, 16110 };   /* rate * 512 / 1017, rounded */

/* ---- screen pieces ------------------------------------------------------------------------- */
void mmss(char *t, uint16_t s)
{
    uint8_t m;
    if (s >= 6000) s = 5999;                   /* 99:59 */
    m = s / 60;
    t[0] = '0' + m / 10; t[1] = '0' + m % 10; t[2] = ':';
    s %= 60; t[3] = '0' + s / 10; t[4] = '0' + s % 10;
}
/* VOL during recording: next microphone gain, applied at once (REC keeps it for next time) */
void rec_gain_step(void)
{
    rh.gain = rh.gain < REC_GAIN_MAX ? rh.gain + 1 : 0;
    rh.gain_chk = (uint8_t)~rh.gain;
    G_AICTL = (G_AICTL & ~AICTL_MICGAIN) | rh.gain;
    gfx_char(REC_X_VAL, REC_Y_GAIN, '0' + rh.gain, 1, C_FG, C_BG);
}

/* mm:ss +1 / -1 second in place (digits only, no division) */
void tick(char *t, int8_t d)                  /* mm:ss +1 / -1 second in place */
{
    static const char top[5] = { '9', '9', 0, '5', '9' };
    uint8_t i = 4;
    for (;;) {
        if (d > 0) { if (t[i] != top[i]) { t[i]++; return; } t[i] = '0'; }
        else { if (t[i] != '0') { t[i]--; return; } t[i] = top[i]; }
        if (!i) return;
        if (--i == 2) i--;
    }
}

void patch_header(uint8_t *h)
{
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) || h[20] != 0x11) return;   /* IMA ADPCM */
    *(uint32_t *)(h + 24) = rate_hz[ri];
    *(uint32_t *)(h + 28) = byte_rate[ri];
}

