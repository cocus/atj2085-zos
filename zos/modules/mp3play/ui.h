/* MP3PLAY screen state shared by main.c (update, keys) and ui.c (static screen, _CODE2) */
#ifndef MP3PLAY_UI_H
#define MP3PLAY_UI_H
#include <stdint.h>
#include "mp3.h"
/* skin colours (HAL_SHARED copy made by skin_load() in SHELL/MUSIC, common/skin.h) */
#include "../common/skin.h"
#undef C_BG
#undef C_FG
#undef C_DIM
#undef C_SEL
#undef C_BAR
#undef C_OK
#define C_BG  (skc.c.bg)
#define C_FG  (skc.c.fg)
#define C_DIM (skc.c.dim)
#define C_OK  (skc.c.sel_top)
#define C_SEL (skc.c.sel_bot)
#define C_BAR (skc.c.bar)
#define S MUS

#define SCR_W     20                             /* title/artist chars on screen */
#define SCR_TICKS 30                             /* scroll step: 300 ms */
#define SCR_PAUSE 5                              /* steps to wait at both ends */
extern char ti[ID3_W + 1], ar[ID3_W + 1];       /* title / artist (or file name) */
extern uint8_t volmode, scrub;                   /* volume mode; << >> held: scrubbing to target */
extern uint16_t target;
extern char big[8], kb[3];                       /* what is on screen */
extern uint16_t tot_shown, req_shown;
extern uint8_t bar_w, vol_shown, paused_shown, mode_shown;
void txt(uint8_t x, uint8_t y, const char *s, uint16_t fg);   /* scale 1 on the background */
void fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c);
void tfmt(char *t, uint16_t s, uint8_t hrs);     /* "mm:ss" / "h:mm:ss" */
void footer(void);
void screen(void);
void scroll(void);                               /* call often: scrolls long title/artist */
void loading(void);                              /* "LOADING" over the time */
void track_info(void);                           /* main.c */
#endif
