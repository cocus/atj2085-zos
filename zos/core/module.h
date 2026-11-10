/* zos modules: position-fixed images linked at PAGE_BASE, loaded into a free RAM page and run in
 * place. A module calls the core only through the API jump table (core/api.txt, at 0x0803), so
 * modules keep working across core rebuilds as long as the table is only appended to. */
#ifndef ZOS_MODULE_H
#define ZOS_MODULE_H
#include <stdint.h>

#define MOD_VERSION 1
#define MOD_CHAIN   0xC4A1       /* mod_main() result: run the module named at MOD_CHAIN_NAME next */
#define MOD_CHAIN_NAME ((const char *)HAL_SHARED)
#define MOD_REBOOT  0xB007       /* mod_main() result (SHELL): leave zos (reboot into the stock firmware) */
#define SHELL_STATE (HAL_SHARED + 512 - 16)   /* 16 bytes kept for the SHELL module (uses 0-7) */
#define FM_STATE    (SHELL_STATE + 8)         /* 7 bytes kept for the FM module (the last: HAL_BATT) */
#define MOD_POWER_REQ (*(volatile uint8_t *)(SHELL_STATE + 7))  /* a module left because of EV_POWER:
                                                SHELL powers off when it starts (core/event.h) */
#define MOD_F_ZRAM2 0x80         /* version bit 7: code in the ZRAM2 page (HAL_ZRAM2_*), variables in the
                                    fixed ZRAM1 block HAL_MODRAM_* - for modules that must run while
                                    the DSP owns every DSP RAM lane (MP3 playback) */
typedef struct {
    char     magic[2];      /* "ZM" */
    uint8_t  version;       /* MOD_VERSION */
    uint8_t  api;           /* number of API entries the module was linked against */
    uint16_t size;          /* image bytes in the file (from PAGE_BASE) */
    uint16_t end;           /* highest address used + 1 (code, data, bss) */
    uint16_t init;          /* C runtime init (data/bss) */
    uint16_t main;          /* uint16_t mod_main(void) */
    char     name[16];      /* display name, NUL padded */
} mod_hdr_t;

enum { MOD_OK, MOD_ENOENT, MOD_EREAD, MOD_EFORMAT, MOD_EBIG, MOD_EAPI };
extern const char *const mod_errstr[];

/* load file `name12` into `page`; fills *hdr on success */
uint8_t  mod_load(const char *name12, uint8_t page, mod_hdr_t *hdr);
/* run a loaded module: init + main, returns main's result */
uint16_t mod_run(uint8_t page, const mod_hdr_t *hdr);
#endif
