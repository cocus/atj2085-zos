/* zos block device: zos' own NAND + FTL reader, no stock OS involved (writes: modules/common/ftlw.c).
 * On-flash format = the stock "641" FTL (lisong_rebuild/analysis/nand_ftl.md 2.x):
 *  - unit = 2 physical blocks (two planes) = 128 superpages x 2 KB = 512 sectors; zone = 2048 units
 *    of which 0x7B0 hold logical units; superpage sp of unit u is block 2u + (sp & 1), page sp >> 1.
 *  - per zone a mapping table unit (spare byte 1 = 0xBB, plane-0 block only): copy k = pages 2k/2k+1,
 *    2048 u16: [L] = physical unit of logical unit L (0xFFFF unmapped), [0x7FF] = 0x55AA.
 *  - UD (user disk driver) sector S -> logical unit 0x40 + (S >> 9), sector S & 511;
 *    UD sectors 0..511 = stock settings (VM), the FAT disk starts at UD sector 0x200.
 * Data writes by the stock driver end up back in the same physical unit (copy-back merge), so the
 * table only changes when units are remapped; it is read once at mount. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"


#define UNITS_PER_ZONE 2048
#define LUNITS         0x7B0
#define PAGES_PER_BLK  64

#define FW_UNITS       0x40
#define MAXZONES       4

/* DSP RAM lanes (device-measured, see attic/memprobe.c). 0xC0 is left out until write-tested. */
const uint8_t hal_pages[] = { 0xC1, 0xC2, 0x34, 0x35, 0x36 };
const uint8_t hal_npages = sizeof hal_pages;

static nand_req_t q;
static uint8_t nzones, ready;
static uint16_t tbl_unit[MAXZONES];           /* zone-relative unit of the table */
static uint8_t tbl_copy[MAXZONES];            /* newest copy k */
static uint16_t c_lu = 0xFFFF, c_pu;          /* one-entry map cache (global logical -> global phys) */

/* read `len` bytes at column `col` of page `pg` (0..63) of physical block `blk` */
static void rd(uint16_t blk, uint8_t pg, uint16_t col, void *buf, uint16_t len)
{
    /* row = blk * 64 + pg, as 3 address bytes, without 32-bit arithmetic */
    q.c0 = (uint8_t)col; q.c1 = (uint8_t)(col >> 8);
    q.r0 = (uint8_t)((uint8_t)blk << 6) | pg;
    q.r1 = (uint8_t)(blk >> 2);
    q.r2 = (uint8_t)(blk >> 10);
    q.buf = (uint16_t)buf; q.len = len;
    nand_read(&q);
}

static uint16_t rd16(uint16_t blk, uint8_t pg, uint16_t col)
{
    uint16_t v;
    rd(blk, pg, col, &v, 2);
    return v;
}

/* table entry e (0..2047) of zone z */
static uint16_t tbl(uint8_t z, uint16_t e)
{
    uint16_t blk = (z * UNITS_PER_ZONE + tbl_unit[z]) * 2;
    return rd16(blk, tbl_copy[z] * 2 + (e >> 10), (e & 1023) * 2);
}

/* newest valid copy in the table unit (pages 2k/2k+1, spare byte 1 = 0xBB, [0x7FF] = 0x55AA) */
static uint8_t find_copy(uint8_t z)
{
    uint16_t blk = (z * UNITS_PER_ZONE + tbl_unit[z]) * 2;
    uint8_t k, best = 0xFF, sp[2];
    for (k = 0; k < PAGES_PER_BLK / 2; k++) {
        rd(blk, k * 2, 0x800, sp, 2);
        if (sp[0] != 0xFF || sp[1] != 0xBB) break;
        if (rd16(blk, k * 2 + 1, 0x7FE) == 0x55AA) best = k;
    }
    return best;
}

/* spare scan of a zone for its table unit (bare metal; ~2048 short reads) */
static uint8_t scan_zone(uint8_t z)
{
    uint16_t u;
    uint8_t sp[2];
    for (u = 0; u < UNITS_PER_ZONE; u++) {
        rd((z * UNITS_PER_ZONE + u) * 2, 0, 0x800, sp, 2);
        if (sp[0] == 0xFF && sp[1] == 0xBB) { tbl_unit[z] = u; return 1; }
        if (!(u & 63)) hal_watchdog();
    }
    return 0;
}

uint8_t hal_blk_init(void)
{
    uint8_t z, k;
    uint16_t blocks;
    if (ready) return 1;
    /* zones: the stock flash-detect result (RAM 0x008A = CE1 size in 2-block units) when hosted,
     * else this board's chip (1 GB, 2 zones). TODO bare metal: READ ID. */
    blocks = *(volatile uint16_t *)0x008A;
    nzones = (blocks && blocks <= 0x4000) ? (uint8_t)((blocks + 4095) >> 12) : 2;
    if (nzones > MAXZONES) nzones = MAXZONES;
    for (z = 0; z < nzones; z++) {
        /* hosted: BREC's descriptor at RAM 0x3C80 + 2z = (copy << 12) | table unit, verified below */
        uint16_t d = *(volatile uint16_t *)(0x3C80 + 2 * z);
        tbl_unit[z] = d & 0x0FFF;
        tbl_copy[z] = d >> 12;
        if (tbl_unit[z] < UNITS_PER_ZONE && tbl(z, 0x7FF) == 0x55AA) continue;
        if (!scan_zone(z) || (k = find_copy(z)) == 0xFF) return 0;
        tbl_copy[z] = k;
    }
    c_lu = 0xFFFF;
    ready = 1;
    return 1;
}

static uint8_t ftl_read(uint16_t lu, uint16_t s, void *buf512);

/* for the module-side write path (modules/common/ftlw.c): mapping table entry e of zone z
 * (zone-relative unit; [0..0x7AF] = logical units, [0x7FD] = temp unit), 0xFFFF = none / no zone */
uint16_t ftl_tbl(uint8_t z, uint16_t e)
{
    if ((!ready && !hal_blk_init()) || z >= nzones) return 0xFFFF;
    return tbl(z, e);
}

uint8_t hal_blk_read(uint32_t lba, void *buf512)
{
    /* UD sector S = lba + 0x200: unit = 0x40 + 1 + lba / 512, sector in unit = lba % 512 */
    uint16_t lo = (uint16_t)lba, hi = ((uint16_t *)&lba)[1];
    return ftl_read(FW_UNITS + 1 + ((lo >> 9) | (hi << 7)), lo & 511, buf512);
}

/* firmware area ("system disk"): sector n of FWIMAGE.FW = logical unit n >> 9 (zone 0),
 * sector n & 511 (nand_ftl.md 2.3). Sector 0 = header, 0x200.. = 32-byte directory entries. */
uint8_t hal_fw_read(uint16_t n, void *buf512)
{
    if (n >= FW_UNITS * 512U) return 0;
    return ftl_read(n >> 9, n & 511, buf512);
}

static uint8_t ftl_read(uint16_t lu, uint16_t s, void *buf512)
{
    uint16_t L;
    uint8_t z, sp;
    if (!ready && !hal_blk_init()) return 0;
    if (lu != c_lu) {
        for (z = 0, L = lu; L >= LUNITS; L -= LUNITS) z++;
        if (z >= nzones) return 0;
        c_pu = tbl(z, L);
        if (c_pu != 0xFFFF) c_pu += z * UNITS_PER_ZONE;
        c_lu = lu;
    }
    if (c_pu == 0xFFFF) { memset(buf512, 0, 512); return 1; }   /* never written */
    sp = s >> 2;
    rd(c_pu * 2 + (sp & 1), sp >> 1, (s & 3) * 512, buf512, 512);
    return 1;
}
