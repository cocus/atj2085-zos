/* MP3PLAY: simple tag reader (cold path, section _CODE2). Title + artist: the TIT2/TPE1 frames of
 * an ID3v2.3/2.4 tag found in the first 512 bytes of the file (where the text frames sit in
 * practice; album art comes later), text frames < 256 bytes; else the ID3v1 tag at the end.
 * Encodings Latin-1/UTF-8 (ASCII part), UTF-16 with BOM, UTF-16BE. Output: upper case A-Z 0-9
 * punctuation (the font), anything else '?', ID3_W chars max. Buffer: the bitstream sector
 * mp3_bs (free: called while the DSP is stopped). */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../core/fat.h"
#include "mp3.h"
#include "mp3int.h"

static void put(char *d, const uint8_t *s, uint8_t n, uint8_t enc)
{
    uint8_t i = 0, step = 1, c;
    if (enc == 1 || enc == 2) {                /* UTF-16: take the low byte of each unit */
        step = 2;
        if (enc == 1 && n >= 2 && s[0] == 0xFE) { s += 3; n -= 3; }   /* BOM FE FF: big endian */
        else if (enc == 1 && n >= 2) { s += 2; n -= 2; }            /* BOM FF FE: little endian */
        else s++;                              /* UTF-16BE without BOM */
    }
    for (; n && i < ID3_W; s += step, n = n > step ? n - step : 0) {
        c = *s;
        if (!c) break;
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c < 32 || c > 90) c = '?';
        d[i++] = c;
    }
    while (i && d[i - 1] == ' ') i--;
    d[i] = 0;
}

void id3_read(const fat_dirent_t *e, char *title, char *artist)
{
    fat_file_t f;
    uint8_t *p = mp3_bs, *end, sz;
    char *d;
    title[0] = artist[0] = 0;
    fat_open(&f, e);
    end = p + fat_read(&f, p, 512);
    if (end - p >= 10 && !memcmp(p, "ID3", 3))
        for (p += 10, end -= 138; p < end; p++) {  /* frame header + text (< 128) must fit */
            if (*p != 'T' || p[4] || p[5] || p[6]) continue;
            if (!memcmp(p + 1, "IT2", 3)) d = title;
            else if (!memcmp(p + 1, "PE1", 3)) d = artist;
            else continue;
            sz = p[7];                         /* < 128: the same in 2.3 and syncsafe 2.4 */
            if (sz < 2 || sz > 127) continue;
            put(d, p + 11, sz - 1, p[10]);
            p += 9 + sz;
        }
    p = mp3_bs;
    if (!title[0] && e->size >= 128 && fat_seek(&f, e->size - 128) && fat_read(&f, p, 128) == 128 &&
        !memcmp(p, "TAG", 3)) {
        put(title, p + 3, 30, 0);
        put(artist, p + 33, 30, 0);
    }
}
