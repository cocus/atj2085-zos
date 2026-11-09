/* USB Audio Class 1: capture from the built-in microphone or FM radio, or a USB speaker
 * (playback to the DAC / headphones, 48 or 44.1 kHz stereo). */
#include <stdint.h>
#include <string.h>
#include "../usb/usb.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "../common/clock.h"

static uint8_t source, stop, ab;
static uint16_t tdraw, tsec, kws, loops, pk0;
static uint8_t dac;
extern uint8_t dac_mono;
extern uint16_t dac_cal48, dac_cal441;
#define NSRC 4
static const char *const sources[NSRC] = { "MICROPHONE", "FM RADIO", "SPEAKER (DAC)", "USB TEST" };
static const uint8_t src_fn[NSRC] = { FN_AUDIO_MIC, FN_AUDIO_FM, FN_AUDIO_DAC, FN_AUDIO_MIC };
extern uint8_t audio_test;
extern uint16_t audio_f10;
void audio_tune(int8_t step);
uint16_t audio_level(void);

/* FM while streaming: the station (<< >> tune) and a VU bar (redrawn only where it changed: the
 * ADC FIFO lasts 375 us at 32 kHz) */
#define VU_X 4
#define VU_Y 112
#define VU_W 120
static uint8_t vu_len;
static uint16_t tvu;
static uint8_t vu_px(uint16_t v)          /* 1.5 dB steps, 0 dBFS = VU_W, 36 dB range (common/vu.c) */
{
    uint8_t b = 0, l;
    uint16_t q = v;
    while (q) { b++; q >>= 1; }
    if (b < 3) return 0;
    l = b * 4 + ((v >> (b - 3)) & 3);
    if (l <= 37) return 0;
    l = (l - 37) * 5;
    return l > VU_W ? VU_W : l;
}
static void vu_draw(void)
{
    uint8_t n = vu_px(audio_level());
    if (n + 4 < vu_len) n = vu_len - 4;                /* falls slowly */
    if (n > vu_len) gfx_fill(VU_X + vu_len, VU_Y, n - vu_len, 5, n > 104 ? RGB(255, 60, 40) : n > 80 ? RGB(255, 200, 40) : C_OK);
    else if (n < vu_len) gfx_fill(VU_X + n, VU_Y, vu_len - n, 5, C_BAR);
    vu_len = n;
}
static void line(uint8_t y, const char *label, const char *text, uint16_t color);
static void fm_line(void)
{
    char t[10];
    uint16_t f = audio_f10;
    t[0] = f >= 1000 ? '1' : ' ';
    t[1] = '0' + (f / 100) % 10; t[2] = '0' + (f / 10) % 10; t[3] = '.'; t[4] = '0' + f % 10;
    t[5] = ' '; t[6] = 'M'; t[7] = 'H'; t[8] = 'Z'; t[9] = 0;
    line(100, "FM", t, C_SEL);
}
void dac_power(uint8_t on);                     /* audio.c */
extern uint16_t audio_dbg;
extern uint8_t audio_peak;
static const char *const states[] = { "OFF", "NO CABLE", "CHARGER?", "ATTACHED",
                                      "BUS RESET", "ADDRESSED", "CONFIGURED" };

static void line(uint8_t y, const char *label, const char *text, uint16_t color)
{
    gfx_fill(0, y, LCD_W, 10, C_BG);
    gfx_text(4, y + 1, label, 1, C_DIM, C_BG);
    gfx_text(46, y + 1, text, 1, color, C_BG);
}

static void number(uint8_t y, const char *label, uint16_t a, const char *label2, uint16_t b)
{
    gfx_fill(0, y, LCD_W, 10, C_BG);
    gfx_text(4, y + 1, label, 1, C_DIM, C_BG);
    gfx_num(46, y + 1, a, 1, C_FG, C_BG);
    if (label2) {
        gfx_text(82, y + 1, label2, 1, C_DIM, C_BG);
        gfx_num(102, y + 1, b, 1, C_FG, C_BG);
    }
}

static void menu(uint8_t running)
{
    uint8_t i, s;
    gfx_fill(0, 16, LCD_W, 48, C_BG);
    for (i = 0; i < NSRC; i++) {
        s = i == source;
        gfx_fill(0, 16 + i * 12, LCD_W, 11, s ? C_SELBG : C_BG);
        gfx_text(6, 18 + i * 12, sources[i], 1, s ? C_SELFG : C_FG, s ? C_SELBG : C_BG);
    }
    if (running) {
        line(64, "USB", states[usb_state], usb_state == US_CONFIGURED ? C_OK : C_FG);
        number(76, "PACKETS", audio_packets, "DROP", audio_dropped);
        number(88, "UNDER", audio_under, clk_fast ? "48M" : "24M", 0);
    }
    skin_footer(running ? "PLAY/MENU: OFF" : "<< >> SOURCE PLAY:GO");
}

static void run(void)
{
    event_t e;
    usb_nsetup = usb_nreset = 0;
    audio_packets = audio_dropped = 0;
    stop = 0;
    menu(1);
    if (src_fn[source] == FN_AUDIO_DAC) {
        dac_power(1);                           /* amplifier up before the host starts */

    }
    audio_test = source == 3;
    if (src_fn[source] == FN_AUDIO_FM) {
        fm_line();
        gfx_fill(VU_X, VU_Y, VU_W, 5, C_BAR);
        vu_len = 0;
    }
    usb_start(src_fn[source]);
    cpu_fast();                                 /* streaming: 48 MHz (no NAND / DSP in use); VOL: 24 */
    tdraw = hal_ticks();
    ab = setjmp(usb_jb);
    if (ab == AB_STOP) stop = 1;
    else if (ab) usb_recover(ab);
    dac = src_fn[source] == FN_AUDIO_DAC;
    while (!stop) {
        loops++;
        if (dac) {                              /* speaker: the DAC FIFO (8 words) lasts 83 us at 48 kHz
                                                   stereo: pump it ~64 times per housekeeping pass */
            uint8_t i;
            for (i = 0; i < 64; i++) usb_audio_poll();
        }
        usb_service();
        usb_audio_poll();
        if (usb_dirty && (uint16_t)(hal_ticks() - tdraw) >= 25) {
            usb_dirty = 0;
            menu(1);
            tdraw = hal_ticks();
        }
        while (ev_get(&e)) {
            if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
            if (src_fn[source] == FN_AUDIO_DAC && e.type == EV_KEY_DOWN && (e.code == KEY_LEFT || e.code == KEY_RIGHT)) {
                dac_mono ^= 1;
                continue;
            }
            if (src_fn[source] == FN_AUDIO_FM && (e.code == KEY_LEFT || e.code == KEY_RIGHT)) {
                audio_tune(e.code == KEY_RIGHT ? 1 : -1);      /* tune while streaming */
                fm_line();
                continue;
            }
            if (e.type != EV_KEY_DOWN) continue;
            if (e.code == KEY_MENU || e.code == KEY_PLAY) stop = 1;
            else if (e.code == KEY_VOL) { if (clk_fast) cpu_normal(); else cpu_fast(); usb_dirty = 1; }
        }
        if (src_fn[source] == FN_AUDIO_FM && (uint16_t)(hal_ticks() - tvu) >= 4) { tvu = hal_ticks(); vu_draw(); }
        if ((uint16_t)(hal_ticks() - tsec) >= 500) {   /* every 5 s (each redraw starves the DAC) */
            tsec = hal_ticks();
            kws = (uint16_t)(audio_words / 500);         /* hundreds of words per second */
            audio_words = 0;
            if (src_fn[source] == FN_AUDIO_DAC) {         /* diagnostics: loop rate, packets/s */
                number(88, "CAL 48K", dac_cal48, "44K", dac_cal441);   /* DAC words/s / 10 */
                number(100, dac_mono ? "MONO" : "W/S00", kws, "PK/S", (audio_packets - pk0) / 5);
                pk0 = audio_packets;
            }
            loops = 0;
        }
    }
    usb_stop();
    cpu_normal();
    dac_power(0);
    menu(0);
}

uint16_t mod_main(void)
{
    event_t e;
    source = 0;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("USB AUDIO");
    menu(0);
    for (;;) {
        ev_wait(&e, 0);
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) break;
        if (e.code == KEY_LEFT || e.code == KEY_RIGHT) {
            source = e.code == KEY_RIGHT ? (source + 1) % NSRC : (source + NSRC - 1) % NSRC;
            menu(0);
        } else if (e.code == KEY_PLAY) {
            run();
            menu(0);
        }
    }
    usb_stop_clock(0);
    return 0;
}
