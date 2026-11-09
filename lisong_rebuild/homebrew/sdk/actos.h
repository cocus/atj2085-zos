/* ACTOS OS services for homebrew APs (see actos.s). */
#ifndef ACTOS_H
#define ACTOS_H
#include <stdint.h>
uint8_t os_get_msg(void);          /* next system/key message, 0 = none */
void    os_put_msg(uint8_t msg);
void    wdt_kick(void);
void    os_backlight(uint8_t on);
/* key codes posted by KY_HARD.DRV - confirmed on the device with KEYTEST */
#define KEY_MENU  0x01   /* MENU */
#define KEY_LEFT  0x04   /* << (prev) */
#define KEY_RIGHT 0x05   /* >> (next) */
#define KEY_PLAY  0x06   /* PLAY/PAUSE */
#define KEY_UP    0x30   /* key released; a held key repeats its code */
#define MSG_TICK    0x80 /* periodic system message, 2 per second (device-confirmed) */
#define MSG_USB_IN  0x91 /* USB cable plugged in   (device-confirmed) */
#define MSG_USB_OUT 0x92 /* USB cable unplugged    (device-confirmed) */

/* OS timer: os_timer_start(1) calls an internal handler every period x 10 ms (device-confirmed:
 * ~50 ticks per MSG_TICK); os_now() returns the tick count. ALWAYS os_timer_stop() before
 * returning from main, or the OS keeps calling into memory the next app overwrites. */
extern volatile uint16_t os_ticks;
uint8_t os_timer_start(uint8_t period);
void    os_timer_stop(uint8_t id);
static inline uint16_t os_now(void)
{
    uint16_t a, b;
    do { a = os_ticks; b = os_ticks; } while (a != b);
    return a;
}

/* AP result codes (SDK ap_common.h) */
#define RESULT_NULL 0
#define RESULT_MAIN 10
#define RESULT_STANDBY 2   /* stock MAIN switch (bank40 0x194B): -> standby.ap */
#define RESULT_UPGRADE 3   /*   -> adfuf641.ap (USBDISK returns it when the PC asks for ADFU) */
#define RESULT_UDISK   4   /*   -> usbdisk.ap */

uint16_t os_exec_ap(const char *name, uint16_t param);   /* run another AP, -> its result */
uint8_t  os_drv_install(uint8_t mode, const char *name);
uint8_t  os_usb_cable(void);                             /* != 0: USB cable plugged */
/* system settings block (comval, 512 bytes; first word 0xDEAD when valid), as the stock MAIN */
#define COMVAL_MAGIC 0xDEAD
void os_comval_read(void *buf512);
void os_comval_write(const void *buf512);
void os_comval_default(void *buf512);
void os_set_contrast(uint16_t v);
#endif
