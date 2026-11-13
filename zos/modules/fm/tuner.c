/* FM tuner drivers on the bit-banged I2C bus (GPIO C0 = SCL, C1 = SDA; lisong_rebuild/analysis/i2c.md):
 * TEA5767 (5-byte frames at 0x60, as the stock FMRADIO.AP) and RDA5807 in native register mode
 * (lisong_rebuild/analysis/ogg_rec_rds.md 3.5). Every 4 bits the bus calls pump() (USB OUT / VU service, main.c). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "regs.h"
#include "tuner.h"

void pump(void);                        /* main.c */
void wait_ticks(uint16_t t);


#define GPC_EN_OUT    (GPC_SCL | GPC_SDA)               /* output enables (bits 0-2) */
#define GPC_EN_IN     ((GPC_SCL | GPC_SDA) << 4)        /* input enables (bits 4-6) */
#define GPC_EN_VALID  0x77
#define GPC_D_VALID   0x07

uint8_t tn_type;
static uint8_t sv_en, sv_d, seeking, chip_id;
static uint16_t cur_f10;

/* ---- I2C (push-pull master: SDA is released for the slave's ACK / data) ------------------------ */
static void dly(void) { volatile uint8_t i; for (i = 4; i; i--) ; }
static void en_set(uint8_t set, uint8_t clr) { GPC_EN = ((GPC_EN & GPC_EN_VALID) | set) & ~clr & GPC_EN_VALID; }
static void d_set(uint8_t set, uint8_t clr) { GPC_D = ((GPC_D & GPC_D_VALID) | set) & ~clr & GPC_D_VALID; }
static void scl(uint8_t v) { if (v) d_set(GPC_SCL, 0); else d_set(0, GPC_SCL); }
static void sda(uint8_t v) { if (v) d_set(GPC_SDA, 0); else d_set(0, GPC_SDA); en_set(GPC_SDA, GPC_SDA << 4); }
static void sda_release(void) { en_set(GPC_SDA << 4, GPC_SDA); }
static uint8_t sda_read(void) { return (GPC_D & GPC_SDA) ? 1 : 0; }
static void i2c_start(void) { sda(1); scl(1); dly(); sda(0); dly(); scl(0); dly(); }   /* also repeated START */
static void i2c_stop(void) { scl(0); sda(0); dly(); scl(1); dly(); sda(1); dly(); }
static uint8_t i2c_wr(uint8_t b)
{
    uint8_t i, ack;
    for (i = 0; i < 8; i++) { sda(b & 0x80); b <<= 1; dly(); scl(1); dly(); scl(0); if ((i & 3) == 3) pump(); }
    sda_release(); dly(); scl(1); dly();
    ack = !sda_read();
    scl(0); dly(); sda(0);
    return ack;
}
static uint8_t i2c_rd(uint8_t nack)
{
    uint8_t i, v = 0;
    sda_release();
    for (i = 0; i < 8; i++) { scl(1); dly(); v = (v << 1) | sda_read(); scl(0); dly(); if ((i & 3) == 3) pump(); }
    sda(nack); dly(); scl(1); dly(); scl(0); dly(); sda(0);
    return v;
}
static uint8_t i2c_addr(uint8_t a8) { i2c_start(); return i2c_wr(a8); }   /* START + address byte */

void i2c_open(void)
{
    sv_en = GPC_EN; sv_d = GPC_D;
    d_set(GPC_SCL | GPC_SDA, 0);                /* I2C idle high */
    en_set(GPC_EN_OUT, GPC_EN_IN);
}
void i2c_close(void)
{
    GPC_D = (GPC_D & ~(GPC_SCL | GPC_SDA)) | (sv_d & (GPC_SCL | GPC_SDA));
    GPC_EN = (GPC_EN & ~(GPC_EN_OUT | GPC_EN_IN)) | (sv_en & (GPC_EN_OUT | GPC_EN_IN));
}

/* ---- TEA5767 ----------------------------------------------------------------------------------- */
#define TEA_ADDR      0x60
#define TEA3_HLSI     0x10      /* write byte 3: high-side injection */
#define TEA4_STBY     0x40      /* write byte 4 */
#define TEA4_XTAL     0x10      /*   32.768 kHz crystal */
#define TEA4_HCC      0x04      /*   high cut */
#define TEA4_SNC      0x02      /*   stereo noise cancelling */
#define TEAR2_STEREO  0x80      /* read byte 2 */
#define TEAR2_IF      0x7F      /*   IF counter: a real station has 0x31..0x3E */
#define TEAR3_LEV_SH  4         /* read byte 3: level 0-15 in bits 7-4 */
#define SEEK_LEVEL    7

static uint8_t tea_tune(uint16_t f10, uint8_t standby)
{
    uint32_t k = (uint32_t)f10 * 100UL + 225UL;               /* kHz + IF (high side) */
    uint16_t pll = (uint16_t)((k * 125UL + 512UL) >> 10);     /* stock CALCULATE_PLL: 4 * f / 32768 Hz */
    uint8_t w[5], i, ok;
    w[0] = (uint8_t)(pll >> 8); w[1] = (uint8_t)pll; w[2] = TEA3_HLSI;
    w[3] = TEA4_XTAL | TEA4_HCC | TEA4_SNC | (standby ? TEA4_STBY : 0); w[4] = 0;
    ok = i2c_addr(TEA_ADDR << 1);
    for (i = 0; ok && i < 5; i++) ok = i2c_wr(w[i]);
    i2c_stop();
    return ok;
}

/* ---- RDA5807 (native mode) --------------------------------------------------------------------- */
#define RDA_SEQ       0x10      /* sequential: writes from 02h, reads from 0Ah */
#define RDA_RND       0x11      /* random access: register index first */
#define RDA_ID_HI     0x58      /* 00h CHIPID = 0x58xx */
#define R02_DHIZ      0x8000    /* audio outputs driven (0 = high-Z) */
#define R02_DMUTE     0x4000    /* 1 = not muted */
#define R02_MONO      0x2000
#define R02_BASS      0x1000
#define R02_SEEKUP    0x0200
#define R02_SEEK      0x0100    /* cleared by the chip at the end (STC) */
#define R02_SKMODE    0x0080    /* 1 = stop at the band edge, 0 = wrap */
#define R02_RDS_EN    0x0008
#define R02_NEWMETH   0x0004    /* new demodulation method (better sensitivity) */
#define R02_RESET     0x0002
#define R02_ENABLE    0x0001
#define R02_RUN       (R02_DHIZ | R02_DMUTE | R02_RDS_EN | R02_NEWMETH | R02_ENABLE)
#define R03_TUNE      0x0010    /* + CHAN << 6, BAND 00 = 87-108 MHz, SPACE 00 = 100 kHz */
#define R03_CHAN_SH   6
#define R04_DE50      0x0800    /* de-emphasis 50 us (as the TEA path: DTC = 0) */
#define R05_VAL       0x888F    /* INT_MODE, SEEKTH 8, LNA_PORT_SEL LNAP, VOLUME 15 (max: the PA sets the level) */
#define R0A_RDSR      0x80      /* in the high byte of 0Ah */
#define R0A_STC       0x40
#define R0A_ST        0x04
#define R0B_FMTRUE    0x01      /* in the high byte of 0Bh; RSSI = bits 7-1 */
#define RDA_BASE      870       /* CHAN 0 = 87.0 MHz */

static uint8_t rda_wr(uint8_t reg, uint16_t v)
{
    uint8_t ok = i2c_addr(RDA_RND << 1) && i2c_wr(reg) && i2c_wr((uint8_t)(v >> 8)) && i2c_wr((uint8_t)v);
    i2c_stop();
    return ok;
}
static uint16_t rda_rd(uint8_t reg)                 /* random read; 0 when nothing answers */
{
    uint16_t v = 0;
    if (i2c_addr(RDA_RND << 1) && i2c_wr(reg) && i2c_addr((RDA_RND << 1) | 1)) {
        v = (uint16_t)i2c_rd(0) << 8;
        v |= i2c_rd(1);
    }
    i2c_stop();
    return v;
}
/* sequential read from 0Ah: n bytes into r; 0 = no ACK */
static uint8_t rda_seq(uint8_t *r, uint8_t n)
{
    uint8_t i, ok = i2c_addr((RDA_SEQ << 1) | 1);
    if (ok) for (i = 0; i < n; i++) r[i] = i2c_rd(i == n - 1);
    i2c_stop();
    return ok;
}

uint8_t tn_probe(void)
{
    uint8_t r[2];
    tn_type = TN_NONE;
    chip_id = (uint8_t)(rda_rd(0x00) >> 8);
    if (chip_id == RDA_ID_HI && rda_seq(r, 2)) {     /* cross-check: the sequential port ACKs too */
        tn_type = TN_RDA;
        rda_wr(0x02, R02_DHIZ | R02_DMUTE | R02_RESET | R02_ENABLE);
        wait_ticks(5);
        rda_wr(0x02, R02_RUN);
        rda_wr(0x04, R04_DE50);
        rda_wr(0x05, R05_VAL);
        wait_ticks(5);
    } else {
        chip_id = 0;
        i2c_start();
        if (i2c_wr(TEA_ADDR << 1)) tn_type = TN_TEA;
        i2c_stop();
    }
    return tn_type;
}

uint8_t tn_id(void) { return chip_id; }

void tn_tune(uint16_t f10)
{
    cur_f10 = f10;
    if (tn_type == TN_TEA) tea_tune(f10, 0);
    else if (tn_type == TN_RDA) {
        if (seeking) { seeking = 0; rda_wr(0x02, R02_RUN); }          /* stop a seek first */
        rda_wr(0x03, ((f10 - RDA_BASE) << R03_CHAN_SH) | R03_TUNE);
    }
}

void tn_seek(uint8_t up)
{
    if (tn_type != TN_RDA) return;
    seeking = 1;
    rda_wr(0x02, R02_RUN | R02_SEEK | (up ? R02_SEEKUP : 0));         /* SKMODE 0: wraps */
}

uint8_t tn_status(tn_stat_t *s)
{
    uint8_t r[5];
    s->f10 = 0; s->done = 1;
    if (tn_type == TN_RDA) {
        if (!rda_seq(r, 4)) return 0;
        s->lev = r[2] >> 3;                         /* RSSI 0..127 -> 0..15 */
        s->stereo = (r[0] & R0A_ST) != 0;
        s->station = (r[2] & R0B_FMTRUE) != 0;
        s->done = (r[0] & R0A_STC) != 0;
        s->f10 = RDA_BASE + ((((uint16_t)r[0] << 8) | r[1]) & 0x3FF);
        if (s->done) seeking = 0;
        return 1;
    }
    if (tn_type != TN_TEA || !i2c_addr((TEA_ADDR << 1) | 1)) { i2c_stop(); return 0; }
    for (s->lev = 0; s->lev < 5; s->lev++) r[s->lev] = i2c_rd(s->lev == 4);
    i2c_stop();
    s->lev = r[3] >> TEAR3_LEV_SH;
    s->stereo = (r[2] & TEAR2_STEREO) != 0;
    r[2] &= TEAR2_IF;
    s->station = s->lev >= SEEK_LEVEL && r[2] >= 0x31 && r[2] <= 0x3E;
    return 1;
}

void tn_off(void)
{
    if (tn_type == TN_TEA) tea_tune(cur_f10, 1);
    else if (tn_type == TN_RDA) rda_wr(0x02, 0);   /* ENABLE 0 (power down), DHIZ 0: outputs high-Z */
}

/* ---- RDS: one group per call when the chip has one (RDSR) ---------------------------------------- */
void rds_group(const uint8_t *r);               /* rds.c: r = 0Ah..0Fh, 12 bytes */
void rds_poll(void)
{
    uint8_t r[12], i;
    if (tn_type != TN_RDA || seeking || !i2c_addr((RDA_SEQ << 1) | 1)) { i2c_stop(); return; }
    r[0] = i2c_rd(0);
    if (!(r[0] & R0A_RDSR)) { i2c_rd(1); i2c_stop(); return; }   /* nothing new: 2 bytes only */
    for (i = 1; i < 12; i++) r[i] = i2c_rd(i == 11);
    i2c_stop();
    rds_group(r);
}
