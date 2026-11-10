/* FIRMWARE: back up / update the firmware image (FWIMAGE.FW in the NAND firmware area) through
 * the normal USB disk - no PC tools beyond copying a file.
 *   BACKUP TO DISK    copy A (firmware sectors 0 .. end of the last directory entry) ->
 *                     FIRMWARE.FW in the disk root (common/fatw.c), then read back and compared.
 *   UPDATE FROM DISK  UPDATE.FW in the root (a raw FWIMAGE "55 AA F0 0F", or an AFI .fw as s1res
 *                     takes it: its FWIMAGE.FW part is used). Checked exactly like BREC checks a
 *                     copy at every boot (BREC 0x06DB-0x091F, lisong_rebuild/analysis/boot.md):
 *                     magic, header u16 sum, directory u32 sum (sectors 1-15), the u32 sum of every
 *                     file except *.$$$ / *.RES over the sectors BREC sums, SYSCFG.SYS present; plus
 *                     a directory end marker and the image fitting copy A (8 MB). A summary, then
 *                     >> held 2 s writes copy A through the FTL write path (ftlw_fwwrite: firmware
 *                     units 0..0x1F only), reads every sector back, re-checks copy A and reboots.
 * Why an interrupted update is recoverable (emulator-verified with the real BREC, see the report /
 * NAND_AND_BOOT.md): BREC verifies copy A at every boot; if A is invalid it fully verifies copy B
 * and copies B over A (old firmware back); if A is valid and its header differs from B it copies
 * A over B. Copy B is never written here, so until BREC itself refreshes it, it holds the old
 * firmware. Both invalid -> BREC jumps to the mask-ROM USB recovery (ADFU).
 * PLAY held 2 s (power) is ignored while writing (the loop does not read events). */
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
#define ZS_PROVIDE
#include "strings.h"

#define B0 ((uint8_t *)HAL_MODRAM_BASE)            /* ZRAM1: NAND DMA source / hal_*_read buffers */
#define B1 (B0 + 512)
#define COPY_B 0x4000U                             /* copy B = firmware sector 0x4000 (BREC 0x062F) */
#define FW_MAGIC 0x0FF0AA55UL
#define DIR_SECS 15
#define CONFIRM_REPS 15                            /* key repeat 500 ms + 15 x 100 ms = 2 s */

uint32_t fw_acc;
void fw_sum512(const uint8_t *p);                  /* fwsum.s */

enum { SRC_A, SRC_B, SRC_FILE };
static uint8_t src;
static fat_file_t uf;
static uint32_t uoff, ulen, upos;                  /* image offset/length in UPDATE.FW, position */

static uint8_t rd(uint16_t s, uint8_t *buf)
{
    uint32_t p;
    if (src != SRC_FILE) return hal_fw_read(s + (src == SRC_B ? COPY_B : 0), buf);
    p = (uint32_t)s << 9;
    if (p + 512 > ulen) return 0;
    p += uoff;
    if (p != upos && !fat_seek(&uf, p)) return 0;  /* sequential reads never seek */
    upos = p + 512;
    return fat_read(&uf, buf, 512) == 512;
}

/* ---- the BREC check -------------------------------------------------------------------- */
enum { CK_OK, CK_READ, CK_MAGIC, CK_HDR, CK_DIR, CK_FILE, CK_NOSYS, CK_NOEND, CK_BIG };
static const char *const ck_str[] = { "", "READ", "MAGIC", "HEADER SUM", "DIR SUM", "", "NO SYSCFG",
                                      "DIR END", "> 8 MB" };
static uint8_t hdr_id[8];                          /* header +4..+7, +0x10..+0x13 (what BREC compares) */
static uint16_t img_end, n_files, n_checked;
static char bad_name[12];

static void progress(uint16_t n, uint16_t of);

static uint8_t fw_check(void)
{
    uint16_t s, p, n, k, i, sum;
    uint8_t j, sys = 0, *e;
    uint32_t want;
    img_end = DIR_SECS + 1; n_files = n_checked = 0;
    if (!rd(0, B0)) return CK_READ;
    if (*(uint32_t *)B0 != FW_MAGIC) return CK_MAGIC;
    for (i = 0, sum = 0; i < 255; i++) sum += ((uint16_t *)B0)[i];
    if (sum != ((uint16_t *)B0)[255]) return CK_HDR;
    memcpy(hdr_id, B0 + 4, 4);
    memcpy(hdr_id + 4, B0 + 0x10, 4);
    want = *(uint32_t *)(B0 + 0x10);
    for (fw_acc = 0, s = 1; s <= DIR_SECS; s++) {
        if (!rd(s, B0)) return CK_READ;
        fw_sum512(B0);
    }
    if (fw_acc != want) return CK_DIR;
    for (s = 1; s <= DIR_SECS; s++) {
        if (!rd(s, B1)) return CK_READ;
        for (j = 0; j < 16; j++) {
            e = B1 + j * 32;
            if (!e[0]) return sys ? (img_end > FTLW_FW_A_SECTORS ? CK_BIG : CK_OK) : CK_NOSYS;
            n_files++;
            if (!memcmp(e, "SYSCFG  SYS", 11)) sys = 1;
            p = *(uint16_t *)(e + 0x10);
            if (*(uint16_t *)(e + 0x12) || *(uint8_t *)(e + 0x17)) return CK_BIG;
            k = p + (uint16_t)((*(uint32_t *)(e + 0x14) + 511) >> 9);
            if (k > img_end) img_end = k;
            if (e[8] == '$' || (e[8] == 'R' && e[10] == 'S')) continue;   /* BREC 0x0836-0x084C */
            n_checked++;
            progress(s * 16 + j, DIR_SECS * 16);
            n = *(uint16_t *)(e + 0x15);                /* BREC 0x0894: (len >> 8) & 0xFFFF */
            fw_acc = 0;
            do {
                if (n >= 0x40) { k = 32; n -= 0x40; }
                else { k = (uint8_t)n >> 1; if (!k) k = 256; n = 0; }   /* djnz with B = 0 */
                while (k--) {
                    if (!rd(p++, B0)) return CK_READ;
                    fw_sum512(B0);
                }
                hal_watchdog();
            } while (n);
            if (p > img_end) img_end = p;
            if (fw_acc != *(uint32_t *)(e + 0x1C)) { memcpy(bad_name, e, 11); return CK_FILE; }
        }
    }
    return CK_NOEND;                                /* BREC would walk on into the data */
}

/* ---- screen ------------------------------------------------------------------------------ */
static uint8_t sel;
static void line(uint8_t y, const char *s, uint16_t c)
{
    gfx_fill(0, y, LCD_W, 10, C_BG);
    gfx_text(4, y + 1, s, 1, c, C_BG);
}
static void num_line(uint8_t y, uint16_t v, const char *label)
{
    gfx_fill(0, y, LCD_W, 10, C_BG);
    gfx_num(4, y + 1, v, 1, C_OK, C_BG);
    gfx_text(46, y + 1, label, 1, C_FG, C_BG);
}
#define Y_MSG 60
static void clear_msgs(void) { gfx_fill(0, Y_MSG - 2, LCD_W, LCD_H - 10 - Y_MSG + 2, C_BG); }

static void menu(void)
{
    uint8_t i;
    for (i = 0; i < 2; i++) {
        gfx_fill(0, 22 + i * 14, LCD_W, 13, i == sel ? C_SELBG : C_BG);
        gfx_text(8, 25 + i * 14, T(i ? S_UPDATE : S_BACKUP), 1, i == sel ? C_SELFG : C_FG,
                 i == sel ? C_SELBG : C_BG);
    }
}

static uint16_t last_px;
static void progress(uint16_t n, uint16_t of)    /* bar at Y_MSG + 12 */
{
    uint16_t px = (uint16_t)((uint32_t)n * (LCD_W - 8) / of);
    if (px == last_px) return;
    if (px < last_px) gfx_fill(4, Y_MSG + 12, LCD_W - 8, 6, C_DIM);
    gfx_fill(4, Y_MSG + 12, (uint8_t)px, 6, C_OK);
    last_px = px;
}
static void phase(uint8_t str)
{
    clear_msgs();
    line(Y_MSG, T(str), C_FG);
    gfx_fill(4, Y_MSG + 12, LCD_W - 8, 6, C_DIM);
    last_px = 0;
}
static void fail(uint8_t str, const char *detail)
{
    clear_msgs();
    line(Y_MSG, T(str), C_SEL);
    if (detail) line(Y_MSG + 12, detail, C_SEL);
}
static void err_code(uint8_t r)
{
    static char t[] = "ERROR 00";
    t[6] = "0123456789ABCDEF"[r >> 4];
    t[7] = "0123456789ABCDEF"[r & 15];
    fail(S_ERROR, t);
}

/* ---- BACKUP ------------------------------------------------------------------------------ */
static void backup(void)
{
    uint16_t s;
    uint8_t r;
    fat_file_t f;
    fat_dirent_t e;
    phase(S_CHECKING);
    src = SRC_A;
    r = fw_check();                                 /* also gives the image size */
    line(Y_MSG + 24, T(r ? S_INSTBAD : S_INSTOK), r ? C_SEL : C_OK);
    if (r == CK_READ || r == CK_MAGIC || r == CK_HDR || r == CK_DIR || r == CK_NOEND) return;
    if ((r = fatw_open(B0, (img_end + 31) / 32))) {
        fail(r == FATW_EFULL ? S_NOROOM : S_ERROR, 0);
        return;
    }
    line(Y_MSG, T(S_READING), C_FG);
    for (s = 0; s < img_end; s++) {
        if (!hal_fw_read(s, B0)) { err_code(FATW_EREAD); return; }
        if ((r = fatw_put(B0))) { err_code(r); return; }
        if (!(s & 31)) { progress(s, img_end); hal_watchdog(); }
    }
    if ((r = fatw_commit("FIRMWAREFW ", (uint32_t)img_end << 9))) { err_code(r); return; }
    phase(S_VERIFY);                                /* read the new file back through the FAT */
    fat_root(&f);
    if (!fat_find(&f, "FIRMWAREFW ", &e) || e.size != (uint32_t)img_end << 9) { err_code(0xF1); return; }
    fat_open(&f, &e);
    for (s = 0; s < img_end; s++) {
        if (fat_read(&f, B1, 512) != 512 || !hal_fw_read(s, B0) || memcmp(B0, B1, 512)) {
            err_code(0xF2);
            return;
        }
        if (!(s & 31)) { progress(s, img_end); hal_watchdog(); }
    }
    clear_msgs();
    line(Y_MSG, T(S_SAVED), C_OK);
    num_line(Y_MSG + 12, (uint16_t)(img_end / 2), T(S_KB));
}

/* ---- UPDATE ------------------------------------------------------------------------------ */
static uint8_t open_update(void)                  /* UPDATE.FW -> uf, uoff, ulen */
{
    fat_dirent_t e;
    uint8_t i;
    fat_root(&uf);
    if (!fat_find(&uf, "UPDATE  FW ", &e) || (e.attr & FAT_ATTR_DIR)) return 0;
    fat_open(&uf, &e);
    if (fat_read(&uf, B0, 512) != 512) return 0;
    uoff = 0; ulen = e.size; upos = 512;
    if (!memcmp(B0, "AFI", 4)) {                  /* AFI container: its FWIMAGE.FW part */
        for (i = 1; i < 16; i++)
            if (!memcmp(B0 + i * 32, "FWIMAGE FW ", 11)) {
                uoff = *(uint32_t *)(B0 + i * 32 + 0x10);
                ulen = *(uint32_t *)(B0 + i * 32 + 0x14);
                break;
            }
        if (i == 16 || (uint16_t)uoff & 511 || uoff + ulen > e.size) return 0;
    }
    return 1;
}

static uint8_t confirm(void)                       /* >> held 2 s = yes, MENU = no */
{
    event_t ev;
    uint8_t reps = 0;
    line(LCD_H - 22, T(S_HOLD), C_SEL);
    skin_footer(T(S_CANCEL));
    for (;;) {
        skin_wait(&ev, 0);
        if (ev.type == EV_KEY_DOWN && ev.code == KEY_MENU) return 0;
        if (ev.code != KEY_RIGHT) continue;
        if (ev.type == EV_KEY_DOWN) reps = 0;
        else if (ev.type == EV_KEY_UP) reps = 0;
        else if (ev.type == EV_KEY_REPEAT && ++reps >= CONFIRM_REPS) return 1;
    }
}

static uint8_t update(void)                        /* 1 = written + verified: reboot */
{
    uint8_t r, id[8];
    uint16_t s;
    phase(S_CHECKING);
    if (!open_update()) { fail(S_NOFILE, 0); return 0; }
    src = SRC_FILE;
    r = fw_check();
    if (r) { fail(S_BAD, r == CK_FILE ? bad_name : ck_str[r]); return 0; }
    memcpy(id, hdr_id, 8);
    src = SRC_A;                                   /* installed copy A: same header id = same image */
    if (rd(0, B0) && !memcmp(B0 + 4, id, 4) && !memcmp(B0 + 0x10, id + 4, 4)) {
        fail(S_SAME, 0);
        return 0;
    }
    src = SRC_FILE;
    clear_msgs();
    line(Y_MSG, "UPDATE.FW OK", C_OK);
    num_line(Y_MSG + 11, img_end / 2, T(S_KB));
    num_line(Y_MSG + 22, n_files, T(S_FILES));
    num_line(Y_MSG + 33, n_checked, T(S_CHECKED));
    if (!confirm()) return 0;
    skin_footer("");
    gfx_fill(0, LCD_H - 22, LCD_W, 10, C_BG);
    phase(S_WRITING);
    line(Y_MSG + 24, T(S_NOOFF), C_SEL);
    for (s = 0; s < img_end; s++) {
        if (s >= FTLW_FW_A_SECTORS) for (;;) ;     /* assert: copy A only (watchdog resets) */
        if (!rd(s, B0)) { err_code(FATW_EREAD); goto failed; }
        if ((r = ftlw_fwwrite(s, B0))) { err_code(r); goto failed; }
        if (!(s & 31)) { progress(s, img_end); hal_watchdog(); }
    }
    if ((r = ftlw_flush())) { err_code(r); goto failed; }
    phase(S_VERIFY);                               /* every sector back from the flash */
    line(Y_MSG + 24, T(S_NOOFF), C_SEL);
    for (s = 0; s < img_end; s++) {
        if (!rd(s, B1) || !hal_fw_read(s, B0) || memcmp(B0, B1, 512)) { err_code(0xF3); goto failed; }
        if (!(s & 31)) { progress(s, img_end); hal_watchdog(); }
    }
    phase(S_CHECKING);
    src = SRC_A;
    if ((r = fw_check())) { fail(S_INSTBAD, ck_str[r]); goto failed; }
    clear_msgs();
    line(Y_MSG, T(S_REBOOT), C_OK);
    return 1;
failed:
    line(Y_MSG + 36, T(S_FAILED), C_SEL);
    line(Y_MSG + 48, T(S_FALLBACK), C_SEL);    /* copy A invalid -> BREC copies B over it */
    return 0;
}

uint16_t mod_main(void)
{
    event_t ev;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    menu();
    skin_footer(T(S_FOOTER));
    for (;;) {
        skin_wait(&ev, 0);
        if (ev.type != EV_KEY_DOWN) continue;
        if (ev.code == KEY_MENU) return 0;
        if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) { sel ^= 1; menu(); }
        else if (ev.code == KEY_PLAY) {
            if (!sel) backup();
            else if (update()) {
                skin_wait(&ev, 150);
                return MOD_REBOOT;                 /* BREC refreshes copy B from the new A */
            }
            skin_footer(T(S_FOOTER));
        }
    }
}
