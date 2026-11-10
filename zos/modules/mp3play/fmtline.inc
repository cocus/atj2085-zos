/* MP3PLAY/DECPLAY format line, included by main.c (ZRAM2) or ui.c (_CODE2, FMT_IN_UI): wherever the
 * player has room */
#include "regs.h"                              /* DACRATE_441 */
/* sample rate in kHz, BCD, by DAC rate code (low nibble; mp3_driver_spec 3.2), 0 = unknown code or 44.1 */
static const uint8_t khz[16] = { 0x48, 0, 0x24, 0x12, 0x32, 0, 0x16, 0x08, 0, 0, 0x22, 0x11 };   /* 44.1: own text */
void fmt_line(void)                            /* "  48K 128K" / "44.1K 128K" */
{
    char t[11];
    uint8_t c = kb[3] & 0x0F, k = khz[c];
    memcpy(t, c == DACRATE_441 ? "44.1K 000K" : "  00K 000K", 11);
    t[2] += k >> 4;
    t[3] += k & 0x0F;
    memcpy(t + 6, kb, 3);                      /* (0 = not known yet: rate only) */
    txt(54, 40, t, C_DIM);
}
