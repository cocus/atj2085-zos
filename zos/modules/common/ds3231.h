/* DS3231 real-time clock on the tuner module's bit-banged I2C bus (GPIO C0 = SCL, C1 = SDA, the pins and
 * timing of modules/fm/tuner.c; address 0x68). The pins are set up for each transaction and restored
 * as found, so the FM tuner (0x60 / 0x10 / 0x11) on the same bus is not disturbed. Registers 00h-06h
 * BCD (sec, min, hour, day of week, date, month + century bit, year), 0Fh bit 7 OSF = the oscillator
 * stopped (battery was empty / first power-up): the time is not valid until it is written again.
 * Include ds3231.c from one module source. */
#ifndef ZOS_DS3231_H
#define ZOS_DS3231_H
#include <stdint.h>

typedef struct { uint8_t year, month, date, hour, min, sec, dow; } ds_time_t;   /* year 0..99 = 2000..2099, dow 1..7 */
enum { DS_NONE, DS_OK, DS_OSF };       /* no ACK / valid / present but OSF set or garbage in the registers */

uint8_t ds_read(ds_time_t *t);          /* -> DS_*; t filled for DS_OK */
uint8_t ds_write(const ds_time_t *t);   /* set the time and clear OSF; 1 = ok */
#endif
