/* zos NAND/FTL WRITE path (module side), compatible with the stock "641" FTL.
 * Use: a module adds one .s file containing  .include "../common/ftlw.inc"  (the implementation,
 * in assembler for size) and includes this header.
 * Spec: lisong_rebuild/analysis/nand_ftl.md 2.x (format) and 3 (stock write algorithm).
 *
 * Algorithm = the stock UD_SF641 one: the first write to a logical unit "opens" it; sectors are
 * programmed into the zone's temp unit (table[0x7FD]) at the SAME sector offsets, superpages in
 * ascending order, skipped superpages copied from the old unit (copy-back). A write to another unit,
 * a write to an earlier superpage or a rewrite of a sector already written closes the unit: copy the
 * rest of the old unit into the temp unit, erase the old unit, copy the temp unit back into it,
 * erase the temp unit. The logical->physical map never changes, so the mapping table, the core's
 * map cache (blk.c c_lu/c_pu) and the stock mount scan stay valid.
 *
 * Coherence (choice): the core read path (hal_blk_read) always reads the OLD physical unit, so a
 * caller must call ftlw_sync(lba, n) before reading sectors that may lie in the open unit (it closes
 * the unit when the range touches it), and ftlw_flush() before leaving the module / on idle.
 * The core FAT layer's sector cache is dropped by fat_mount(), which every module load does.
 *
 * Buffers: buf512 must be in ZRAM1 (< 0x4000): DMA1 source, as the stock driver uses it.
 * Interrupts are off while CE1 is selected (one sector program / copy-back / erase at a time).
 * Run the CPU at 24 MHz (NAND timing at 48 MHz is unverified on the device). */
#ifndef ZOS_FTLW_H
#define ZOS_FTLW_H
#include <stdint.h>

enum {
    FTLW_OK,
    FTLW_ERANGE,        /* LBA beyond the user disk (nothing done) */
    FTLW_EMAP,          /* table entry / unit sanity check failed (nothing done) */
    FTLW_EDIRTY,        /* temp unit not erased (nothing done) */
    FTLW_EFAIL,         /* NAND reported program/erase failure (sticky) */
    FTLW_ETIMEOUT,      /* NAND stayed busy (sticky) */
    FTLW_EWP            /* NAND write protected (sticky) */
};

/* lba = FAT volume sector, the numbering of hal_blk_read (UD sector 0x200 + lba). 0 = ok.
 * After a sticky error every call returns it until ftlw_reset(). */
uint8_t ftlw_write(uint32_t lba, const void *buf512);
uint8_t ftlw_flush(void);                     /* close the open unit (no-op if none) */
uint8_t ftlw_sync(uint32_t lba, uint16_t n);  /* flush if [lba, lba+n) touches the open unit */
uint8_t ftlw_pending(void);                   /* 1 = a unit is open (data not yet in place) */
uint8_t ftlw_error(void);                     /* sticky error code, 0 = none */
void    ftlw_reset(void);                     /* clear the sticky error */
#endif
