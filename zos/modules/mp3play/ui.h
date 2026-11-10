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
extern char big[12];                             /* what is on screen: time digits, then kb */
#define kb (big + 8)                             /* kbps[3] + rate code (as mp3.h: adjacent) */
extern uint16_t tot_shown;
extern uint8_t bar_w, vol_shown, paused_shown, mode_shown;
void txt(uint8_t x, uint8_t y, const char *s, uint16_t fg);   /* scale 1 on the background */
void fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c);
#define PB_Y 89                                  /* progress bar (BAR_H rows, music/shared.h) */
#define VB_Y 102                                 /* volume bar */
void bar(uint8_t x, uint8_t y, uint8_t w, const uint16_t *rows);   /* one colour per row */
void batt(void);                                 /* the title bar's battery icon (EV_BATT) */
void tfmt(char *t, uint16_t s, uint8_t hrs);     /* "mm:ss" / "h:mm:ss" */
void footer(void);
void fmt_line(void);                             /* "44.1K 128K" next to the speaker icon */
void screen(void);
void scroll(void);                               /* call often: scrolls long title/artist */
void loading(void);                              /* "LOADING" over the time */
void track_info(void);                           /* main.c */
#endif
