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
 * PLAY held 2 s (power) is ignored while writing (the loop does not read events).
 *   BREC BACKUP / BREC UPDATE / BREC RESTORE: the boot record in NAND blocks 0/1 (see the BOOT
 *                     RECORD section below; emulator-verified only). */
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
static uint32_t uoff, ulen, upos, usize;                  /* image offset/length in UPDATE.FW, position */

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
#define Y_MSG 26                                   /* actions use the whole area under the title */
static void clear_msgs(void) { gfx_fill(0, Y_MSG - 6, LCD_W, LCD_H - 10 - Y_MSG + 6, C_BG); }

#define N_ITEMS 5
static const uint8_t items[N_ITEMS] = { S_BACKUP, S_UPDATE, S_BBACKUP, S_BUPDATE, S_BRESTORE };
static void menu(void)
{
    uint8_t i;
    for (i = 0; i < N_ITEMS; i++) {
        gfx_fill(0, 22 + i * 14, LCD_W, 13, i == sel ? C_SELBG : C_BG);
        gfx_text(8, 25 + i * 14, T(items[i]), 1, i == sel ? C_SELFG : C_FG, i == sel ? C_SELBG : C_BG);
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
static uint8_t open_file(const char *name11)       /* root file -> uf, ulen; its sector 0 in B0 */
{
    fat_dirent_t e;
    fat_root(&uf);
    if (!fat_find(&uf, name11, &e) || (e.attr & FAT_ATTR_DIR)) return 0;
    fat_open(&uf, &e);
    if (fat_read(&uf, B0, 512) != 512) return 0;
    uoff = 0; ulen = usize = e.size; upos = 512;
    return 1;
}

static uint8_t open_update(void)                  /* UPDATE.FW -> uf, uoff, ulen */
{
    uint8_t i;
    if (!open_file("UPDATE  FW ")) return 0;
    if (!memcmp(B0, "AFI", 4)) {                  /* AFI container: its FWIMAGE.FW part */
        for (i = 1; i < 16; i++)
            if (!memcmp(B0 + i * 32, "FWIMAGE FW ", 11)) {
                uoff = *(uint32_t *)(B0 + i * 32 + 0x10);
                ulen = *(uint32_t *)(B0 + i * 32 + 0x14);
                break;
            }
        if (i == 16 || (uint16_t)uoff & 511 || uoff + ulen > usize) return 0;
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

/* ---- BOOT RECORD (BREC) -------------------------------------------------------------------
 * The 16 KB boot record the mask ROM loads: NAND physical block 0 pages 0-7 (copy: block 1),
 * raw, outside the FTL (NAND_AND_BOOT.md 5). Each 512-byte sector has the spare FF x8, hardware
 * ECC x4, FF FF 55 FF. The ROM reads only the data (block 0, then 1, 2, 3) and takes the first
 * block whose u16 at 0x3FFC is 0x55AA and u16 at 0x3FFE = the sum of the u16 words 0..0x3FFC.
 *   BREC BACKUP   installed BREC (block 0, else 1) -> BREC.BIN (BRCnnn.BIN if BREC.BIN exists)
 *   BREC UPDATE   NEWBREC.BIN -> blocks 1 then 0 (BREC.BIN is made first if missing)
 *   BREC RESTORE  BREC.BIN    -> blocks 1 then 0
 * RISK: a BREC that passes the sum but does not work (hangs, no LCD, no NAND) bricks the player
 * until ROM recovery: the ROM only enters its USB recovery mode when NO block passes the check,
 * so the NAND must be kept from answering at power-up (e.g. CE1# held inactive by a switch, see
 * NAND_AND_BOOT.md), then s1clone writes the dump back. Only install BRECs made by brecpatch.py
 * from this player's own BREC.BIN. */
#define BREC_SECS 32
#define BREC_SIG 0x55AA
enum { BS_NAND0, BS_NAND1, BS_FILE };              /* NAND block 0, block 1, the open file */
static const char n_bak[] = "BREC    BIN", n_new[] = "NEWBREC BIN";
#define bhead (B1 + 512)                          /* ZRAM1 scratch (module RAM is full):    */
#define bentry (bhead + 8)                         /* installed 0x0000-0x0007, 0x0200-0x0214 */
#define BENTRY_N 0x15
#define becc (bentry + BENTRY_N)                   /* ECC written per sector, 4 bytes each */
static uint8_t inst;
static uint16_t bdiff;
static uint8_t bcode;
static char bname[13];

static uint8_t brd(uint8_t bs, uint8_t s, uint8_t *buf, uint16_t len)
{
    nand_req_t r;
    if (bs == BS_FILE) { src = SRC_FILE; return rd(s, buf); }
    r.c0 = len == 512 ? 0 : (s & 3) << 4;          /* len 16: the sector's spare group */
    r.c1 = len == 512 ? (s & 3) << 1 : 0x08;       /* spare = column 0x800 + 16 * sector */
    r.r0 = (uint8_t)(bs << 6) | (s >> 2);          /* row = block * 64 + page */
    r.r1 = r.r2 = 0;
    r.buf = (uint16_t)buf; r.len = len;
    nand_read(&r);
    return 1;
}

/* 0x55AA + sum (BROM 0x436E); ref 1 = remember the family bytes, 2 = compare with them.
 * -> 0 or a string id */
static uint8_t brec_check(uint8_t bs, uint8_t ref)
{
    uint8_t s, n, *m;
    uint16_t i, sum = 0, *w = (uint16_t *)B0;
    for (s = 0; s < BREC_SECS; s++) {
        if (!brd(bs, s, B0, 512)) return S_ERROR;
        if (s < 2) {
            m = s ? bentry : bhead;
            n = s ? BENTRY_N : 8;
            if (ref == 1) memcpy(m, B0, n);
            else if (ref == 2 && memcmp(m, B0, n)) return S_BFAMILY;
        }
        for (i = 0; i < 256; i++) sum += w[i];
        hal_watchdog();
    }
    sum -= w[255];                                  /* B0 = last sector: w[254] = 0x3FFC */
    return w[254] == BREC_SIG && sum == w[255] ? 0 : S_BSUM;
}

static uint8_t brec_installed(void)                /* inst = the block the ROM boots from */
{
    for (inst = BS_NAND0; inst <= BS_NAND1; inst++)
        if (!brec_check(inst, 1)) return 1;
    fail(S_BINVALID, 0);
    return 0;
}

/* 1 = identical; bdiff = differing bytes, bcode = some outside the screen area */
static uint8_t brec_same(uint8_t a, uint8_t b)
{
    uint8_t s;
    uint16_t i, ad;
    bdiff = 0; bcode = 0;
    for (s = 0; s < BREC_SECS; s++) {
        brd(a, s, B1, 512);
        brd(b, s, B0, 512);
        for (i = 0; i < 512; i++)
            if (B0[i] != B1[i]) {
                bdiff++;
                ad = ((uint16_t)s << 9) + i;        /* paint routine + bitmap 0x3000-0x37FF, sum */
                if ((ad < 0x3000 || ad >= 0x3800) && ad < 0x3FFE) bcode = 1;
            }
    }
    return !bdiff;
}

static void name83(const char *n)
{
    uint8_t i, k = 0;
    for (i = 0; i < 11; i++) {
        if (i == 8) bname[k++] = '.';
        if (n[i] != ' ') bname[k++] = n[i];
    }
    bname[k] = 0;
}

static uint8_t brec_backup(void)                   /* 1 = saved and read back */
{
    uint8_t s, r, b1;
    fat_file_t f;
    fat_dirent_t e;
    char n[12];
    phase(S_CHECKING);
    if (!brec_installed()) return 0;
    b1 = brec_check(BS_NAND1, 2) ? S_BLK1BAD : !brec_same(BS_NAND0, BS_NAND1) ? S_BLK1DIFF : S_BLK1SAME;
    memcpy(n, n_bak, 12);
    fat_root(&f);
    if (fat_find(&f, n, &e)) {                       /* never overwrite the original backup */
        memcpy(n, "BRC000  BIN", 12);
        if (!fatw_next_name(n)) { err_code(FATW_EDIR); return 0; }
    }
    if ((r = fatw_open(B0, 1))) { fail(r == FATW_EFULL ? S_NOROOM : S_ERROR, 0); return 0; }
    for (s = 0; s < BREC_SECS; s++) {
        brd(inst, s, B0, 512);
        if ((r = fatw_put(B0))) { err_code(r); return 0; }
    }
    if ((r = fatw_commit(n, (uint32_t)BREC_SECS << 9))) { err_code(r); return 0; }
    if (!open_file(n) || ulen != (uint32_t)BREC_SECS << 9 || !brec_same(BS_FILE, inst)) {
        err_code(0xF4);
        return 0;
    }
    name83(n);
    clear_msgs();
    line(Y_MSG, T(S_BSAVED), C_OK);
    line(Y_MSG + 12, bname, C_OK);
    line(Y_MSG + 24, T(b1), b1 == S_BLK1SAME ? C_OK : C_SEL);
    return 1;
}

static uint8_t brec_write(uint8_t blk)             /* erase, program, read back -> 0 / error code */
{
    uint8_t s, r, i, v;
    if (blk > 1) for (;;) ;                         /* assert: blocks 0/1 only (watchdog resets) */
    if ((r = ftlw_brec_erase(blk))) return r;
    for (s = 0; s < BREC_SECS; s++) {
        if (!brd(BS_FILE, s, B0, 512)) return FATW_EREAD;
        if ((r = ftlw_brec_prog(((uint16_t)blk << 5) | s, B0))) return r;
        memcpy(becc + s * 4, ftlw_brec_ecc, 4);
        progress(s, 2 * BREC_SECS);
    }
    for (s = 0; s < BREC_SECS; s++) {               /* every data byte and spare byte back */
        if (!brd(BS_FILE, s, B1, 512)) return FATW_EREAD;
        brd(blk, s, B0, 512);
        if (memcmp(B0, B1, 512)) return 0xE1;
        brd(blk, s, B0, 16);
        for (i = 0; i < 16; i++) {
            v = i >= 8 && i < 12 ? becc[s * 4 + i - 8] : i == 14 ? 0x55 : 0xFF;
            if (B0[i] != v) return 0xE2;
        }
        progress(BREC_SECS + s, 2 * BREC_SECS);
    }
    return brec_check(blk, 2) ? 0xE3 : 0;
}

static uint8_t brec_install(uint8_t restore)       /* 1 = both blocks written + verified */
{
    uint8_t r, blk;
    fat_file_t f;
    fat_dirent_t e;
    phase(S_CHECKING);
    if (!brec_installed()) return 0;
    if (!restore) {                                 /* keep the original first */
        fat_root(&f);
        if (!fat_find(&f, n_bak, &e)) {
            if (!brec_backup()) return 0;
            phase(S_CHECKING);
            if (!brec_installed()) return 0;
        } else if (!open_file(n_bak) || ulen != (uint32_t)BREC_SECS << 9 || brec_check(BS_FILE, 2)) {
            fail(S_BAKBAD, 0);
            return 0;
        }
    }
    if (!open_file(restore ? n_bak : n_new)) { fail(restore ? S_NOBAK : S_NONEW, 0); return 0; }
    if (ulen != (uint32_t)BREC_SECS << 9) { fail(S_BAD, T(S_BSIZE)); return 0; }
    if ((r = brec_check(BS_FILE, 2))) { fail(S_BAD, T(r)); return 0; }
    brec_same(BS_FILE, inst);
    name83(restore ? n_bak : n_new);
    clear_msgs();
    line(Y_MSG, bname, C_OK);
    if (!bdiff) line(Y_MSG + 11, T(S_BSAMEINS), C_OK);
    else {
        num_line(Y_MSG + 11, bdiff, T(S_BDIFF));
        line(Y_MSG + 22, T(bcode ? S_BCODE : S_BSCREEN), bcode ? C_SEL : C_OK);
    }
    line(Y_MSG + 36, T(S_BWARN), C_SEL);
    if (!confirm()) return 0;
    skin_footer("");
    gfx_fill(0, LCD_H - 22, LCD_W, 10, C_BG);
    ftlw_flush();                                   /* (nothing open: the FTL is not used here) */
    for (blk = 1; ; blk--) {                        /* the copy first: block 0 stays bootable */
        phase(blk ? S_BBLK1 : S_BBLK0);
        line(Y_MSG + 24, T(S_NOOFF), C_SEL);
        if ((r = brec_write(blk))) {
            err_code(r);
            line(Y_MSG + 24, T(blk ? S_BBLK1 : S_BBLK0), C_SEL);
            line(Y_MSG + 36, T(S_FAILED), C_SEL);
            if (blk) line(Y_MSG + 48, T(S_B0SAFE), C_OK);
            return 0;
        }
        if (!blk) break;
    }
    clear_msgs();
    line(Y_MSG, T(S_BOK), C_OK);
    line(Y_MSG + 12, T(S_REBOOT), C_OK);
    return 1;
}

uint16_t mod_main(void)
{
    event_t ev;
    uint8_t done = 0, ok;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    menu();
    skin_footer(T(S_FOOTER));
    for (;;) {
        skin_wait(&ev, 0);
        if (ev.type != EV_KEY_DOWN) continue;
        if (done) { done = 0; clear_msgs(); menu(); continue; }   /* any key: result -> menu */
        if (ev.code == KEY_MENU) return 0;
        if (ev.code == KEY_LEFT) { sel = sel ? sel - 1 : N_ITEMS - 1; menu(); }
        else if (ev.code == KEY_RIGHT) { sel = sel < N_ITEMS - 1 ? sel + 1 : 0; menu(); }
        else if (ev.code == KEY_PLAY) {
            switch (sel) {
            case 0: backup(); ok = 0; break;
            case 1: ok = update(); break;           /* BREC then refreshes copy B from the new A */
            case 2: brec_backup(); ok = 0; break;
            default: ok = brec_install(sel == 4);
            }
            if (ok) {
                skin_wait(&ev, 150);
                return MOD_REBOOT;
            }
            done = 1;
            skin_footer(T(S_FOOTER));
        }
    }
}
