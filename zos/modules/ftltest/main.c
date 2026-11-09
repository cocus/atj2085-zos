/* FTLTEST: exercises the module-side NAND/FTL write path (modules/common/ftlw.c) on the user disk.
 * Writes only into FREE clusters of the FAT16 volume (found by scanning FAT 1; the FAT itself is not
 * changed, so no file is touched), plus one rewrite of the boot sector with its own content.
 * Sequence (each write = a 512-byte pattern of (lba, seed), see pat()):
 *   cluster A (inside one unit): +0 +1 +9 +10, then +4 (backward: close + reopen), +20, +20 again
 *   (rewrite: close + reopen), ftlw_sync + read-back of +4 (coherence), cluster X (a free cluster
 *   that straddles a unit boundary): all 32 sectors in order, A +31 (other unit), boot sector rewrite,
 *   flush, refused writes (LBA >= 2^23, LBA in a zone that does not exist), read back + compare.
 * Results for the emulator script (zos/tools/emu_ftltest.py) in ZRAM1 at HAL_MODRAM_BASE (ftres_t). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "../common/ftlw.h"

#define MAXW 48
typedef struct {
    char magic[4];              /* "FTLT" when done */
    uint8_t stage, nfail, err, nw;
    uint8_t cmp_bad, coh_bad, boot_bad, refuse_bad;
    uint32_t lba_a, lba_x;
    uint32_t wl[MAXW];          /* writes in order: lba */
    uint8_t ws[MAXW];           /* seed (0 = boot sector rewrite, content = original) */
    uint16_t ticks;             /* duration of the writes + flushes (stage 2..5), 10 ms */
} ftres_t;
#define R ((ftres_t *)HAL_MODRAM_BASE)
#define WBUF ((uint8_t *)(HAL_MODRAM_BASE + 0x200))     /* ZRAM1: DMA source for ftlw_write */
#define RBUF ((uint8_t *)(HAL_MODRAM_BASE + 0x400))

static uint8_t y = SKIN_TITLE_H + 2;

static void say(const char *s, uint16_t c)
{
    gfx_text(4, y, s, 1, c, C_BG);
    y += 10;
}

static void pat(uint32_t lba, uint8_t seed, uint8_t *b)
{
    uint16_t i;
    uint8_t v = (uint8_t)lba * 31 + seed;
    for (i = 0; i < 512; i++) b[i] = (uint8_t)(i * 7) + v;
    memcpy(b, &lba, 4);
    b[4] = seed;
}

static void wr(uint32_t lba, uint8_t seed)
{
    uint8_t r;
    if (seed) pat(lba, seed, WBUF);
    r = ftlw_write(lba, WBUF);
    if (R->nw < MAXW) { R->wl[R->nw] = lba; R->ws[R->nw] = seed; R->nw++; }
    if (r && !R->err) R->err = r;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }

uint16_t mod_main(void)
{
    event_t e;
    uint16_t spc, rsvd, fsz, nroot, i, k, t0;
    uint32_t data0, c, s, cx = 0, ca = 0, nclus;
    uint8_t *b = RBUF, ok;
    memset(R, 0, sizeof *R);
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("FTL WRITE TEST");
    /* FAT16 geometry from the boot sector */
    if (!hal_blk_read(0, b) || b[510] != 0x55 || b[511] != 0xAA) { say("NO BOOT SECTOR", C_SEL); goto out; }
    spc = b[13];
    rsvd = rd16(b + 14);
    nroot = rd16(b + 17);
    fsz = rd16(b + 22);
    s = rd16(b + 19);
    if (!s) memcpy(&s, b + 32, 4);
    if (spc < 32) { say("NEED 32 SECT/CLUSTER", C_SEL); goto out; }
    data0 = rsvd + (uint32_t)b[16] * fsz + ((nroot * 32 + 511) >> 9);
    nclus = (s - data0) / spc;
    R->stage = 1;
    /* free clusters: X straddles a unit boundary (512 sectors), A lies inside another unit */
    for (k = 0; k < fsz && (!cx || !ca); k++) {
        if (!hal_blk_read(rsvd + k, b)) { say("FAT READ ERROR", C_SEL); goto out; }
        hal_watchdog();
        for (i = 0; i < 256; i++) {
            c = (uint32_t)k * 256 + i;
            if (c < 2 || c >= nclus + 2 || rd16(b + 2 * i)) continue;
            s = data0 + (c - 2) * spc;
            if ((s ^ (s + spc - 1)) >> 9) { if (!cx) cx = s; }
            else if (cx && !ca && (s >> 9) > (cx >> 9) + 1) ca = s;
        }
    }
    if (!cx || !ca) { say("NO FREE CLUSTERS", C_SEL); goto out; }
    R->lba_a = ca;
    R->lba_x = cx;
    R->stage = 2;
    t0 = hal_ticks();
    say("WRITING...", C_FG);
    wr(ca + 0, 1); wr(ca + 1, 1); wr(ca + 9, 1); wr(ca + 10, 1);
    wr(ca + 4, 1);                          /* backward */
    wr(ca + 20, 1);
    wr(ca + 20, 2);                         /* rewrite of a written sector */
    /* coherence: the read path sees the new data after ftlw_sync */
    if (ftlw_sync(ca + 4, 1) || !hal_blk_read(ca + 4, b)) R->coh_bad = 1;
    pat(ca + 4, 1, WBUF);
    if (memcmp(b, WBUF, 512)) R->coh_bad |= 2;
    R->stage = 3;
    for (i = 0; i < 32; i++) wr(cx + i, 3);  /* across the unit boundary */
    wr(ca + 31, 4);
    R->stage = 4;
    if (!hal_blk_read(0, WBUF)) R->boot_bad = 1;   /* rewrite the boot sector with itself */
    wr(0, 0);
    if (ftlw_flush() && !R->err) R->err = ftlw_flush();
    R->stage = 5;
    R->ticks = hal_ticks() - t0;
    /* refused: LBA >= 2^23, and a zone that does not exist (nothing may be written) */
    if (ftlw_write(0x00800000UL, WBUF) != FTLW_ERANGE) R->refuse_bad |= 1;
    if (ftlw_write(0x007FFE00UL, WBUF) != FTLW_EMAP) R->refuse_bad |= 2;
    if (ftlw_pending()) R->refuse_bad |= 4;
    say("VERIFYING...", C_FG);
    /* read back: the last seed written for each LBA */
    for (i = 0; i < R->nw; i++) {
        for (k = i + 1; k < R->nw && R->wl[k] != R->wl[i]; k++) ;
        if (k < R->nw) continue;             /* superseded */
        if (!R->ws[i]) continue;             /* boot sector: below */
        pat(R->wl[i], R->ws[i], WBUF);
        if (!hal_blk_read(R->wl[i], b) || memcmp(b, WBUF, 512)) R->cmp_bad++;
        hal_watchdog();
    }
    hal_blk_read(0, b);
    if (b[510] != 0x55 || b[511] != 0xAA || rd16(b + 22) != fsz) R->boot_bad |= 2;
    R->stage = 6;
    ok = !R->err && !R->cmp_bad && !R->coh_bad && !R->boot_bad && !R->refuse_bad;
    R->nfail = !ok;
    say(ok ? "ALL OK" : "FAILED", ok ? C_OK : C_SEL);
    gfx_text(4, y, "ERR", 1, C_DIM, C_BG); gfx_num(40, y, R->err, 1, C_FG, C_BG); y += 10;
    gfx_text(4, y, "CMP", 1, C_DIM, C_BG); gfx_num(40, y, R->cmp_bad, 1, C_FG, C_BG); y += 10;
    gfx_text(4, y, "WRITES", 1, C_DIM, C_BG); gfx_num(52, y, R->nw, 1, C_FG, C_BG); y += 10;
    memcpy(R->magic, "FTLT", 4);
out:
    skin_footer("ANY KEY: BACK");
    do ev_wait(&e, 0); while (e.type != EV_KEY_DOWN);
    return 0;
}
