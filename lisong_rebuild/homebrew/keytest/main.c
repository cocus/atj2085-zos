/* KEYTEST - shows every OS message (key codes, system messages) as hex.
 * Debug tool: run it on the device to learn the key codes of the player.
 * Exit: hold any key (6 identical key codes in a row). */
#include <stdint.h>
#include "actos.h"
#include "gfx.h"

#define BG  RGB(0, 0, 60)
#define FG  RGB(255, 255, 255)
#define HI  RGB(255, 255, 0)

uint16_t main(uint16_t param)
{
    static const char hx[] = "0123456789ABCDEF";
    char t[3];
    uint8_t m, last = 0, same = 0, x = 0, y = 24;
    uint16_t ticks80 = 0, t_last = 0, t_now;
    uint8_t timer;
    (void)param;
    timer = os_timer_start(1);
    gfx_fill(0, 0, SCR_W, SCR_H, BG);
    gfx_text(4, 2, "KEY/MSG TEST", 1, HI, BG);
    gfx_text(4, 12, "HOLD A KEY TO EXIT", 1, FG, BG);
    t[2] = 0;
    for (;;) {
        wdt_kick();
        m = os_get_msg();
        if (!m) continue;
        if (m == 0x80) {                      /* periodic system message: just count it */
            ticks80++;
            t_now = os_now();
            gfx_text(4, 118, "80 X", 1, FG, BG);
            gfx_num(28, 118, ticks80, 1, FG, BG);
            gfx_text(64, 118, "T/80", 1, FG, BG);      /* timer ticks per 0x80 message */
            gfx_fill(94, 118, 34, 8, BG);
            gfx_num(94, 118, t_now - t_last, 1, HI, BG);
            t_last = t_now;
            continue;
        }
        t[0] = hx[m >> 4]; t[1] = hx[m & 15];
        gfx_text(x, y, t, 1, HI, BG);
        x += 16;
        if (x > 112) { x = 0; y += 10; if (y > 104) { y = 24; gfx_fill(0, 24, SCR_W, 90, BG); } }
        if (m == last && m != KEY_UP) { if (++same >= 4) break; } else { same = 0; last = m; }
    }
    os_timer_stop(timer);
    return RESULT_NULL;
}
