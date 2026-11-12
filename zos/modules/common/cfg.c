/* ZOS.CFG: the settings file in the disk root, loaded by SHELL at boot and saved in place by
 * SETTINGS / MUSIC / SHELL (include this file from one module source, plus a .s file with
 * .include "../common/ftlw.inc"). The build creates the file (ZCFG_FILE_SIZE bytes of text, padded
 * with spaces) when the disk has none, so saving never allocates clusters or changes directory
 * entries: it rewrites the file's first sector through the NAND write path.
 * Text, one KEY=VALUE per line, also editable on the PC:
 *   LANG=EN|ES  CONTRAST=0..38  BACKLIGHT=0|10|30|60 (s)  BATTERY=0|1  BEEP=0|1  FMUSB=0|1  VOLUME=0..31
 *   RESDIR=cluster  RESFILE=cluster  RESPOS=file position in 16 KB units  (last file played)
 *   FMFREQ=875..1080 (100 kHz)  FMVOL=0..31  (FM station, common/fmstate.h; absent = FM defaults)
 *   CLOCKDAY=days since 2000-01-01  CLOCKMIN=minute of the day  (the clock anchor, cfg.h) */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "../../core/config.h"
#include "ftlw.h"
#include "cfg.h"
#include "fmstate.h"
#include "rtc.h"

#define CFG_BUF ((char *)HAL_MODRAM_BASE)            /* ZRAM1: the NAND DMA source must be < 0x4000 */
#define CFG_OLD (CFG_BUF + ZCFG_FILE_SIZE)            /* what is on the disk now */
static const uint8_t bl_secs[NBL] = { 0, 10, 30, 60 };
uint16_t cfg_aday, cfg_amin;
uint8_t cfg_anchor_force;
#define ANCHOR_SLACK 30      /* minutes the stored anchor may lag before it forces a write */

static uint8_t cfg_find(fat_dirent_t *e)
{
    fat_file_t d;
    fat_root(&d);
    return fat_find(&d, ZCFG_FILE, e) && !(e->attr & FAT_ATTR_DIR) && e->clus >= 2 &&
           e->size == ZCFG_FILE_SIZE;
}

static uint32_t cfg_num(const char *p)
{
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (uint8_t)(*p++ - '0');
    return n;
}

static void flag(uint8_t f, uint32_t on)
{
    if (on) ZCFG->flags |= f; else ZCFG->flags &= ~f;
}

static void fm_def(void)                              /* FM_STATE valid, defaults first */
{
    if (FMST_OK) return;
    FMST->magic = FM_MAGIC;
    FMST->f10 = FMST->sv_f10 = FM_FREQ_DEF;
    FMST->vol = FMST->sv_vol = FM_VOL_DEF;
}

static void cfg_line(char *l)
{
    char *v = strchr(l, '=');
    uint32_t n;
    if (!v) return;
    *v++ = 0;
    n = cfg_num(v);
    if (!strcmp(l, "LANG")) ZCFG->lang = (v[0] == 'E' && v[1] == 'S') ? LANG_ES : LANG_EN;
    else if (!strcmp(l, "CONTRAST")) { if (n <= CONTRAST_MAX) ZCFG->contrast = (uint8_t)n; }
    else if (!strcmp(l, "BACKLIGHT")) ZCFG->backlight = !n ? BL_ALWAYS : n <= 10 ? BL_10S : n <= 30 ? BL_30S : BL_60S;
    else if (!strcmp(l, "BATTERY")) flag(ZF_BATT, n);
    else if (!strcmp(l, "BEEP")) flag(ZF_BEEP, n);
    else if (!strcmp(l, "FMUSB")) flag(ZF_FMUSB, n);
    else if (!strcmp(l, "VOLUME")) { if (n <= 31) ZCFG->vol = (uint8_t)n; }
    else if (!strcmp(l, "RESDIR")) ZCFG->res_dir = n;
    else if (!strcmp(l, "RESFILE")) ZCFG->res_file = n;
    else if (!strcmp(l, "RESPOS")) ZCFG->res_pos = (uint16_t)n;
    else if (!strcmp(l, "CLOCKDAY")) cfg_aday = (uint16_t)n;
    else if (!strcmp(l, "CLOCKMIN")) { if (n < 1440) cfg_amin = (uint16_t)n; }
    else if (!strcmp(l, "FMFREQ")) { if (n >= FM_FREQ_MIN && n <= FM_FREQ_MAX) { fm_def(); FMST->f10 = FMST->sv_f10 = (uint16_t)n; } }
    else if (!strcmp(l, "FMVOL")) { if (n <= FM_VOL_MAX) { fm_def(); FMST->vol = FMST->sv_vol = (uint8_t)n; } }
}

void cfg_load(void)
{
    char l[24], c;
    uint8_t n = 0;
    uint16_t i, got;
    fat_file_t f;
    fat_dirent_t e;
    memset(ZCFG, 0, sizeof *ZCFG);
    cfg_aday = cfg_amin = 0;
    ZCFG->contrast = CONTRAST_DEF;
    ZCFG->flags = ZF_BATT | ZF_BEEP;
    ZCFG->vol = VOL_DEF;
    if (cfg_find(&e)) {
        fat_open(&f, &e);
        got = fat_read(&f, CFG_BUF, ZCFG_FILE_SIZE);
        for (i = 0; i <= got; i++) {
            c = i < got ? CFG_BUF[i] : '\n';
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == '\n' || c == '\r') { l[n] = 0; cfg_line(l); n = 0; }
            else if (c != ' ' && n < sizeof l - 1) l[n++] = c;
        }
    }
    ZCFG->magic = ZCFG_MAGIC;
}

static uint16_t old_num(const char *key)             /* a 9-char "KEY=" in the old file image -> its value */
{
    uint16_t i;
    for (i = 0; i + 10 <= ZCFG_FILE_SIZE; i++)
        if (!memcmp(CFG_OLD + i, key, 9)) return (uint16_t)cfg_num(CFG_OLD + i + 9);
    return 0;
}

static char *put(char *p, const char *k, uint32_t v)
{
    char t[11];
    uint8_t i = 10;
    while (*k) *p++ = *k++;
    t[10] = 0;
    do { t[--i] = '0' + (uint8_t)(v % 10); v /= 10; } while (v);
    for (k = t + i; *k; ) *p++ = *k++;
    *p++ = '\r'; *p++ = '\n';
    return p;
}

uint8_t cfg_save(void)
{
    fat_dirent_t e;
    fat_file_t f;
    char *p = CFG_BUF;
    uint16_t pre, oday = 0, omin = 0, nmin = 0;
    clk_t t;
    uint8_t oldok;
    if (ZCFG->magic != ZCFG_MAGIC || !cfg_find(&e)) return 0;
    memset(CFG_BUF, ' ', ZCFG_FILE_SIZE);
    memcpy(p, ZCFG->lang == LANG_ES ? "LANG=ES\r\n" : "LANG=EN\r\n", 9); p += 9;
    p = put(p, "CONTRAST=", ZCFG->contrast);
    p = put(p, "BACKLIGHT=", bl_secs[ZCFG->backlight < NBL ? ZCFG->backlight : 0]);
    p = put(p, "BATTERY=", (ZCFG->flags & ZF_BATT) != 0);
    p = put(p, "BEEP=", (ZCFG->flags & ZF_BEEP) != 0);
    p = put(p, "FMUSB=", (ZCFG->flags & ZF_FMUSB) != 0);
    p = put(p, "VOLUME=", ZCFG->vol);
    p = put(p, "RESDIR=", ZCFG->res_dir);
    p = put(p, "RESFILE=", ZCFG->res_file);
    p = put(p, "RESPOS=", ZCFG->res_pos);
    if (FMST_OK) { p = put(p, "FMFREQ=", FMST->f10); p = put(p, "FMVOL=", FMST->vol); }
    fat_open(&f, &e);
    oldok = fat_read(&f, CFG_OLD, ZCFG_FILE_SIZE) == ZCFG_FILE_SIZE;
    pre = p - CFG_BUF;
    if (CLK_OK) {                                     /* the clock anchor, last in the file */
        clk_get(&t);
        nmin = t.hour * 60 + t.min;
        if (oldok) { oday = old_num("CLOCKDAY="); omin = old_num("CLOCKMIN="); }
        if (oldok && !cfg_anchor_force && oday == t.day && nmin >= omin && nmin - omin < ANCHOR_SLACK &&
            !memcmp(CFG_OLD, CFG_BUF, pre))
            goto saved;                               /* only the anchor would change: keep the file */
        p = put(p, "CLOCKDAY=", t.day);
        put(p, "CLOCKMIN=", nmin);
    }
    if (oldok && !memcmp(CFG_OLD, CFG_BUF, ZCFG_FILE_SIZE))   /* unchanged: no NAND write (wear) */
        goto saved;
    if (ftlw_write(fat_clus_lba(e.clus), CFG_BUF) || ftlw_flush()) return 0;
saved:
    FMST->sv_f10 = FMST->f10; FMST->sv_vol = FMST->vol;  /* FM_STATE: what the file holds now */
    return 1;
}
