/* zos FAT12/16/32 reader. One 512-byte sector cache (ZRAM1) serves FAT, directory and data
 * sectors; data is copied out of it, so callers' buffers may live anywhere (e.g. a module page).
 * Size: SDCC's 32-bit code through pointers is huge, so every public call copies the caller's
 * fat_file_t into the static F, works on F (direct addressing) and copies it back. */
#include <stdint.h>
#include <string.h>
#include "../hal/hal.h"
#include "fat.h"

static uint8_t cache[512];
static uint32_t cache_lba = 0xFFFFFFFFUL;

static uint8_t type;                 /* 12 / 16 / 32, 0 = not mounted */
static uint8_t spc, spc_shift;       /* sectors per cluster (power of 2) */
static uint32_t fat_lba, root_lba, data_lba, root_clus, eoc_min;
static uint16_t root_secs;
static fat_file_t F;                 /* working copy */

#define RD16(p) ((uint16_t)(p)[0] | (uint16_t)(p)[1] << 8)
static uint32_t rd32(const uint8_t *p) { return *(const uint32_t *)p; }   /* Z80: little endian */

static const uint8_t *sec(uint32_t lba)
{
    if (lba != cache_lba) {
        cache_lba = 0xFFFFFFFFUL;
        if (!hal_blk_read(lba, cache)) return 0;
        cache_lba = lba;
    }
    return cache;
}

/* BIOS parameter block, little endian, from boot sector offset 11 */
static struct {
    uint16_t bps; uint8_t spc; uint16_t rsvd; uint8_t nfats; uint16_t rootent, tot16;
    uint8_t media; uint16_t fsz16, spt, heads; uint32_t hidden, tot32, fsz32;
    uint16_t flags, ver; uint32_t rootclus;
} bpb;
static uint32_t base;

uint8_t fat_mount(void)
{
    const uint8_t *b;
    uint8_t i;
    type = 0;
    base = 0;
    cache_lba = 0xFFFFFFFFUL;
    if (!hal_blk_init()) return 0;
    for (i = 0;; i++) {              /* superfloppy, or MBR -> first partition */
        if (!(b = sec(base)) || RD16(b + 510) != 0xAA55) return 0;
        memcpy(&bpb, b + 11, sizeof bpb);
        spc = bpb.spc;
        if (bpb.bps == 512 && spc && !(spc & (spc - 1)) && bpb.nfats) break;
        if (i) return 0;
        base = rd32(b + 0x1C6);
    }
    for (spc_shift = 0; (uint8_t)(1 << spc_shift) != spc; spc_shift++) ;
    if (bpb.tot16) bpb.tot32 = bpb.tot16;
    if (bpb.fsz16) bpb.fsz32 = bpb.fsz16;
    root_secs = (bpb.rootent + 15) >> 4;
    root_clus = bpb.rootclus;
    fat_lba = base + bpb.rsvd;
    root_lba = fat_lba;
    for (i = bpb.nfats; i; i--) root_lba += bpb.fsz32;
    data_lba = root_lba + root_secs;
    bpb.tot32 = (bpb.tot32 - (data_lba - base)) >> spc_shift;     /* cluster count */
    if (bpb.tot32 < 4085) { type = 12; eoc_min = 0xFF8; }
    else if (bpb.tot32 < 65525UL) { type = 16; eoc_min = 0xFFF8; }
    else { type = 32; eoc_min = 0x0FFFFFF8UL; }
    return type;
}

/* F.clus = next cluster of F.clus; returns 0 at end of chain / error */
static uint8_t next_clus(void)
{
    const uint8_t *p;
    uint16_t c = (uint16_t)F.clus, off, v;
    if (type == 16) {
        if (!(p = sec(fat_lba + (F.clus >> 8)))) return 0;
        F.clus = RD16(p + ((c & 255) << 1));
    } else if (type == 32) {
        if (!(p = sec(fat_lba + (F.clus >> 7)))) return 0;
        F.clus = rd32(p + ((c & 127) << 2)) & 0x0FFFFFFFUL;
    } else {                         /* FAT12: an entry may straddle two sectors */
        off = c + (c >> 1);
        if (!(p = sec(fat_lba + (off >> 9)))) return 0;
        v = p[off & 511];
        off++;
        if (!(p = sec(fat_lba + (off >> 9)))) return 0;
        v |= (uint16_t)p[off & 511] << 8;
        F.clus = (c & 1) ? v >> 4 : v & 0xFFF;
    }
    return F.clus >= 2 && F.clus < eoc_min;
}

/* pointer to the cached sector holding F.pos, 0 = EOF/error. Sector advances are lazy
 * (FAT_F_ADV): following the chain reads the FAT through the same cache, which must not
 * happen while a caller still uses a pointer into it. */
static const uint8_t *cur(void)
{
    if (!type) return 0;
    if (F.flags & FAT_F_ADV) {
        F.flags &= ~FAT_F_ADV;
        if (!(F.flags & FAT_F_ROOT16) && ++F.sic == spc) {
            F.sic = 0;
            if (!next_clus()) { F.size = F.pos; return 0; }
        }
    }
    if (F.pos >= F.size) return 0;
    if (F.flags & FAT_F_ROOT16) return sec(root_lba + (F.pos >> 9));
    return sec(data_lba + ((F.clus - 2) << spc_shift) + F.sic);
}

static void step(uint16_t n)
{
    F.pos += n;
    if (!((uint16_t)F.pos & 511)) F.flags |= FAT_F_ADV;
}

static uint16_t avail(void)          /* bytes left in the current sector, capped at EOF */
{
    uint16_t k = 512 - ((uint16_t)F.pos & 511);
    uint32_t left = F.size - F.pos;
    return left < k ? (uint16_t)left : k;
}

static void set(fat_file_t *f, uint32_t clus, uint32_t size)
{
    memset(&F, 0, sizeof F);
    F.start = F.clus = clus;
    F.size = size;
    if (!clus && type != 32) { F.flags = FAT_F_ROOT16; F.size = (uint32_t)root_secs << 9; }
    *f = F;
}

void fat_root(fat_file_t *d)
{
    set(d, type == 32 ? root_clus : 0, 0xFFFFFFFFUL);
}

void fat_open(fat_file_t *f, const fat_dirent_t *e)
{
    if (e->attr & FAT_ATTR_DIR) {
        if (e->clus) set(f, e->clus, 0xFFFFFFFFUL);
        else fat_root(f);                                  /* ".." of a first-level dir */
    } else set(f, e->clus, e->clus ? e->size : 0);
}

const uint8_t *fat_read_sector(fat_file_t *f, uint16_t *len)
{
    const uint8_t *p;
    F = *f;
    if ((p = cur())) {
        p += (uint16_t)F.pos & 511;
        step(*len = avail());
    }
    *f = F;
    return p;
}

uint16_t fat_read(fat_file_t *f, void *buf, uint16_t n)
{
    uint8_t *d = buf;
    uint16_t done = 0, k;
    const uint8_t *p;
    F = *f;
    while (n && (p = cur())) {
        k = avail();
        if (k > n) k = n;
        memcpy(d, p + ((uint16_t)F.pos & 511), k);
        d += k; n -= k; done += k;
        step(k);
    }
    *f = F;
    return done;
}

uint8_t fat_seek(fat_file_t *f, uint32_t pos)
{
    uint32_t s;
    uint8_t ok = 1;
    F = *f;
    if (pos > F.size) return 0;
    F.pos = pos;
    F.flags &= ~FAT_F_ADV;
    if (!(F.flags & FAT_F_ROOT16)) {
        s = pos >> 9;
        if (pos && !((uint16_t)pos & 511)) { s--; F.flags |= FAT_F_ADV; }  /* sector before pos */
        F.sic = (uint8_t)s & (spc - 1);
        F.clus = F.start;
        for (s >>= spc_shift; s && ok; s--) ok = next_clus();
    }
    *f = F;
    return ok;
}

static const uint8_t lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

uint8_t fat_readdir(fat_file_t *d, fat_dirent_t *e)
{
    const uint8_t *p;
    const uint8_t *q;
    char *l;
    uint8_t i, r = 0;
    F = *d;
    e->lfn[0] = 0;
    while ((p = cur())) {
        p += (uint16_t)F.pos & 511;
        if (!p[0]) break;                                      /* end of directory */
        step(32);
        if (p[0] == 0xE5 || (p[11] != 0x0F && (p[11] & 0x08))) {   /* deleted / volume label */
            e->lfn[0] = 0;
        } else if (p[11] == 0x0F) {                            /* long name part */
            if (p[0] & 0x40) memset(e->lfn, 0, sizeof e->lfn);
            i = (p[0] & 0x3F) - 1;
            if (i < 3) {
                l = e->lfn + i * 13;
                for (i = 0; i < 13 && l < e->lfn + FAT_LFN_MAX; i++, l++) {
                    q = p + lfn_off[i];
                    if (!q[0] && !q[1]) break;
                    *l = q[1] ? '?' : q[0];
                }
            }
        } else {
            memcpy(e->name, p, 11);
            e->attr = p[11];
            e->clus = RD16(p + 26);
            if (type == 32) ((uint16_t *)&e->clus)[1] = RD16(p + 20);
            e->size = rd32(p + 28);
            r = 1;
            break;
        }
    }
    *d = F;
    return r;
}

uint8_t fat_find(fat_file_t *d, const char *name11, fat_dirent_t *e)
{
    while (fat_readdir(d, e))
        if (!memcmp(e->name, name11, 11)) return 1;
    return 0;
}

/* first volume sector of a data cluster (module-side writers: modules/common/ftlw + cfgsave) */
uint32_t fat_clus_lba(uint32_t clus)
{
    return data_lba + ((clus - 2) << spc_shift);
}
