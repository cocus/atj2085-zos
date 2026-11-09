/* zos FAT16 file creation on the user disk (module side; include fatw.c from one module source,
 * plus a .s file with .include "../common/ftlw.inc"). Used by FIRMWARE (backup) and REC.
 *
 * One file = one contiguous run of free clusters (the largest one on the volume, which bounds the
 * file size), written sector by sector straight through the NAND write path, then committed:
 *   fatw_open(buf, min)      read the BPB, scan FAT1 for the largest free run (>= min clusters)
 *   fatw_put(buf512)         next data sector of the run (fatw_nsec counts them)
 *   fatw_put_at(i, buf512)   rewrite data sector i (< fatw_nsec), e.g. a header written last
 *   fatw_commit(name, size)  data flush -> FAT1 chain -> FAT2 chain -> root entry (replaces an
 *                            existing entry of that name in place) -> flush -> old chain of the
 *                            replaced file freed in FAT1, FAT2 -> flush -> fat_mount()
 * Crash safety: until fatw_commit the FATs and the directory are untouched, so an interrupted
 * write leaves only data in clusters that are still free (the disk stays consistent, the file is
 * simply not there). A crash inside the commit can at worst leave lost clusters (allocated, not
 * referenced: the new chain before the entry, the old chain after it), never a cross-link.
 * Limits (choices): FAT16 superfloppy (this player's volume, nand_ftl.md 2.3), root directory
 * only, 8.3 name, fixed timestamp. buf (512 B) must be in ZRAM1 (< 0x4000): it is the NAND DMA
 * source and the hal_blk_read buffer. The core FAT cache is dropped by fat_mount() at the end. */
#ifndef ZOS_FATW_H
#define ZOS_FATW_H
#include <stdint.h>

enum { FATW_OK, FATW_EFS = 0x10, FATW_EFULL, FATW_EDIR, FATW_EREAD };   /* or an FTLW_* code */

extern uint16_t fatw_c0, fatw_n;     /* run: first cluster, clusters */
extern uint32_t fatw_nsec;           /* data sectors written */
extern uint8_t  fatw_spc;            /* sectors per cluster */

uint8_t  fatw_open(uint8_t *buf512, uint16_t min_clusters);
/* the same run again in another module (REC -> RECRUN -> REC): BPB only, no scan */
uint8_t  fatw_resume(uint8_t *buf512, uint16_t c0, uint16_t n, uint32_t nsec);
uint32_t fatw_lba0(void);            /* volume sector of the run's first sector */
uint32_t fatw_room(void);            /* data sectors left in the run */
uint8_t  fatw_put(const void *buf512);
uint8_t  fatw_put_at(uint32_t i, const void *buf512);
uint8_t  fatw_commit(const char *name11, uint32_t size);
/* first free "PFXnnn" number in the root (name11 = 3-char prefix + 3 digits + ext): fills the
 * digits of name11 in place; 0 = none left */
uint8_t  fatw_next_name(char *name11);
#endif
