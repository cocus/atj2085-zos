/* zos module loader: file -> RAM page, then run in place (see module.h) */
#include <stdint.h>
#include <string.h>
#include "../hal/hal.h"
#include "fat.h"
#include "module.h"

#ifndef ZOS_API_COUNT           /* set by build.py from core/api.txt */
#define ZOS_API_COUNT 0
#endif

const char *const mod_errstr[] = { "OK", "NOT FOUND", "READ ERROR", "NOT A MODULE", "TOO BIG", "NEWER API" };

uint8_t mod_load(const char *name12, uint8_t page, mod_hdr_t *hdr)
{
    fat_file_t f;
    fat_dirent_t e;
    xfer_t x;
    const uint8_t *p;
    uint16_t n, lim;
    if (!fat_mount()) return MOD_EREAD;
    fat_root(&f);
    if (!fat_find(&f, name12, &e) || (e.attr & FAT_ATTR_DIR)) return MOD_ENOENT;
    fat_open(&f, &e);
    if (fat_read(&f, hdr, sizeof *hdr) != sizeof *hdr) return MOD_EREAD;
    if (hdr->magic[0] != 'Z' || hdr->magic[1] != 'M' || (hdr->version & 0x7F) != MOD_VERSION ||
        hdr->size < sizeof *hdr || hdr->size > e.size) return MOD_EFORMAT;
    lim = (hdr->version & MOD_F_ZRAM2) ? HAL_ZRAM2_SIZE : PAGE_SIZE;
    if (hdr->size > lim || hdr->end > PAGE_BASE + lim || hdr->end < PAGE_BASE + hdr->size ||
        hdr->init < PAGE_BASE || hdr->main < PAGE_BASE) return MOD_EBIG;
    if (hdr->api > ZOS_API_COUNT) return MOD_EAPI;
    if (hdr->version & MOD_F_ZRAM2) page = HAL_ZRAM2_PAGE;
    fat_seek(&f, 0);
    f.size = hdr->size;
    x.dst = PAGE_BASE;
    while ((p = fat_read_sector(&f, &n))) {      /* p points into the FAT cache (ZRAM1) */
        x.src = p; x.len = n;
        page_write(page, &x);
        x.dst += n;
    }
    return f.pos == hdr->size ? MOD_OK : MOD_EREAD;
}

uint16_t mod_run(uint8_t page, const mod_hdr_t *hdr)
{
    if (hdr->version & MOD_F_ZRAM2) page = HAL_ZRAM2_PAGE;
    mod_call(page, hdr->init);
    return mod_call(page, hdr->main);
}
