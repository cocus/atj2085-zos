/* USBCON: USB serial console (function module of the USB launcher, modules/usb; not in the shell
 * menu). A CDC-ACM COM port with a debug command line (cdc.c); "wr" / "rdmode" / "log" act on the
 * mass-storage settings and event log kept for USBDISK (common/usb/usbsh.h). PLAY or MENU
 * disconnects and goes back to the launcher, which drew the screen chrome; only the status lines are
 * drawn here (common/usb/usbfn.c). */
#include <stdint.h>
#include "../common/usb/usb.h"
#include "../common/usb/usbfn.c"

static void fn_status(void)
{
    if (!state_line()) return;
    line(58, "PORT", cdc_dtr ? "OPEN (DTR)" : "CLOSED", cdc_dtr ? C_OK : C_FG);
    numline(70, "RX", cdc_nrx, "DRP", cdc_ndrop);
}

static void fn_poll(void) { cdc_poll(); }
static void fn_abort(uint8_t why) { (void)why; }
static void fn_end(void) { }

uint16_t mod_main(void)
{
    usb_disk_size();                                  /* for "ver" */
    return fn_session(FN_CDC);
}
