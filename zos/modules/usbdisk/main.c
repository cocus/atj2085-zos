/* USBDISK: USB mass storage (function module of the USB launcher, modules/usb; not in the shell menu).
 * The user disk as a removable drive, read-write unless the launcher's 48 MHz setting or the console's
 * "wr off" turned writes off (common/usb/usbsh.h). PLAY or MENU disconnects and goes back to the
 * launcher. Started by the launcher, which drew the screen chrome; only the status lines are drawn
 * here (common/usb/usbfn.c). Design: lisong_rebuild/analysis/zos_usb.md. */
#include <stdint.h>
#include <string.h>
#include "../common/usb/usb.h"
#include "../common/ftlw.h"
#include "../common/usb/usbfn.c"

/* last SCSI command on the LCD: "LAST 28 OK  RD FLAT" (hex opcode) */
static void hexop(void)
{
    char t[18];
    static const char hx[] = "0123456789ABCDEF";
    t[0] = hx[msc_lastop >> 4]; t[1] = hx[msc_lastop & 15]; t[2] = ' ';
    t[3] = msc_ncmd ? (msc_laststatus ? 'F' : 'O') : '-';
    t[4] = msc_ncmd ? (msc_laststatus ? 'L' : 'K') : '-';
    strcpy(t + 5, msc_rdmode ? "  RD PP" : "  RD FLAT");
    line(108, "LAST", t, msc_laststatus ? C_SEL : C_FG);
}

static void fn_status(void)
{
    if (!state_line()) return;
    if (msc_ejected) line(58, "DISK", "EJECTED", C_SEL);
    else numline(58, "CMDS", msc_ncmd, "MB", (uint16_t)((msc_last + 1) >> 11));
    numline(70, "SECT", msc_nsec, 0, 0);
    hexop();
}

static void fn_poll(void)
{
    msc_poll();
    if (usb_state == US_NOVBUS) msc_ejected = 0;      /* cable out: the next host sees the disk */
}

static void fn_abort(uint8_t why) { msc_abort_note(why); }   /* logged if a SCSI command was cut off */

static void fn_end(void) { ftlw_flush(); }          /* written data in place before anything else */

uint16_t mod_main(void)
{
    usb_disk_size();
    return fn_session(FN_MSC);
}
