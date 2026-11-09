/* zos user settings: 16 bytes in HAL_SHARED (survive module chaining). SHELL fills them on its first
 * run after boot from ZOS.CFG in the disk root (a fixed 512-byte text file the build creates);
 * modules/common/cfgsave.c writes them back into that file in place (NAND write path).
 * SETTINGS edits them; MUSIC/MP3PLAY keep the resume point (last file + second) and the volume. */
#ifndef ZOS_CONFIG_H
#define ZOS_CONFIG_H
#include <stdint.h>
#include "../hal/hal.h"

enum { LANG_EN, LANG_ES, NLANG };
enum { BL_ALWAYS, BL_10S, BL_30S, BL_60S, NBL };

typedef struct {
    uint8_t  magic;
    uint8_t  lang;         /* LANG_* */
    uint8_t  contrast;     /* 0..CONTRAST_MAX */
    uint8_t  backlight;    /* BL_* timeout */
    uint8_t  flags;        /* ZF_*: on/off settings */
    uint8_t  vol;          /* player volume 0..31 */
    uint32_t res_dir;      /* resume: start cluster of the folder (0 = root) */
    uint32_t res_file;     /*   first cluster of the file, 0 = nothing to resume */
    uint16_t res_pos;      /*   file position in 16 KB units (RES_POS_SHIFT) */
} zcfg_t;

#define ZCFG_ADDR    (HAL_SHARED + 512 - 32)    /* below SHELL_STATE (last 16 bytes); skin colours below
                                                * (modules/common/skin.h SKIN_SHARED, 16 bytes) */
#define ZCFG         ((zcfg_t *)ZCFG_ADDR)
#define ZCFG_MAGIC   0xC8
#define CONTRAST_MAX 38               /* LCD electronic volume steps (power.md), stock default 6 */
#define CONTRAST_DEF 6
#define VOL_DEF      20
#define ZCFG_LANG    (ZCFG->lang == LANG_ES ? LANG_ES : LANG_EN)   /* garbage-safe */
#define RES_POS_SHIFT 14
/* ZCFG->flags */
#define ZF_BATT   0x01       /* battery icon in the title bar */
#define ZF_BEEP   0x02       /* key beep (modules/common/beep.c) */
#define ZF_FMUSB  0x04       /* FM player: USB audio out on by default */
#define ZCFG_OK   (ZCFG->magic == ZCFG_MAGIC)
#define ZCFG_BATT   (ZCFG_OK && (ZCFG->flags & ZF_BATT))
#define ZCFG_BEEP   (ZCFG_OK && (ZCFG->flags & ZF_BEEP))
#define ZCFG_FM_USB (ZCFG_OK && (ZCFG->flags & ZF_FMUSB))
#define ZCFG_FILE    "ZOS     CFG"
#define ZCFG_FILE_SIZE 512
typedef char zcfg_fits[sizeof(zcfg_t) <= 16 ? 1 : -1];
#endif
