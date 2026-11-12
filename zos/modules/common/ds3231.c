/* DS3231 driver, see ds3231.h. Bit-banged I2C as modules/fm/tuner.c (push-pull master, SDA released for
 * the slave's ACK / data), without the USB-audio pump(). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "regs.h"
#include "ds3231.h"

SFR(DS_GPC_EN, P_GPC_EN);
SFR(DS_GPC_D, P_GPC_D);

#define DS_ADDR   0x68
#define DS_OUT    (GPC_SCL | GPC_SDA)            /* output enables (bits 0-2) */
#define DS_IN     ((GPC_SCL | GPC_SDA) << 4)     /* input enables (bits 4-6) */
#define DS_EN_VALID 0x77
#define DS_D_VALID  0x07
#define DS_STATUS 0x0F
#define DS_OSF    0x80

static uint8_t sv_en, sv_d;

static void dly(void) { volatile uint8_t i; for (i = 4; i; i--) ; }
static void en_set(uint8_t set, uint8_t clr) { DS_GPC_EN = ((DS_GPC_EN & DS_EN_VALID) | set) & ~clr & DS_EN_VALID; }
static void d_set(uint8_t set, uint8_t clr) { DS_GPC_D = ((DS_GPC_D & DS_D_VALID) | set) & ~clr & DS_D_VALID; }
static void scl(uint8_t v) { if (v) d_set(GPC_SCL, 0); else d_set(0, GPC_SCL); }
static void sda(uint8_t v) { if (v) d_set(GPC_SDA, 0); else d_set(0, GPC_SDA); en_set(GPC_SDA, GPC_SDA << 4); }
static void sda_release(void) { en_set(GPC_SDA << 4, GPC_SDA); }
static uint8_t sda_read(void) { return (DS_GPC_D & GPC_SDA) ? 1 : 0; }
static void start(void) { sda(1); scl(1); dly(); sda(0); dly(); scl(0); dly(); }   /* also repeated START */
static void stop(void) { scl(0); sda(0); dly(); scl(1); dly(); sda(1); dly(); }
static uint8_t wr(uint8_t b)
{
    uint8_t i, ack;
    for (i = 0; i < 8; i++) { sda(b & 0x80); b <<= 1; dly(); scl(1); dly(); scl(0); }
    sda_release(); dly(); scl(1); dly();
    ack = !sda_read();
    scl(0); dly(); sda(0);
    return ack;
}
static uint8_t rd(uint8_t nack)
{
    uint8_t i, v = 0;
    sda_release();
    for (i = 0; i < 8; i++) { scl(1); dly(); v = (v << 1) | sda_read(); scl(0); dly(); }
    sda(nack); dly(); scl(1); dly(); scl(0); dly(); sda(0);
    return v;
}

static void bus_open(void)
{
    sv_en = DS_GPC_EN; sv_d = DS_GPC_D;
    d_set(GPC_SCL | GPC_SDA, 0);                 /* idle high */
    en_set(DS_OUT, DS_IN);
}

static void bus_close(void)                      /* the pins as found */
{
    DS_GPC_D = (DS_GPC_D & ~(GPC_SCL | GPC_SDA)) | (sv_d & (GPC_SCL | GPC_SDA));
    DS_GPC_EN = (DS_GPC_EN & ~(DS_OUT | DS_IN)) | (sv_en & (DS_OUT | DS_IN));
}

static uint8_t bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static uint8_t enc(uint8_t v) { uint8_t t = 0; while (v >= 10) { v -= 10; t++; } return (t << 4) | v; }

uint8_t ds_read(ds_time_t *t)
{
    uint8_t r[16], i, ok, h;
    bus_open();
    ok = (start(), wr(DS_ADDR << 1)) && wr(0) && (start(), wr((DS_ADDR << 1) | 1));
    if (ok) for (i = 0; i < 16; i++) r[i] = rd(i == 15);
    stop();
    bus_close();
    if (!ok) return DS_NONE;
    h = r[2] & 0x1F;                             /* 12 h mode (bit 6): bit 5 = PM; 24 h: bit 5 = tens */
    if (r[2] & 0x40) { h = bcd(h & 0x1F); if (h == 12) h = 0; if (r[2] & 0x20) h += 12; }
    else h = bcd(r[2] & 0x3F);
    t->sec = bcd(r[0] & 0x7F); t->min = bcd(r[1] & 0x7F); t->hour = h;
    t->dow = r[3] & 7; t->date = bcd(r[4] & 0x3F);
    t->month = bcd(r[5] & 0x1F); t->year = bcd(r[6]);
    if ((r[DS_STATUS] & DS_OSF) || t->sec > 59 || t->min > 59 || t->hour > 23 || !t->date || t->date > 31 ||
        !t->month || t->month > 12 || t->year > 99)
        return DS_OSF;
    return DS_OK;
}

uint8_t ds_write(const ds_time_t *t)
{
    uint8_t ok, st = 0;
    bus_open();
    ok = (start(), wr(DS_ADDR << 1)) && wr(0) && wr(enc(t->sec)) && wr(enc(t->min)) && wr(enc(t->hour)) &&
         wr(t->dow) && wr(enc(t->date)) && wr(enc(t->month)) && wr(enc(t->year));   /* 24 h mode, no century */
    stop();
    if (ok && (start(), wr(DS_ADDR << 1)) && wr(DS_STATUS) && (start(), wr((DS_ADDR << 1) | 1))) {
        st = rd(1);
        stop();
        ok = (start(), wr(DS_ADDR << 1)) && wr(DS_STATUS) && wr(st & ~DS_OSF);      /* clear OSF */
    } else ok = 0;
    stop();
    bus_close();
    return ok;
}
