#ifndef ZOS_THEME_H
#define ZOS_THEME_H
#include "../ui/gfx.h"
/* the zos colours = the no-skin defaults (modules/common/skin.h SKIN_DEFAULTS); the THEME_ names
 * stay valid where modules/common/skcol.h maps C_BG ... to the skin */
#define THEME_BG  RGB(8, 12, 32)
#define THEME_FG  RGB(225, 230, 240)
#define THEME_DIM RGB(120, 130, 160)
#define THEME_BAR RGB(20, 60, 140)
#define THEME_SEL RGB(255, 196, 0)
#define C_BG   THEME_BG
#define C_FG   THEME_FG
#define C_DIM  THEME_DIM
#define C_BAR  THEME_BAR
#define C_SEL  THEME_SEL
#define C_OK   RGB(80, 230, 120)
void ui_header(const char *title);
void ui_footer(const char *text);
#endif
