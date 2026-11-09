/* zos FAT16 file creation on the user disk: see fatw.h (rules, order, limits). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "ftlw.h"
#include "fatw.h"

#define FATW_ATTR_ARCH 0x20
#define FATW_ATTR_SKIP 0x18          /* directory / volume label (and long-name parts) */
#define FATW_EOC       0xFFFF
#define FATW_DATE      0x5C21        /* 2026-01-01: no calendar clock */
#define FATW_TIME      0x6000        /* 12:00:00 */

uint16_t fatw_c0, fatw_n;
uint32_t fatw_nsec;
uint8_t  fatw_spc;
static uint8_t *fb;                  /* caller's ZRAM1 sector buffer */
static uint32_t f_fat, f_root, f_data;
static uint16_t f_fsz, f_ncl, f_rootsec;
static uint8_t f_nfats, f_shift;

#define RD16(p) ((uint16_t)(p)[0] | (uint16_t)(p)[1] << 8)
#define ENT(c) ((uint16_t *)fb + ((uint8_t)(c)))      /* FAT16 entry of cluster c in its sector */

/* read with the NAND write path coherent: a sector in the open unit is flushed first */
static uint8_t rd(uint32_t lba)
{
    return !ftlw_sync(lba, 1) && hal_blk_read(lba, fb);
}

static uint8_t bpb(uint8_t *buf512)
{
    uint32_t tot;
    fb = buf512;
    fatw_n = 0;
    fatw_nsec = 0;
    if (!rd(0) || RD16(fb + 510) != 0xAA55 || RD16(fb + 11) != 512) return FATW_EFS;
    fatw_spc = fb[13];
    for (f_shift = 0; f_shift < 8 && (uint8_t)(1 << f_shift) != fatw_spc; f_shift++) ;
    f_nfats = fb[16];
    f_fsz = RD16(fb + 22);
    tot = RD16(fb + 19);
    if (!tot) tot = *(uint32_t *)(fb + 32);
    if (f_shift == 8 || !f_nfats || f_nfats > 2 || !f_fsz) return FATW_EFS;
    f_fat = RD16(fb + 14);
    f_root = f_fat + (uint32_t)f_fsz * f_nfats;
    f_rootsec = (RD16(fb + 17) + 15) >> 4;
    f_data = f_root + f_rootsec;
    tot = (tot - f_data) >> f_shift;
    if (tot < 4085 || tot >= 65525UL) return FATW_EFS;      /* FAT16 only */
    f_ncl = (uint16_t)tot;
    return FATW_OK;
}

uint8_t fatw_resume(uint8_t *buf512, uint16_t c0, uint16_t n, uint32_t nsec)
{
    uint8_t r = bpb(buf512);
    fatw_c0 = c0; fatw_n = n; fatw_nsec = nsec;
    return r;
}

uint32_t fatw_lba0(void)
{
    return f_data + ((uint32_t)(fatw_c0 - 2) << f_shift);
}

uint8_t fatw_open(uint8_t *buf512, uint16_t min_clusters)
{
    uint16_t s, c, run = 0, start = 0, i;
    uint8_t r;
    if ((r = bpb(buf512))) return r;
    /* largest run of free clusters (2 .. f_ncl + 1) */
    for (s = 0, c = 0; s < f_fsz; s++) {
        if (!rd(f_fat + s)) return FATW_EREAD;
        hal_watchdog();
        for (i = 0; i < 256; i++, c++) {
            if (c < 2) continue;
            if (c > f_ncl + 1) goto done;
            if (*ENT(i)) { run = 0; continue; }
            if (!run++) start = c;
            if (run > fatw_n) { fatw_n = run; fatw_c0 = start; }
        }
    }
done:
    if (!fatw_n || fatw_n < min_clusters) return FATW_EFULL;
    return FATW_OK;
}

uint32_t fatw_room(void)
{
    return ((uint32_t)fatw_n << f_shift) - fatw_nsec;
}

uint8_t fatw_put_at(uint32_t i, const void *buf512)
{
    if (i >= (uint32_t)fatw_n << f_shift) return FATW_EFULL;
    return ftlw_write(f_data + ((uint32_t)(fatw_c0 - 2) << f_shift) + i, buf512);
}

uint8_t fatw_put(const void *buf512)
{
    uint8_t r = fatw_put_at(fatw_nsec, buf512);
    if (!r) fatw_nsec++;
    return r;
}

/* FAT copy k: n clusters from c as one chain (n > 0), or free the chain starting at c (n = 0).
 * One read-modify-write per FAT sector visited (contiguous chains: each sector once, ascending). */
static uint8_t fat_chain(uint8_t k, uint16_t c, uint16_t n)
{
    uint32_t base = f_fat + (uint32_t)f_fsz * k;
    uint16_t steps = f_ncl, nx;
    uint8_t sec, r, alloc = n != 0;
    while (c >= 2 && c <= f_ncl + 1) {
        sec = (uint8_t)(c >> 8);
        if (!rd(base + sec)) return FATW_EREAD;
        do {
            if (!steps--) return FATW_EFS;                     /* a loop in the old chain */
            if (alloc) nx = --n ? c + 1 : FATW_EOC;
            else nx = *ENT(c);                                 /* 0 / EOC end the walk */
            *ENT(c) = alloc ? nx : 0;
            c = nx;
        } while (c >= 2 && c <= f_ncl + 1 && (uint8_t)(c >> 8) == sec);
        if ((r = ftlw_write(base + sec, fb))) return r;
        hal_watchdog();
    }
    return FATW_OK;
}

/* root directory: slot of name11 (or the first free one) -> *lba, *off; *old = its cluster */
static uint8_t dir_slot(const char *name11, uint32_t *lba, uint8_t *off, uint16_t *old)
{
    uint16_t k;
    uint8_t j, *e, have = 0;
    *old = 0;
    for (k = 0; k < f_rootsec; k++) {
        if (!rd(f_root + k)) return FATW_EREAD;
        for (j = 0; j < 16; j++) {
            e = fb + j * 32;
            if (!e[0] || e[0] == 0xE5) {
                if (!have) { have = 1; *lba = f_root + k; *off = j; }
                if (!e[0]) return have ? FATW_OK : FATW_EDIR;
            } else if (!(e[11] & FATW_ATTR_SKIP) && !memcmp(e, name11, 11)) {
                *lba = f_root + k; *off = j; *old = RD16(e + 26);
                return FATW_OK;
            }
        }
    }
    return have ? FATW_OK : FATW_EDIR;
}

uint8_t fatw_commit(const char *name11, uint32_t size)
{
    uint32_t lba;
    uint16_t old, ncl;
    uint8_t off, r, k, *e;
    if ((r = ftlw_flush())) return r;                         /* 1. data in place */
    ncl = (uint16_t)((size + ((uint32_t)fatw_spc << 9) - 1) >> (f_shift + 9));
    if (ncl > fatw_n || size > fatw_nsec << 9) return FATW_EFULL;            /* only written data */
    if ((r = dir_slot(name11, &lba, &off, &old))) return r;   /* before any FAT change */
    for (k = 0; ncl && k < f_nfats; k++)                     /* 2. new chain, FAT1 then FAT2 */
        if ((r = fat_chain(k, fatw_c0, ncl))) return r;
    if (!rd(lba)) return FATW_EREAD;                          /* 3. directory entry */
    e = fb + off * 32;
    memset(e, 0, 32);                                         /* (a long name stays valid:
                                                                 its checksum is of name11) */
    memcpy(e, name11, 11);
    e[11] = FATW_ATTR_ARCH;
    *(uint16_t *)(e + 14) = FATW_TIME;
    *(uint16_t *)(e + 16) = FATW_DATE;
    *(uint16_t *)(e + 18) = FATW_DATE;
    *(uint16_t *)(e + 22) = FATW_TIME;
    *(uint16_t *)(e + 24) = FATW_DATE;
    *(uint16_t *)(e + 26) = ncl ? fatw_c0 : 0;
    *(uint32_t *)(e + 28) = size;
    if ((r = ftlw_write(lba, fb)) || (r = ftlw_flush())) return r;
    if (old >= 2)                                             /* 4. the replaced file's chain */
        for (k = 0; k < f_nfats; k++)
            if ((r = fat_chain(k, old, 0))) return r;
    if ((r = ftlw_flush())) return r;
    fat_mount();                                              /* drop the core's FAT cache */
    return FATW_OK;
}

uint8_t fatw_next_name(char *name11)
{
    uint16_t k, v, best = 0;
    uint8_t j, *e, d;
    for (k = 0; k < f_rootsec; k++) {
        if (!rd(f_root + k)) return 0;
        for (j = 0; j < 16; j++) {
            e = fb + j * 32;
            if (!e[0]) goto out;
            if (e[0] == 0xE5 || memcmp(e, name11, 3) || memcmp(e + 6, name11 + 6, 5)) continue;
            for (v = 0, d = 3; d < 6 && e[d] >= '0' && e[d] <= '9'; d++) v = v * 10 + e[d] - '0';
            if (d == 6 && v + 1 > best) best = v + 1;
        }
    }
out:
    if (best > 999) return 0;
    name11[3] = '0' + best / 100;
    name11[4] = '0' + best / 10 % 10;
    name11[5] = '0' + best % 10;
    return 1;
}
