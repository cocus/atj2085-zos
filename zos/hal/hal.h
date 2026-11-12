/* zos hardware abstraction layer - everything a target must provide.
 * Kernel, UI and apps only talk to the hardware through this header. */
#ifndef ZOS_HAL_H
#define ZOS_HAL_H
#include <stdint.h>

/* ---- lifecycle ---------------------------------------------------------------------------- */
void hal_init(void);                 /* take over the machine (hosted: from the stock OS) */
void hal_exit(void);                 /* give it back (hosted) / reset (bare metal) */

/* ---- time --------------------------------------------------------------------------------- */
#define HAL_TICK_HZ 100              /* hal_ticks() rate */
uint16_t hal_ticks(void);
void hal_idle(void);                 /* sleep until the next interrupt */
void hal_watchdog(void);             /* kick the watchdog (call from the main loop only) */
/* LCD backlight, applied by the next hal_watchdog(). Off works like the idle timeout: the next key
 * press only switches it on again and is swallowed (press and release). */
void hal_backlight(uint8_t on);


/* ---- input: hal posts key events into the kernel event queue (core/event.h) ------------- */
enum { KEY_NONE, KEY_MENU, KEY_LEFT, KEY_RIGHT, KEY_PLAY, KEY_VOL, KEY_COUNT };
#define KEY_EXTRA KEY_VOL            /* old name */

/* ---- display: 16-bit RGB565, (0,0) = top left -------------------------------------------- */
#define LCD_W 128
#define LCD_H 128
void hal_lcd_contrast(uint8_t v);                                 /* 0..CONTRAST_MAX (core/config.h) */
void hal_lcd_window(uint8_t x, uint8_t y, uint8_t w, uint8_t h);  /* then stream pixels */
void hal_lcd_fill(uint16_t color, uint16_t count);                /* stream `count` pixels */
void hal_lcd_pixel(uint16_t color);                               /* stream one pixel */
void hal_lcd_blit(const uint8_t *rgb565be, uint16_t count);       /* stream big-endian pixels */
void hal_lcd_colors(uint16_t fg, uint16_t bg);                    /* for hal_lcd_glyph6x8 */
void hal_lcd_glyph6x8(const uint8_t *cols5);                      /* 5x7 font cell + spacing */

/* ---- block device: the user disk as 512-byte sectors, LBA 0 = sector 0 of the FAT disk -------
 * (hosted: the stock NAND/FTL sector driver; later zos' own). Core-only: buf must be outside
 * the page window. */
uint8_t  hal_blk_init(void);                               /* 0 = no disk */
uint8_t  hal_blk_read(uint32_t lba, void *buf512);         /* 0 = failed */
/* firmware area (atj2085: FWIMAGE.FW in the NAND system area, sector 0 = header, then the
 * 32-byte directory {name[11], attr[5], u32 sector, u32 length, ...}); DSP programs live there */
uint8_t  hal_fw_read(uint16_t sector, void *buf512);
/* atj2085 NAND/FTL internals exported for the module-side write path (modules/common/ftlw.c) */
typedef struct { uint8_t c0, c1, r0, r1, r2; uint16_t buf, len; } nand_req_t;   /* column, row */
uint8_t  nand_read(const nand_req_t *r);                   /* raw page read, len 1..0x840 */
uint16_t ftl_tbl(uint8_t zone, uint16_t entry);            /* mapping table entry, 0xFFFF = none */

/* ---- interrupt sources (atj2085: port 0x27 bits; 0x01 = DSP -> EV_DSP, self-masking) ------ */
void hal_irq_mask(uint8_t set, uint8_t clr);


/* ---- pages: 16 KB RAM pages mapped at PAGE_BASE (modules run in place from them) --------- */
#define PAGE_BASE 0x4000
#define PAGE_SIZE 0x4000
#define HAL_ZRAM2_PAGE 0x07          /* atj2085: 4 KB ZRAM2 at 0x4000 (stock UI driver page), port 05
                                        bits 7-4 = 0: every DSP RAM half stays with the DSP */
#define HAL_ZRAM2_SIZE 0x1000
#define HAL_MODRAM_BASE 0x3400       /* ZRAM1 scratch for page modules (ZRAM2 modules: build.py layout) */
#define HAL_MODRAM_END  0x3E00       /* (takeover: stock BREC/kernel RAM up here is not used by zos;
                                        blk.c reads the FTL descriptors at 0x3C80 once, at boot) */
#define HAL_SHARED      0x3E00       /* 512 B kept across modules (chain name + state), cleared at boot */
#define HAL_BATT (*(volatile uint8_t *)(HAL_SHARED + 511))   /* battery state for the icon, written by
                                        the target's 2 Hz interrupt (debounced; EV_BATT on a change):
                                        0 = not sampled yet (read the ports), else HAL_BATT_* */
#define HAL_BATT_VALID  0x80
#define HAL_BATT_VBUS   0x10         /* USB power present (the level then reads the charger) */
#define HAL_BATT_LEVEL  0x0F         /* ADC level (targets/<t>/board.h: icon calibration) */
/* Wall clock: 17 bytes at HAL_SHARED+444 (survive module chaining, cleared at boot). The core's 2 Hz
 * interrupt ticks the first 8 (targets/<t>/clock.s: plain software ticks, the core has no room for more);
 * modules read them directly (modules/common/skintime.h, rtc.h) - no API entries, so modules keep working
 * with an OLDER core: it never writes `core`, so modules then show no time and nothing else changes.
 *   src   0 = no clock (the core does not tick), else where the time came from (HAL_CLK_*)
 *   half  0/1: the half-second phase; sec/min/hour = time of day; day = days since 2000-01-01
 *   core  HAL_CLK_CORE (written by home()): this core has the clock
 * EV_TIME (core/event.h) is posted whenever the minute changes. To set the time a module writes src = 0
 * first, then the fields, src last (rtc.c clk_set).
 * Module side (rtc.c): the MCU RTC counter (24 bit, 2 Hz, ports 45h-47h) is the running time base:
 *   cnt   the counter and base* the time of day it belongs to when the clock was set / last re-based:
 *         now = base + (counter - cnt) / 2 s. SHELL compares it with the software time (clk_sync) on every
 *         start and each minute and after standby, and corrects the clock: a stopped tick (standby, long
 *         interrupt-off windows) costs nothing. A counter that went backwards / was cleared: the clock
 *         re-bases on the software time and err counts it (SYSINFO)
 * The block lies in the free tail of the MUSIC block (music/shared.h asserts it stays below). */
typedef struct {
    uint8_t src, half, sec, min, hour; uint16_t day; uint8_t core;
    uint8_t cnt0, cnt1, cnt2, err;
    uint16_t bday; uint8_t bhour, bmin, bsec;
} hal_clock_t;
#define HAL_CLOCK_ADDR (HAL_SHARED + 444)
#define HAL_CLK ((volatile hal_clock_t *)HAL_CLOCK_ADDR)
#define HAL_CLK_CORE    0xC7
enum { HAL_CLK_NONE, HAL_CLK_BUILD, HAL_CLK_ANCHOR, HAL_CLK_RTC };   /* src: build date / ZOS.CFG anchor / DS3231 */
extern const uint8_t hal_pages[];    /* page ids usable for modules */
extern const uint8_t hal_npages;
typedef struct { uint16_t dst; const void *src; uint16_t len; } xfer_t;
uint8_t  page_get(void);
void     page_set(uint8_t page);
uint16_t mod_call(uint8_t page, uint16_t fn);              /* run fn() with `page` mapped */
void     page_write(uint8_t page, const xfer_t *x);        /* src outside the window */
void     page_read(uint8_t page, const xfer_t *x);         /* dst outside the window */

#endif
