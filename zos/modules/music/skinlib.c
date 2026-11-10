#include "../common/skin.c"
#include "../common/draw.c"
#include "../common/cfg.c"
#include "../common/beep.c"

/* MUSIC: the colour of each of the first h rows of a skin image (its middle column): SELBAR.ZI, so
 * that the scrolling name of the selected row can be redrawn over the bar's gradient without reading
 * the image again; PROGBAR.ZI / VOLBAR.ZI for the players' bars (main.c). 0 = no such image */
uint8_t skin_column(const char *name11, uint16_t *col, uint8_t h)
{
    fat_file_t f;
    uint8_t hdr[5], r, px[2];
    if (!skin_open(name11, &f) || fat_read(&f, hdr, 5) != 5 || hdr[0] != 'Z' || hdr[1] != 'I' ||
        hdr[3] < h || !hdr[2])
        return 0;
    for (r = 0; r < h; r++) {
        if (!fat_seek(&f, 5 + ((uint32_t)r * hdr[2] + hdr[2] / 2) * 2) || fat_read(&f, px, 2) != 2) return 0;
        col[r] = ((uint16_t)px[0] << 8) | px[1];
    }
    return 1;
}
