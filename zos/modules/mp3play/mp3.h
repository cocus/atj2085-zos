#ifndef MUSIC_MP3_H
#define MUSIC_MP3_H
#include <stdint.h>
#include "../../core/fat.h"

/* #define MP3_STEP 1  device bring-up: stop before each start stage, show request counters */
/* #define MP3_DIAG 1      TEMPORARY: breadcrumbs on screen (phase letter + request code) to find a
                       device-only reboot during playback; the screen freezes on the last one */
void mp3_diag(uint8_t slot, char c);   /* main.c */

/* ---- MP3DEC.DSP mailbox protocol (lisong_rebuild/analysis/mp3_driver_spec.md 4.2) ---------- */
enum {                                  /* requests (DSP -> MCU, port HDR5) */
    REQ_START   = 0x11,                 /*   handshake; HDR4 <- play mode */
    REQ_DATA    = 0xEE,                 /*   512 bytes of bitstream wanted */
    REQ_FORMAT  = 0xCC,                 /*   first frame decoded: rate, bitrate, total time */
    REQ_STATUS  = 0xBB,                 /*   periodic: current time */
    REQ_END     = 0xAA,                 /*   end of stream */
    REQ_FADED   = 0xE0,                 /*   fade-out done */
    REQ_SENT    = 0x55,                 /*   sentence point */
    REQ_CHIP    = 0xA8,                 /*   chip check: HDR4 <- DSP version port */
    REQ_VERDICT = 0xA9,
    REQ_MIPS    = 0x77,                 /*   more DSP clock wanted */
    REQ_OVERLAY = 0x2B
};
enum {                                  /* replies (MCU -> DSP, port HDR5) */
    REP_START = 0x22, REP_DATA = 0xDD, REP_FORMAT = 0x99, REP_STATUS = 0x00, REP_SENT = 0x66,
    REP_CHIP = 0x95, REP_VERDICT = 0x96, REP_MIPS = 0x88, REP_OVERLAY = 0x3B
};
#define MODE_PLAY       0x00            /* REQ_START reply data: play from the start */
#define DATA_OK         0x00            /* REQ_DATA reply HDR0 */
#define DATA_END        0x03            /*   no more data: the DSP drains, then REQ_END */
#define STATUS_RUNNING  0x01            /* REQ_STATUS reply HDR4: 0 = once after a pause */
/* DSP data RAM variables (IDM word addresses as seen in the 0x4000 window, lanes 0x24/25/26) */
#define DM_TOTAL_S      0x5C01          /* L: total seconds (valid at REQ_FORMAT) */
#define DM_TOTAL_M      0x5C02
#define DM_TOTAL_H      0x5D52
#define DM_FILELEN      0x5C03          /* L/M/H: file length bytes 0-2 */
#define DM_FILELEN3     0x5C04          /* L: file length byte 3 */
#define DM_VOLUME       0x5C07
#define DM_GATE_RMS     0x5D4D          /* L/M/H 0x00202F (stock default) */
#define DM_GATE_TIME    0x5D4C          /* L/M/H 0x000150 */
#define DM_NOW_H        0x5D57          /* L: current time h, min, s */
#define DM_NOW_M        0x5D58
#define DM_NOW_S        0x5D59
#define DM_DACFIFO      0x7FED          /* PG p.52: the DSP writes PCM to the DAC FIFO at DM 0x3FED
                                           (lanes 0x14..16 = IDM high half): experimental VU source */

enum { MP3_IDLE, MP3_PLAYING, MP3_PAUSED, MP3_DONE };
typedef struct {
    uint8_t  vol;          /* first: kept across mp3_start */
    uint16_t now_s, total_s;
    uint32_t size;
    char     kbps[3];      /* 3 ASCII digits from the DSP */
    uint8_t  rate;         /* DSP rate code (spec 3.2) */
    uint8_t  state, resumed, unknown;
    uint16_t nreq;         /* DSP requests handled (diagnostics) */
    uint8_t  last;         /* last request code */
    uint8_t  vu;           /* experimental level 0..127 (|high byte| of the last DAC word) */
    uint8_t  mode;         /* MPEG channel mode from the DSP (port 0x30 at REQ_FORMAT) */
#define MP3_MODE_MONO 3
    uint32_t start;        /* byte offset the decoding started at (seek / resume) */
    uint16_t base_s;       /* its time (from the average byte rate): shown time = base_s + now_s */
    uint16_t bps;          /* average bytes per second (size / total), 0 = unknown */
} mp3_t;
extern mp3_t mp3;
extern fat_file_t mp3_file;                 /* the playing file (pos = next byte to send the DSP) */

uint8_t mp3_start(const fat_dirent_t *e, uint32_t off);  /* from byte off; 0 = MP3DEC.DSP missing */
void    mp3_base(void);                     /* REQ_FORMAT: bps, base_s from start (cold path) */
uint32_t mp3_offset(uint16_t s);            /* file byte position of second s (from bps) */
#define ID3_W   32                          /* tag text kept (the player scrolls it) */

void    id3_read(const fat_dirent_t *e, char *title, char *artist);   /* id3.c, cold path */
void    mp3_service(uint8_t req);           /* on EV_DSP: req = event code (ISR read + acked) */
void    mp3_pause(uint8_t on);
void    mp3_stop(void);
void    mp3_off(uint8_t keep_pa);           /* fade out + stop; analog off unless keep_pa */
void    mp3_volume(uint8_t v);              /* 0..31 */
void    mp3_cpu(uint8_t fast);              /* CPU 48 MHz (1) / 24.576 MHz (0) */
uint8_t mp3_cpu_fast(void);
#endif
