/* DECINFO: helper of DECPLAY (not a menu entry). DECPLAY (ZRAM2 module) has no room for header parsing,
 * so it loads this page module into a DSP lane page while the DSP is stopped and runs it once per track
 * (decplay/deccold.c id3_read). Request/answer: MUS->arg = dec_info_t (decplay/dec.h).
 *  - Ogg Vorbis: identification header (first page: channels, rate, nominal bitrate), comment header
 *    TITLE= / ARTIST= (inside the first 1 KB), total time = granule of the last page / rate (the stock
 *    gets it from a second "caltime" DSP run instead).
 *  - WAV (RIFF): what PCMDEC.DSP's header parser accepts (its PM 0x02DE-0x032B, checked by
 *    disassembly): format 0x11 IMA ADPCM or 1 PCM, 1-2 channels, 4 or 16 bits, a rate of its table
 *    (48000 44100 32000 24000 22050 16000 12000 11025 8000); total = data bytes / byte rate. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "../music/shared.h"
#include "../decplay/dec.h"

static uint8_t b[1024];
static fat_file_t f;
static dec_info_t *I;

static uint32_t le32(const uint8_t *p) { return *(const uint32_t *)p; }

static void kbps(uint32_t k)
{
    if (k > 999) k = 999;
    I->kbps[2] = '0' + k % 10; k /= 10;
    I->kbps[1] = '0' + k % 10;
    I->kbps[0] = '0' + (uint8_t)k / 10;
}

static void rate(uint32_t bytes_per_s)         /* average byte rate for seeking: 16 bits + shift */
{
    I->shift = 0;
    while (bytes_per_s > 0xFFFF) { bytes_per_s >>= 1; I->shift++; }
    I->bps = bytes_per_s;
}

/* the same text filter as mp3play/id3.c: upper case A-Z 0-9 punctuation, else '?' */
static void put(char *d, const uint8_t *s, uint16_t n)
{
    uint8_t i = 0, c;
    for (; n && i < ID3_W; s++, n--) {
        c = *s;
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c < 32 || c > 90) c = '?';
        d[i++] = c;
    }
    while (i && d[i - 1] == ' ') i--;
    d[i] = 0;
}

static uint8_t key(const uint8_t *p, const char *k)   /* case-insensitive "KEY=" */
{
    uint8_t c;
    for (; *k; p++, k++) {
        c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != *k) return 0;
    }
    return 1;
}

/* Vorbis comments: "TITLE=..." fields preceded by their 32-bit little-endian length */
static void comments(uint16_t n)
{
    uint16_t i, len;
    uint8_t m;
    char *d;
    for (i = 4; i + 8 < n; i++) {
        if (key(b + i, "TITLE=")) { m = 6; d = I->title; }
        else if (key(b + i, "ARTIST=")) { m = 7; d = I->artist; }
        else continue;
        if (b[i - 1] || b[i - 2] || le32(b + i - 4) < m || d[0]) continue;
        len = le32(b + i - 4) - m;
        if (len > n - i - m) len = n - i - m;
        put(d, b + i + m, len);
    }
}

static uint8_t ogg(uint16_t n)
{
    uint8_t *p = b + 27 + b[26];               /* first packet, behind the segment table */
    uint32_t r, g, pos, k;
    int16_t i;
    if (p + 30 > b + n || *p != 1 || memcmp(p + 1, "vorbis", 6)) return 0;
    I->ogg = 1;
    I->mode = p[11] == 1 ? MP3_MODE_MONO : 0;
    r = le32(p + 12);
    kbps(le32(p + 20) / 1000);                 /* nominal bitrate */
    comments(n);
    if (!r) return 0;
    for (k = 0; k < 40; k++) {                 /* the last "OggS" in the last ~20 KB, overlapping windows */
        pos = f.size > 512 + k * 498 ? f.size - 512 - k * 498 : 0;
        if (!fat_seek(&f, pos)) break;
        for (i = (int16_t)fat_read(&f, b, 512) - 14; i >= 0; i--)
            if (b[i] == 'O' && !memcmp(b + i, "OggS", 4) && (g = le32(b + i + 6)) != 0xFFFFFFFFUL) {
                I->total_s = g / r;
                if (I->total_s) rate(f.size / I->total_s);
                return 1;
            }
        if (!pos) break;
    }
    return 1;                                  /* no granule: plays, total unknown, no seeking */
}

static const uint16_t rates[] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };

static uint8_t wav(void)
{
    uint32_t off = 12, n, br = 0;
    uint8_t i, j, ok = 0;
    if (memcmp(b + 8, "WAVE", 4)) return 0;
    for (i = 0; i < 32; i++) {
        if (!fat_seek(&f, off) || fat_read(&f, b, 24) != 24) return 0;
        n = le32(b + 4);
        if (!memcmp(b, "fmt ", 4)) {
            ok = b[10] && b[10] <= 2 && !b[11] && !b[9] &&
                 ((b[8] == 0x11 && b[22] == 4) || (b[8] == 1 && b[22] == 16));
            for (j = 0; j < sizeof rates / 2 && rates[j] != le32(b + 12); j++) ;
            if (j == sizeof rates / 2) ok = 0;
            I->mode = b[10] == 1 ? MP3_MODE_MONO : 0;
            br = le32(b + 16);
        } else if (!memcmp(b, "data", 4)) {
            if (!ok || !br) return 0;
            I->total_s = n / br;
            kbps(br / 125);                    /* bytes/s x 8 / 1000 */
            rate(br);
            return 1;
        }
        off += 8 + ((n + 1) & ~1UL);
    }
    return 0;
}

uint16_t mod_main(void)
{
    uint16_t n;
    I = (dec_info_t *)MUS->arg;
    if (MUS->magic != MUS_MAGIC || !I) return 0;
    I->ok = I->ogg = I->mode = 0;
    I->total_s = I->bps = 0;
    I->shift = 0;
    memset(I->kbps, ' ', 3);
    fat_open(&f, I->e);
    n = fat_read(&f, b, sizeof b);
    if (n >= 64) I->ok = !memcmp(b, "OggS", 4) ? ogg(n) : !memcmp(b, "RIFF", 4) && wav();
    return 0;
}
