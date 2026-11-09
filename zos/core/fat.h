/* zos FAT12/16/32 (read-only for now) on hal_blk_read(). Portable: no target code in here. */
#ifndef ZOS_FAT_H
#define ZOS_FAT_H
#include <stdint.h>

#define FAT_LFN_MAX 39               /* long names are truncated to this (ASCII; others -> '?') */
#define FAT_ATTR_DIR 0x10

typedef struct {
    uint32_t start, clus;            /* first / current cluster (0 + FAT_F_ROOT16 = fixed root) */
    uint32_t size, pos;              /* directories: size = 0xFFFFFFFF */
    uint8_t  sic, flags;             /* sector in cluster */
} fat_file_t;
#define FAT_F_ROOT16 1
#define FAT_F_ADV    2               /* internal: pos is on a sector boundary not yet entered */

typedef struct {
    char     name[11];               /* 8.3, space padded, as on disk */
    uint8_t  attr;
    uint32_t clus, size;
    char     lfn[FAT_LFN_MAX + 1];   /* long name, or "" */
} fat_dirent_t;

uint8_t  fat_mount(void);                                    /* 0 = no FAT volume */
void     fat_root(fat_file_t *d);
uint8_t  fat_readdir(fat_file_t *d, fat_dirent_t *e);        /* 0 = end; skips volume labels, "." ".." stay */
void     fat_open(fat_file_t *f, const fat_dirent_t *e);
uint16_t fat_read(fat_file_t *f, void *buf, uint16_t n);     /* bytes read; buf may be in a page */
const uint8_t *fat_read_sector(fat_file_t *f, uint16_t *len);/* next <=512 bytes in place (cache), 0 = EOF */
uint8_t  fat_seek(fat_file_t *f, uint32_t pos);              /* 0 = failed */
uint8_t  fat_find(fat_file_t *d, const char *name11, fat_dirent_t *e);
uint32_t fat_clus_lba(uint32_t clus);                        /* first sector of a data cluster */
#endif
