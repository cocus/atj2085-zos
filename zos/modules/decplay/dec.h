/* DECPLAY internals: the stock OGGDEC.DSP (Ogg Vorbis) and PCMDEC.DSP (WAV: IMA ADPCM and 16-bit PCM)
 * driven over the DSP host mailbox. Both are the Actions "decXXX" codec family (v04 MUSIC.AP bank 0x1C
 * for OGG, VOICE.AP bank 0x09 for PCM); protocol and addresses: lisong_rebuild/analysis/ogg_rec_rds.md
 * sections 1.2 and 2.2, checked against the disassembly (disasm_v04/MUSIC_AP/seg_bank1c.s,
 * VOICE_AP/seg_bank09.s). The player front end (screen, keys, playlist) is MP3PLAY's main.c/ui.c; this
 * module implements the mp3.h backend API (mp3_start, mp3_service, ...) for these two programs. */
#ifndef DECPLAY_DEC_H
#define DECPLAY_DEC_H
#include <stdint.h>
#include "../../core/fat.h"
#include "../mp3play/mp3.h"

/* requests (DSP -> MCU) beyond mp3.h, and replies */
enum { REQ_TABLE = 0x2A, REQ_2C = 0x2C };
enum { REP_TABLE = 0x3A, REP_2C = 0x3C, REP_FADED = 0xE1 };
#define END_OK          0x30            /* REQ_END HDR4: normal end; 0x10 / 0x2x = error, bad format */

/* OGGDEC data RAM mailbox (IDM word addresses as seen in the 0x4000 window, lanes L/M/H) */
#define OGG_DM_ARG      0x40AF          /* L/M/H 24 bit: REQ_START <- file sectors (play) or resume
                                           sector; REQ_DATA -> sector step (direction in HDR4) */
#define OGG_DM_TIME     0x40B3          /* L/M/H = seconds, minutes, hours (REQ_STATUS) */
#define OGG_DM_VOL      0x40B5          /* L: volume 0..31 */
#define OGG_DM_EQ       0x40B6          /* L: EQ (0 = off) */
#define OGG_TABLE_PM    0x1A49          /* REQ_TABLE: PM words 0x1A49..0x2BE0 back from OGGDEC.DSP */
#define OGG_TABLE_N     0x1198
/* PCMDEC data RAM mailbox (words 0x1262..0x127B, mostly lane L) */
#define PCM_DM_TIME     0x5264          /* L, 3 words: seconds, minutes, hours (REQ_STATUS) */
#define PCM_DM_STEP     0x5267          /* H, 3 words: REQ_DATA sector step, LE (direction in HDR4) */
#define PCM_DM_EQ       0x526F          /* L, 11 words: EQ flag + 10 bands (stock: zeros) */
#define PCM_DM_VOL      0x527A          /* L: volume */
#define PCM_DM_SPEED    0x527B          /* L: play speed (0 = normal) */
#define PCM_DM_RESUME   0x4100          /* H, 4 words: REQ_START mode 1: resume sector, LE */
#define DIR_FWD         0x11            /* REQ_DATA HDR4 */
#define MODE_RESUME     0x01            /* REQ_START reply HDR4 */

/* file probe + tags: done by the page module DECINFO.ZM (modules/decinfo), run in a DSP lane page
 * while the DSP is stopped (ZRAM2 + _CODE2 have no room for it); request/answer via MUS->arg */
typedef struct {
    const fat_dirent_t *e;              /* in: the file */
    char    *title, *artist;            /* out: ID3_W + 1 each ("" = none: the player shows the name) */
    uint8_t  ok;                        /* out: 0 = not playable (format, rate, channels) */
    uint8_t  ogg;                       /*   1 = Ogg Vorbis (OGGDEC), 0 = WAV (PCMDEC) */
    uint8_t  mode;                      /*   MP3_MODE_MONO or 0 */
    char     kbps[3];                   /*   3 ASCII digits */
    uint16_t total_s;                   /*   0 = unknown */
    uint16_t bps;                       /*   average bytes per second >> shift (0 = no seeking) */
    uint8_t  shift;
} dec_info_t;
#define DECINFO_ZM "DECINFO ZM "
extern dec_info_t dec_info;

/* bitstream DMA (decdma.s): one 512-byte sector per REQ_DATA, per program */
typedef struct { uint16_t da; uint8_t ida, mode, page; } dec_dma_t;
extern dec_dma_t dec_dmad;
void dec_dma(const uint8_t *buf512);
uint16_t dec_udiv(uint32_t n, uint16_t d);   /* decdma.s: n / d, needs n >> 16 < d */

extern uint8_t  dec_ogg;                /* 1 = OGGDEC, 0 = PCMDEC */
extern uint8_t  dec_bs[512];            /* bitstream sector (DMA source, ZRAM1) */
extern uint32_t dec_arg;                /* REQ_START argument: file sectors / resume sector */
extern uint8_t  dec_mode;               /* MODE_PLAY / MODE_RESUME */
extern uint32_t dec_last;               /* last sector sent to the DSP (0xFFFFFFFF = none yet) */
extern uint32_t dec_pf;                 /* sector held in dec_bs when have */
extern uint8_t  dec_have, dec_pend, dec_first_bb;
extern uint16_t dec_eof;                /* hal_ticks() of a failed read (end of file), 0 = none */
extern uint16_t dec_pm_sec[3];          /* OGGDEC.DSP PMLS/PMMS/PMHS start sectors (table restore) */
extern fat_file_t dec_dspf;             /* OGGDEC.DSP, opened (table restore) */

void dec_wr8(uint8_t pg, uint16_t a, uint8_t v);
uint8_t dec_rd8(uint8_t pg, uint16_t a);
void dec_slow(void);
void dec_fast(void);
void dec_wait(uint8_t n);
void dec_pll(uint8_t v);
void dec_table(void);                   /* deccold.c: REQ_TABLE */
void dec_req_start(void);               /* deccold.c: REQ_START */
void dec_req_format(void);              /* deccold.c: REQ_FORMAT */
uint8_t dec_read_sec(uint32_t s);      /* decio.c: sector s into dec_bs (prefetch slot), 0 = EOF */
void dec_poll(void);                    /* main loop: end of file without REQ_END */
#define PLAYER_POLL dec_poll
#endif
