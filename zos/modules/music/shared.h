/* MUSIC (browser, page module) <-> MP3PLAY / DECPLAY (players, ZRAM2 modules) hand-over block at HAL_SHARED.
 * MP3DEC needs every DSP RAM lane, so the browser cannot stay resident while playing: it writes the
 * playlist + its own browse state here and chains to MP3PLAY, which chains back on MENU. */
#ifndef MUSIC_SHARED_H
#define MUSIC_SHARED_H
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"

#define MUS_MAGIC  0xA5
#define MUS_TRACKS 13
#define MUS_NAMEW  15
typedef struct { uint32_t clus, size; uint8_t idx; char name[MUS_NAMEW + 1]; } mus_trk_t;/* idx = browser row (< 64) | format: MP3 -> MP3PLAY.ZM (MP3DEC), OGG/WAV -> DECPLAY.ZM (OGGDEC/PCMDEC).
 * A player meeting a track of the other player's format (next/previous/end of track) chains to it. */
#define TRK_IDX    0x3F
#define TRK_KIND   0xC0
#define TRK_MP3    0x00
#define TRK_OGG    0x40
#define TRK_WAV    0x80
#define MP3PLAY_ZM "MP3PLAY ZM "
#define DECPLAY_ZM "DECPLAY ZM "
/* player chrome: MUSIC draws the skinned title bar (TITLE.ZI, "NOW PLAYING", battery) before it chains
 * to a player; the players never touch rows 0..SKIN_TITLE_H-1 (only the battery icon). The progress
 * and volume bars are drawn row by row in these colours (MUSIC: one column of PROGBAR.ZI / VOLBAR.ZI,
 * the empty part darkened; without a skin flat theme colours, bg rows = a thinner bar) */
#define BAR_H 7
enum { BAR_PON, BAR_POFF, BAR_VON, BAR_VOFF, BAR_VSEL, BAR_N };
typedef struct {
    char       next[12];           /* MOD_CHAIN_NAME: must be first */
    uint8_t    magic;
    uint8_t    depth, sel, top;    /* browser state */
    uint32_t   dclus[6];           /* folder stack: first cluster of each ([0] = root, unused) */
    uint16_t   bar[BAR_N][BAR_H];  /* progress on/off, volume on/off/volume-mode, per row */
    uint8_t    ntrk, cur, vol;     /* playlist */
    uint16_t   start_pos;          /* first track: start at this file position (RES_POS_SHIFT units) */
    uint8_t    from_player;        /* set by MP3PLAY when it chains back (MUSIC then saves ZOS.CFG) */
    uint8_t    start_paused;       /* MP3PLAY shows the first track paused (resume from the shell) */
    mus_trk_t  trk[MUS_TRACKS];
    void      *arg;                /* DECPLAY -> its helper DECINFO.ZM: request (decplay/dec.h dec_info_t) */
} mus_shared_t;
static __at(HAL_SHARED) mus_shared_t mus_sh;   /* absolute: no storage allocated */
#define MUS (&mus_sh)
/* the last 48 bytes of HAL_SHARED: skin colours (common/skin.h), zos settings (core/config.h),
 * SHELL/FM state */
typedef char mus_shared_fits[sizeof(mus_shared_t) <= 512 - 48 ? 1 : -1];
#endif
