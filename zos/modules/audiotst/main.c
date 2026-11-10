/* AUDIOTST: launcher of the audio converter tests (page module, full skin): DAC test tones, ADC record
 * (microphone / FM radio), playback, live level meter. << >> choose, PLAY starts, MENU leaves.
 *   TONE GENERATOR  -> ATTONE.ZM   MIC / FM: METER+REC -> ATREC.ZM
 *   DAC WORDS/FRAME (setting: auto / 1 / 2 FIFO writes per frame), PORT DUMP: here
 * The launcher saves the stock converter registers once, draws the chrome (title) and chains to a test
 * module, which draws below the title, restores the registers and chains back (common/audiotst/at.h).
 *
 * Mechanism (evidence in lisong_rebuild/analysis/audio_hw.md): the Z80 talks to the converters directly,
 * no DSP involved:
 *   DAC : 20-bit PCM written to ports 0x88/0x89/0x8A (the 0x8A write commits the word into the 8-deep
 *         playback FIFO), FIFO-full flag = port 0x8B bit 5. Rate 0x81, control 0x80, analog 0xDD/0x9E.
 *   ADC : port 0x9A (R hi, R lo [, L hi, L lo]), FIFO not-empty = port 0xD7 bit 5. Source/gain/rate in
 *         0xD3..0xD6, 0x9F. We poll, no IRQ (zos' 0x27 mask stays 0x20).
 * The register sequences are copies of the stock OS routines ADC_Enable/DAC_Enable/PA_Enable/AI_Enable
 * (SYSBNKA.SYS).  Everything we change is saved on entry and restored on exit.
 *
 * Keys: MENU back, LEFT/RIGHT/PLAY/VOL as printed in the footer of each screen. */
#include <string.h>
#include "../common/audiotst/atbase.c"
#include "../../core/fat.h"
#include "../../core/module.h"
#include "strings.h"

static void regs_save(void)
{
    sv[0] = P_CLK;  sv[1] = DACCTL; sv[2] = DACSR;  sv[3] = FIFOST; sv[4] = VOLR;   sv[5] = ADCMOD;
    sv[6] = PD2;    sv[7] = ADCTUNE; sv[8] = ADC1;  sv[9] = ADC2;   sv[10] = AING; sv[11] = ADCFIFO;
    sv[12] = PD9;   sv[13] = ANA;   sv[14] = ADCBUS;
}

static void title(uint8_t id)
{
    gfx_fill(0, 0, LCD_W, SKIN_TITLE_H, C_BG);       /* (no TITLE.ZI: a 12-row bar) */
    skin_title(T(id));
}

/* ---- PORT DUMP: stock state at module entry vs now ---------------------------------------------- */
static void regs_screen(void)
{
    static const char *const nm[15] = { "42", "80", "81", "8B", "9E", "9F", "D2", "D3", "D4", "D5", "D6", "D7", "D9", "DD", "9B" };
    uint8_t i, x, y, now[15], key;
    title(S_T_PORTS);
    clr(SKIN_TITLE_H, LCD_H - SKIN_TITLE_H);
    txt(4, 106, T(S_ENTRY), C_DIM);
    skin_footer(T(S_FT_PORTS));
    do {
        now[0] = P_CLK; now[1] = DACCTL; now[2] = DACSR; now[3] = FIFOST; now[4] = VOLR; now[5] = ADCMOD;
        now[6] = PD2; now[7] = ADCTUNE; now[8] = ADC1; now[9] = ADC2; now[10] = AING; now[11] = ADCFIFO;
        now[12] = PD9; now[13] = ANA; now[14] = ADCBUS;
        for (i = 0; i < 15; i++) {
            x = i < 8 ? 4 : 66;
            y = 17 + (i & 7) * 11;
            gfx_fill(x, y, 60, 9, C_BG);
            txt(x, y, nm[i], C_DIM);
            hex2(x + 16, y, sv[i], C_FG);
            hex2(x + 38, y, now[i], now[i] == sv[i] ? C_DIM : C_SEL);
        }
        do { key = getkey(); hal_watchdog(); hal_idle(); } while (!key);
        if (key == KEY_MENU) break;
    } while (key == KEY_PLAY);
}

/* ---- main menu -------------------------------------------------------------------------------- */
#define NITEM 5
static const uint8_t item[NITEM] = { S_TONE, S_MIC, S_FM, S_WPF, S_PORTS };
static const char *const wpf_txt[3] = { "AUTO", "1", "2" };
#define ROW_Y(i) (18 + (i) * 13)

static void row(uint8_t i, uint8_t s)
{
    uint8_t y = ROW_Y(i);
    uint16_t fg = s ? sk.sel_text : sk.fg;
    gfx_fill(0, y, LCD_W, 13, sk.bg);
    if (s && !skin_draw("SELBAR  ZI ", 0, y + 1, 0)) gfx_fill(0, y + 1, LCD_W, 11, sk.sel_bot);
    if (s) skin_text(6, y + 3, T(item[i]), fg);
    else gfx_text(6, y + 3, T(item[i]), 1, fg, sk.bg);
    if (i == 3) skin_text(100, y + 3, wpf_txt[wpf_mode], C_OK);
}

static void menu(void)
{
    uint8_t i;
    title(S_TITLE);
    clr(SKIN_TITLE_H, LCD_H - SKIN_TITLE_H);
    for (i = 0; i < NITEM; i++) row(i, i == ATS->sel);
    txt(4, 96, T(S_PAGE), C_DIM);
    num(46, 96, page_get(), C_FG);
    txt(4, 106, T(S_LOW), C_DIM);
    skin_footer(T(S_FT_MENU));
}

/* title of the test, then chain to its module; 0 = the module file is missing (message shown) */
static uint16_t go(const char *name11, uint8_t t)
{
    fat_file_t d;
    fat_dirent_t e;
    fat_root(&d);
    if (!fat_find(&d, name11, &e)) {
        clr(94, 22);
        txt(4, 96, T(S_NOMOD), C_SEL);
        wait_ticks(200);
        return 0;
    }
    title(t);
    clr(SKIN_TITLE_H, LCD_H - SKIN_TITLE_H);
    memcpy(ATS->next, name11, 12);
    return MOD_CHAIN;
}

uint16_t mod_main(void)
{
    uint8_t key, s;
    uint16_t r;
    if (ATS->magic != AT_MAGIC || !ATS->back) {      /* from the shell: stock state, defaults */
        memset((uint8_t *)ATS + 12, 0, sizeof *ATS - 12);
        ATS->magic = AT_MAGIC;
        regs_save();
        vol = 14; wpf = 2; mic_gain = 5; f10 = 987;
    }
    ATS->back = 0;
    skin_load();
    for (;;) {
        menu();
        for (;;) {
            do { key = getkey(); hal_watchdog(); hal_idle(); } while (!key);
            if (key == KEY_MENU) { regs_restore(); ATS->magic = 0; return 0; }
            s = ATS->sel;
            if (key == KEY_LEFT) ATS->sel = s ? s - 1 : NITEM - 1;
            else if (key == KEY_RIGHT) ATS->sel = s < NITEM - 1 ? s + 1 : 0;
            else if (key == KEY_PLAY && !key_rep) break;
            row(s, 0); row(ATS->sel, 1);
        }
        s = ATS->sel;
        if (s == 0) r = go("ATTONE  ZM ", S_T_TONE);
        else if (s <= 2) r = go("ATREC   ZM ", s == 1 ? S_T_MIC : S_T_FM);
        else if (s == 3) { wpf_mode = wpf_mode < 2 ? wpf_mode + 1 : 0; measured = 0; if (wpf_mode) wpf = wpf_mode; r = 0; }
        else { regs_screen(); r = 0; }
        if (r) return r;
    }
}
