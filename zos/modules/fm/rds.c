/* RDS decoder for the RDA5807's block registers (tuner.c rds_poll): groups 0A/0B = Program Service
 * name (4 segments x 2 chars), 2A/2B = RadioText (16 segments x 4 / 2 chars, A/B flag change =
 * new text), PI from block A. The RDA reports error levels for blocks A and B only (0Bh BLERA/B), so
 * C/D are checked by repetition: a segment is taken only when two separate groups carry the same
 * characters (stations repeat PS ~2-4x per second, RadioText every few seconds). Identical
 * back-to-back reads (the chip's RDSR still set) count once. */
#include <stdint.h>
#include <string.h>
#include "tuner.h"

char     rds_ps[9];
char     rds_rt[65];
uint16_t rds_pi;
uint8_t  rds_new;

static char ps_c[8], ps_v[8], rt_c[64];
static uint8_t ps_ok, rt_ab, last[8];

#define BLER_OK 1               /* 0 = no errors, 1 = 1-2 corrected; 2-3 = unreliable */

void rds_clear(void)
{
    memset(ps_c, 0, sizeof ps_c); memset(ps_v, 0, sizeof ps_v); memset(rt_c, 0, sizeof rt_c);
    memset(rds_ps, 0, sizeof rds_ps); memset(rds_rt, 0, sizeof rds_rt); memset(last, 0, sizeof last);
    ps_ok = 0; rt_ab = 0xFF; rds_pi = 0;
    rds_new = RDS_PS | RDS_RT | RDS_PI;
}

/* n new characters vs the candidate: 0 = differs (now the candidate), 1 = confirmed, 2 = confirmed + new */
static uint8_t conf(char *cand, char *dst, const uint8_t *src, uint8_t n)
{
    uint8_t i, r = 1;
    for (i = 0; i < n; i++) if (cand[i] != (char)src[i]) { cand[i] = src[i]; r = 0; }
    if (r) for (i = 0; i < n; i++) if (dst[i] != (char)src[i]) { dst[i] = src[i]; r = 2; }
    return r;
}

/* r = registers 0Ah..0Fh (big endian): r[2..3] = 0Bh (BLERA bits 3-2, BLERB bits 1-0), r[4..11] = blocks A-D */
void rds_group(const uint8_t *r)
{
    const uint8_t *g = r + 4;
    uint8_t seg, n, ab;
    uint16_t pi;
    if ((r[3] & 3) > BLER_OK || !memcmp(last, g, 8)) return;
    memcpy(last, g, 8);
    pi = ((uint16_t)g[0] << 8) | g[1];
    if (((r[3] >> 2) & 3) <= BLER_OK && pi != rds_pi) {     /* another station: start over */
        rds_clear();
        memcpy(last, g, 8);
        rds_pi = pi;
    }
    if ((g[2] & 0xF0) == 0x00) {                            /* group 0A / 0B: PS, chars in D */
        seg = g[3] & 3;
        n = conf(ps_c + seg * 2, ps_v + seg * 2, g + 6, 2);
        if (!n) return;
        n = n == 2 || ps_ok != 0x0F;
        ps_ok |= 1 << seg;
        if (ps_ok == 0x0F && n) { memcpy(rds_ps, ps_v, 8); rds_new |= RDS_PS; }
    } else if ((g[2] & 0xF0) == 0x20) {                     /* group 2A / 2B: RadioText */
        ab = g[3] & 0x10;
        if (ab != rt_ab) {                                  /* A/B flag toggled: a new text */
            if (rt_ab != 0xFF) { memset(rt_c, 0, sizeof rt_c); memset(rds_rt, 0, sizeof rds_rt); rds_new |= RDS_RT; }
            rt_ab = ab;
        }
        seg = g[3] & 15;
        if (g[2] & 0x08) n = conf(rt_c + seg * 2, rds_rt + seg * 2, g + 6, 2);   /* 2B: 32 chars, D only */
        else n = conf(rt_c + seg * 4, rds_rt + seg * 4, g + 2 + 2, 4);           /* 2A: 64 chars, C + D */
        if (n == 2) rds_new |= RDS_RT;
    }
}

/* RDS character (EBU set) -> the font's A-Z 0-9 punctuation: lower case and the accented Latin
 * letters to their capitals, anything else 0 (dropped) */
static const char acc_lo[16] = "AAEEIIOOUUNCSS!I";      /* 0x80-0x9F: a-acute ... */
static const char acc_up[16] = "AAEEIIOOUURCSZDL";      /* 0xC0-0xDF: A-acute ... */
static char rds_ch(uint8_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0x80 && c < 0xA0) return acc_lo[c & 15];
    if (c >= 0xC0 && c < 0xE0) return acc_up[c & 15];
    return c >= ' ' && c <= 'Z' ? c : 0;
}

/* displayable copy of up to n raw chars (stops at 0 = not received yet, or CR = end); returns the length */
uint8_t rds_text(char *d, const char *s, uint8_t n)
{
    uint8_t k = 0;
    char c;
    while (n-- && (c = *s++) && c != 0x0D)
        if ((c = rds_ch(c))) d[k++] = c;
    d[k] = 0;
    return k;
}
