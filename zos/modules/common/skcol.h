/* the apps/theme.h colour names -> the current skin (skin.h: skc, valid after skin_load() with skin.c
 * or skinlite.c), for module code written with the theme names. Include after apps/theme.h.
 * C_SEL (highlight / warning) and C_OK (good value) stay the theme's accent colours; selected list
 * rows: C_SELBG background, C_SELFG text (the skin's selection colours). */
#ifndef ZOS_SKCOL_H
#define ZOS_SKCOL_H
#include "skin.h"
#undef C_BG
#undef C_FG
#undef C_DIM
#undef C_BAR
#define C_BG    (skc.c.bg)
#define C_FG    (skc.c.fg)
#define C_DIM   (skc.c.dim)
#define C_BAR   (skc.c.bar)
#define C_SELBG (skc.c.sel_bot)
#define C_SELFG (skc.c.sel_text)
#endif
