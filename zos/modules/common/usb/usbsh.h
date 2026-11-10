/* USB launcher (USB.ZM) <-> its function modules (USBDISK.ZM mass storage, USBCON.ZM serial console,
 * USBAUDIO.ZM) hand-over block at HAL_SHARED: the launcher draws the screen chrome once and chains to
 * a function module, which only redraws its status lines and chains back. Kept here: the menu choice
 * and the settings that must outlive one module (mass-storage writes, READ(10) path, 48 MHz, the
 * event-log ring position: the ring itself is in ZRAM1, usb.h zr_t). SHELL sets `autostart` when a PC
 * woke the player (standby): the launcher then goes straight into mass storage. */
#ifndef ZOS_USBSH_H
#define ZOS_USBSH_H
#include <stdint.h>
#include "../../../hal/hal.h"

#define USB_MAGIC 0x5B                  /* (not MUS_MAGIC: the MUSIC block shares the offset) */
#define USB_ZM    "USB     ZM "
typedef struct {
    char    next[12];                   /* MOD_CHAIN_NAME: must be first */
    uint8_t magic;                      /* USB_MAGIC: the rest is valid */
    uint8_t fn;                         /* launcher menu item (FN_MSC, FN_CDC, 2 = USB AUDIO) */
    uint8_t wr;                         /* mass-storage writes (default 1) */
    uint8_t rdmode;                     /* READ(10) path: 1 = stock ping-pong (default), 0 = flat */
    uint8_t fast;                       /* 1 = function modules run the CPU at 48 MHz */
    uint8_t autostart;                  /* SHELL: woken by a PC -> mass storage at once */
    uint8_t back;                       /* set by a function module chaining back */
    uint8_t logpos, logn;               /* mass-storage event ring (usb.h) */
} usb_shared_t;
static __at(HAL_SHARED) usb_shared_t usb_sh;  /* absolute: no storage allocated */
#define USH (&usb_sh)
#endif
