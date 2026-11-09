/* zos skins, module side (draft). A skin is the SKIN\ folder in the disk root, made at build time
 * by tools/rbskin.py from a Rockbox theme (zos/skins/<name>/rockbox):
 *   COLORS.ZT  RGB565 words: bg, fg, dim, sel_top, sel_bottom, sel_text, bar
 *   *.ZI       'Z' 'I' w h frames + big-endian RGB565 pixels (= LCD byte order: a draw is a copy)
 * Use: add a source file with  #include "../common/skin.c"  to the module, call skin_load() once.
 * Without a SKIN folder everything falls back to the zos theme colours (apps/theme.h).
 * Modules without room for skin.c (~1.5 KB) include "../common/skinlite.c" instead: same
 * skin_load() / skin_title() / skin_footer() calls, colours from the shared copy below (skc), a
 * plain title bar in the skin's bar colour instead of TITLE.ZI, no battery icon (~150-300 bytes).
 * Code written with the apps/theme.h names: include skcol.h after theme.h (C_BG ... = the skin). */
#ifndef ZOS_SKIN_H
#define ZOS_SKIN_H
#include <stdint.h>
#include "../../hal/hal.h"

typedef struct { uint16_t bg, fg, dim, sel_top, sel_bot, sel_text, bar; } skin_colors_t;
extern skin_colors_t sk;
extern uint8_t skin_ok;                          /* 1 = SKIN folder found */
#define SKIN_DEFAULTS { THEME_BG, THEME_FG, THEME_DIM, THEME_BAR, THEME_BAR, THEME_SEL, THEME_BAR } /* apps/theme.h */
#define SKIN_TITLE_H 16                          /* TITLE.ZI rows; content starts below */

/* the colours in HAL_SHARED (survive module chaining, cleared at boot): skin.c's skin_load() copies
 * sk here (SHELL does on every start), skinlite.c and MP3PLAY read them. ok = 0: not loaded yet
 * (SHELL.ZM missing: the core's fallback list), skinlite.c then uses SKIN_DEFAULTS. */
typedef struct { skin_colors_t c; uint8_t ok, spare; } skin_shared_t;
#define SKIN_SHARED (HAL_SHARED + 512 - 48)      /* below ZCFG (core/config.h) */
static __at(SKIN_SHARED) skin_shared_t skc;     /* absolute: no storage allocated */

/* Rockbox icon strip order (ICONS.ZI / ICONSEL.ZI frames) */
enum { ICON_AUDIO, ICON_FOLDER, ICON_PLAYLIST, ICON_CURSOR, ICON_WPS, ICON_FIRMWARE, ICON_FONT,
       ICON_LANGUAGE, ICON_CONFIG, ICON_PLUGIN, ICON_BOOKMARK, ICON_PRESET, ICON_QUEUED,
       ICON_MOVING, ICON_KEYBOARD, ICON_REVCURSOR, ICON_QUESTION, ICON_SETTING, ICON_FUNCTION,
       ICON_SUBMENU, ICON_SUBMENU_IN, ICON_RECORDING, ICON_VOICE, ICON_GENERAL, ICON_SYSTEM,
       ICON_PLAYBACK, ICON_DISPLAY, ICON_REMOTE, ICON_RADIO, ICON_FILEVIEW, ICON_EQ, ICON_ROCKBOX };

uint8_t skin_load(void);                                         /* colours; 0 = no skin */
uint8_t skin_draw(const char *name11, uint8_t x, uint8_t y, uint8_t frame);   /* 0 = missing */
void    skin_text(uint8_t x, uint8_t y, const char *s, uint16_t fg); /* transparent background */
void    skin_title(const char *title);                           /* title bar + centred title */
void    skin_footer(const char *text);                           /* bottom line in sk.dim */
/* scaled icon-strip frame; only with SKIN_ICON_BIG defined before including skin.c */
uint8_t skin_icon_big(const char *name11, uint8_t frame, uint8_t x, uint8_t y, uint8_t sc);
#endif
