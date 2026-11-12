/* zos home: runs the SHELL module (the launcher UI, modules/shell) and whatever it chains to;
 * a module that returns without chaining comes back to SHELL. SHELL returns MOD_REBOOT to leave.
 * Without SHELL.ZM on the disk: the plain module list (apps/modules.c), MENU there = reboot. */
#include <stdint.h>
#include <string.h>
#include "../hal/hal.h"
#include "../core/event.h"
#include "../core/module.h"
#include "theme.h"
#include "apps.h"

void ui_header(const char *title)
{
    gfx_fill(0, 0, LCD_W, 12, C_BAR);
    gfx_text(4, 2, title, 1, C_FG, C_BAR);
}

void ui_footer(const char *text)
{
    gfx_fill(0, LCD_H - 10, LCD_W, 10, C_BG);
    gfx_text(4, LCD_H - 9, text, 1, C_DIM, C_BG);
}

static const char shell[] = "SHELL   ZM ";

void home(void)
{
    mod_hdr_t hdr;
    const char *name = shell;
    uint16_t r;
    memset((void *)HAL_SHARED, 0, 512);          /* power-on: stock leftovers, no stale magics */
    HAL_CLK->core = HAL_CLK_CORE;                /* this core keeps the wall clock (hal.h); SHELL starts it */
    for (;;) {
        if (mod_load(name, hal_pages[0], &hdr) != MOD_OK) {
            if (name == shell) break;            /* no shell: fallback list */
            name = shell;                        /* a chained module is missing: back to the shell */
            continue;
        }
        r = mod_run(hal_pages[0], &hdr);
        if (r == MOD_REBOOT) return;
        name = r == MOD_CHAIN ? MOD_CHAIN_NAME : shell;
    }
    app_modules();
}
