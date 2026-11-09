/* DECPLAY file reads for the DSP (section _CODE2: no room in ZRAM2; ZRAM1 code
 * runs at any time, the bitstream buffer is in ZRAM1 too) */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "regs.h"
#include "dec.h"
#define file mp3_file
#define bs   dec_bs

/* sector s of the file into bs (the prefetch slot); 0 = past the end / read error */
uint8_t dec_read_sec(uint32_t s)
{
    uint16_t n = 0;
    dec_slow();
    if (fat_seek(&file, s << 9)) n = fat_read(&file, bs, 512);
    dec_fast();
    if (n && n < 512) memset(bs + n, 0, 512 - n);
    dec_pf = s;
    return dec_have = n != 0;
}
