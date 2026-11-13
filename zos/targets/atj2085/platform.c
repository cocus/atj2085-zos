/* zos target: Actions ATJ2085 (Z80 + DSP), "Ximpro"/LISONG LSD_501 board, hosted mode.
 * Port meanings: lisong_rebuild/analysis/atj2085_hw.md (ATJ2085X programming guide). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/config.h"

#include "regs.h"
#include "board.h"

#define LCD (*(volatile uint8_t *)LCD_DATA)

void zos_hook_irq(uint8_t enable_mask);
void zos_unhook_irq(void);
void lcd_cmd(uint8_t c);

static uint8_t saved_clk, saved_emhi, saved_page;
static volatile uint16_t ticks;

/* ------------------------------------------------------------------------------- display */
/* zos scan mode: DATCTL p1 = 0x03 (row + column inverse, x first). With the panel mounted 180
 * degrees, screen (x,y) = controller column x, row y + 4, streamed left-right / top-down. */

static void lcd_datctl(uint8_t p1)
{
    lcd_cmd(LCD_DATCTL);
    LCD = p1; LCD = 0x01; LCD = 0x00;
}

void hal_lcd_contrast(uint8_t v)
{
    if (v > CONTRAST_MAX) v = CONTRAST_MAX;
    lcd_cmd(LCD_VOLCTR);
    LCD = CONTRAST_BASE + v; LCD = CONTRAST_P2;
}

void hal_lcd_window(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    lcd_cmd(LCD_CASET); LCD = x;           LCD = x + w - 1;
    lcd_cmd(LCD_PASET); LCD = y + LCD_ROW0; LCD = y + LCD_ROW0 + h - 1;
    lcd_cmd(LCD_RAMWR);
}

/* ------------------------------------------------------------------------------- keys */
/* Board wiring, from the stock KY_HARD.DRV scan routine (device-confirmed key codes):
 *   phase 1: drive B1,B2,B5,B6 as inputs (OE &= 0x99, IE |= 0x66), read D & 0x66:
 *            B1 = LEFT, B2 = MENU, B5 = (unused), B6 = RIGHT
 *   phase 2 (only if nothing pressed): drive B6 high (OE |= 0x40, D |= 0x40), read D & 0x02:
 *            B1 = PLAY */
#define REPEAT_DELAY 25    /* scans (20 ms each) before auto-repeat */
#define REPEAT_RATE  5

static uint8_t key_down, key_cnt;

static uint8_t keys_read(void)
{
    uint8_t c, i;
    GPB_OE &= ~KEYB_ROWS;
    GPB_IE |= KEYB_ROWS;
    for (i = 0; i < 32; i++) ;                  /* settle, as the stock driver does */
    c = GPB_D & KEYB_ROWS;
    if (c) {
        if (c & KEYB_MENU) return KEY_MENU;
        if (c & KEYB_LEFT) return KEY_LEFT;
        if (c & KEYB_RIGHT) return KEY_RIGHT;
        if (c & KEYB_VOL) return KEY_VOL;      /* stock code 0x0D (KY_HARD table 0x070F) */
        return KEY_NONE;
    }
    GPB_OE |= KEYB_DRIVE;
    GPB_D |= KEYB_DRIVE;
    return (GPB_D & KEYB_PLAY) ? KEY_PLAY : KEY_NONE;
}

/* backlight timeout (ZCFG->backlight): counted in half seconds by the RTC interrupt; a key press
 * while it is dark only switches the light back on (that press and its release are swallowed).
 * The interrupt only sets bl_off; the port (shared with pin mode / CE0S bits) is written by
 * hal_watchdog() in the main loop. The port is read ONCE (hal_init) into a shadow and never read
 * back afterwards: with read-modify-write, the screen stopped updating after the light went off
 * (suspected: port EE does not read back what was written, so the pin-mode bits 7-5 that give
 * us GPIO C = LCD A0 got changed). */
static const uint8_t bl_halfsecs[NBL] = { 0, 20, 60, 120 };
static volatile uint8_t bl_off;
static uint8_t bl_idle, bl_eat, bl_port_off, mfp_shadow;

static void backlight_apply(void)
{
    uint8_t off = bl_off;
    if (off == bl_port_off) return;
    bl_port_off = off;
    MFP = off ? mfp_shadow & ~MFP_BACKLIGHT : mfp_shadow | MFP_BACKLIGHT;
}

/* power-off: PLAY held POWER_HOLD scans posts EV_POWER once and swallows the rest of that press
 * (no EV_KEY_UP), so modules keep short PLAY presses. SHELL does the rest (modules/shell/standby.s):
 * it switches the light off (hal_backlight) with interrupts off, so the press that wakes the
 * player is swallowed here like any light-waking press. */
#define POWER_HOLD 100         /* scans: 2 s */
static uint8_t play_hold;

static void keys_scan(void)
{
    uint8_t k = keys_read();
    if (k) {
        bl_idle = 0;
        if (bl_off) { bl_off = 0; bl_eat = 1; }
    }
    if (bl_eat) {                               /* swallow the waking press until released */
        if (!k) bl_eat = 0;
        key_down = k;
        return;
    }
    if (k != KEY_PLAY) play_hold = 0;
    else if (++play_hold == POWER_HOLD) { ev_post(EV_POWER, 0); bl_eat = 1; }
    if (k != key_down) {
        if (key_down) ev_post(EV_KEY_UP, key_down);
        if (k) ev_post(EV_KEY_DOWN, k);
        key_down = k;
        key_cnt = 0;
    } else if (k && ++key_cnt >= REPEAT_DELAY) {
        ev_post(EV_KEY_REPEAT, k);
        key_cnt = REPEAT_DELAY - REPEAT_RATE;
    }
}

/* ------------------------------------------------------------------------------- battery */
/* 2 Hz: battery level + USB power into HAL_BATT, debounced (board.h); EV_BATT when it changes.
 * bcand: the candidate state's level + VBUS (bits 4-0) and how many samples in a row it was seen
 * (bits 7-5) */
static uint8_t bcand;

static void batt_sample(void)
{
    uint8_t s = (BATT & BATT_LEVEL) | (USB_STAT & USBST_VBUS ? HAL_BATT_VBUS : 0), old = HAL_BATT, d, n;
    if (s == (old & 0x1F) && (old & HAL_BATT_VALID)) { bcand = 0; return; }
    if ((bcand & 0x1F) != s) bcand = s;                      /* new candidate, seen once */
    else if (bcand < 0xE0) bcand += 0x20;
    n = bcand >> 5;                                          /* repeats after the first sample */
    d = (s & BATT_LEVEL) - (old & BATT_LEVEL);
    if ((int8_t)d < 0) d = -d;
    if (!(old & HAL_BATT_VALID) ||
        (n >= 1 && (((s ^ old) & HAL_BATT_VBUS) || d >= BATT_HYST_STEPS)) || n >= BATT_SLOW_SAMPLES) {
        HAL_BATT = s | HAL_BATT_VALID;
        bcand = 0;
        ev_post(EV_BATT, 0);
    }
}

/* ------------------------------------------------------------------------------- wall clock */
/* hal/hal.h HAL_CLK, advanced by the MCU RTC counter's delta from the 2 Hz interrupt (clock.s): a late
 * interrupt does not lose time; a jump > 255 (standby, counter cleared) ticks once, re-bases and counts
 * in err (SHELL adds the standby gap); a counter that does not move ticks like the interrupt. */
void clk_irq(void);
typedef char ev_time_is_9[EV_TIME == 9 ? 1 : -1];        /* clock.s */

/* ------------------------------------------------------------------------------- time */
/* called from the interrupt handler with the RTC status (port 0x44, already acknowledged) */
void zos_rtc_irq(uint8_t st)
{
    if (st & RTC_TICK) {                 /* 10 ms timer */
        ticks++;
        if (ticks & 1) keys_scan();  /* every 20 ms */
    }
    if (st & RTC_HALFSEC) {                 /* 2 Hz */
        ev_post(EV_SECOND_HALF, 0);
        batt_sample();
        clk_irq();
        if (ZCFG->magic == ZCFG_MAGIC && ZCFG->backlight && ZCFG->backlight < NBL && !bl_off &&
            ++bl_idle >= bl_halfsecs[ZCFG->backlight])
            bl_off = 1;
    }
}

uint16_t hal_ticks(void)
{
    uint16_t a, b;
    do { a = ticks; b = ticks; } while (a != b);
    return a;
}

void hal_watchdog(void) { WDT |= WDT_CLR; backlight_apply(); }
void hal_backlight(uint8_t on) { bl_off = !on; }

/* ------------------------------------------------------------------------------- lifecycle */
void hal_init(void)
{
    saved_clk = CLKCTL;
    mfp_shadow = MFP | MFP_BACKLIGHT;     /* as the stock OS left it (light on) */
    saved_emhi = EMHI;
    saved_page = PAGE;
    EMHI = EMHI_CE3_LCD;                  /* external window -> CE3 (LCD) for the whole session */
    CLKCTL = saved_clk | CLK_WS3;       /* 3 wait states for the LCD bus (stock driver does this) */
    lcd_datctl(DATCTL_ZOS);
    key_down = keys_read();          /* a key still held from the launcher is not a new press */
    zos_hook_irq(IRQ_RTC);              /* own the IRQ line, RTC only */
}

/* takeover mode: the stock OS' memory may be overwritten, so there is no way back except a reset
 * (start.s: watchdog). Just stop interrupts and blank the screen so the reboot looks clean. */
void hal_exit(void)
{
    __asm__("di");
    bl_off = 0;
    backlight_apply();
    hal_lcd_window(0, 0, LCD_W, LCD_H);
    hal_lcd_fill(0, LCD_W * LCD_H);
}
