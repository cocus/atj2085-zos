/* USB mass storage, Bulk-Only Transport, SCSI subset. Read-only unless the console's "wr on" set
 * msc_wr: then WRITE(10) goes through the module-side FTL write path (../common/ftlw.c); the open
 * unit is closed (data in place) by SYNCHRONIZE CACHE, eject, 1 s without writes, a READ(10)
 * touching it, and when the module disconnects.
 * One LUN = the zos block device: host LBA n = hal_blk_read(n) = stock UD sector 0x200 + n (the
 * FAT volume's sector 0), exactly the stock USBDISK mapping (Compute_LBA b21:205a).
 * Capacity as the stock driver reports it (f_b20_16e4): last LBA = 256 * UD_GetCap - 0x8000 - 0x210,
 * UD_GetCap = flash size in 128 KB units (BREC's flash detect, RAM 0x008A = 0x1EC0 on this
 * player -> last LBA 1 981 936, i.e. 1 981 937 blocks = the sector count in the FAT boot sector).
 * Write protection like the stock firmware for a protected LUN: MODE SENSE reports WP, WRITE(10)
 * receives (drains) the data and fails with DATA PROTECT (lsSendCSWInWriteFlash b21:1cfa).
 * Like the stock code every data phase moves exactly dCBWDataTransferLength bytes in the
 * direction the host asked for (replies zero-padded / truncated), so there are no phase errors. */
#include <stdint.h>
#include <string.h>
#include "../common/usb/usb.h"
#include "../common/ftlw.h"

SFR(CLKCTL2, P_CLKCTL2);
SFR(EPIRQ, P_USB_EPIRQ);
SFR(BR, P_USB_BR);

/* msc_wr (writes, default on: user request; console "wr off" / 48 MHz turn them off) and msc_rdmode
 * (1 = stock ping-pong: Windows mounts (device); 0 = flat: Windows looped on LBA 0) live in the USB
 * shared block (usbsh.h, set by the launcher / console); msc_last, msc_boot_total: usbdev.c */
uint16_t msc_ncmd, msc_nsec;
uint8_t msc_ejected;
static uint16_t twr;      /* hal_ticks() of the last WRITE(10) */
uint8_t msc_lastop, msc_laststatus, msc_stage;
static uint8_t armed, sense_key, sense_asc, curop;
static uint32_t dlen;                     /* dCBWDataTransferLength */
static uint8_t flags;                     /* bCBWFlags: bit 7 = data to the host */

/* sense codes (stock canned records at b21:2847: none, 0B/4E, 07/27, 06/28, 02/3A) */
#define SK_NOT_READY   0x02
#define SK_MEDIUM      0x03
#define SK_ILLEGAL     0x05
#define SK_PROTECT     0x07
#define ASC_NO_MEDIUM  0x3A
#define ASC_READ_ERR   0x11
#define ASC_WRITE_ERR  0x0C
#define ASC_BAD_OP     0x20
#define ASC_BAD_LBA    0x21
#define ASC_BAD_FIELD  0x24
#define ASC_WP         0x27

static const uint8_t inquiry[36] = {
    0x00, 0x80, 0x02, 0x02, 31, 0, 0, 0,          /* disk, REMOVABLE (superfloppy: no MBR), SPC-0 */
    'z', 'o', 's', ' ', ' ', ' ', ' ', ' ',
    'U', 'S', 'B', ' ', 'D', 'I', 'S', 'K', ' ', 'R', 'O', ' ', ' ', ' ', ' ', ' ',
    '0', '.', '1', '0'
};

void msc_reset(void) { armed = 0; }

/* EP1 OUT armed for the next CBW (sPrepare_EP1_Out b21:21d7, at USB offset ZO_RX) */
void msc_configured(void)
{
    ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, 64);
    armed = 1;
}

uint8_t msc_setup(void)
{
    uint8_t rq = usb_su[1];
    if (usb_su[0] == 0x21 && rq == 0xFF) {        /* Bulk-Only Mass Storage Reset (b21:1a71) */
        ep0_ack_class();
        return 1;
    }
    if (usb_su[0] == 0xA1 && rq == 0xFE) {        /* GET_MAX_LUN: one LUN */
        uint8_t z = 0;
        ep0_reply(&z, 1);
        return 1;
    }
    return 0;
}

static void fail(uint8_t key, uint8_t asc) { sense_key = key; sense_asc = asc; }

/* bulk IN `n` bytes at ZO_TX (f_b21_1f5c / sWait_Ep2_Send_Out b21:204f) */
static void send_in(uint16_t n)
{
    ep_arm(EPI_EP2IN, EPMODE_NORMAL, MPS_BULK, ZO_TX, n);
    ep_wait(EPB_EP2IN, 0, 0);
    EPIRQ = EPB_EP2IN;
}

/* data-in phase: `valid` bytes from ZR->tx, then zeros, `len` bytes in total */
static void data_in(uint16_t valid, uint32_t len)
{
    uint16_t k, v;
    while (len) {
        k = len > ZO_MAX ? ZO_MAX : (uint16_t)len;
        v = valid < k ? valid : k;
        if (v) zr_put(ZO_TX, ZR->tx, v);
        if (k > v) zr_zero(ZO_TX + v, k - v);
        valid -= v;
        send_in(k);
        len -= k;
    }
}

/* WRITE(10): up to 4 blocks per transfer into B1 (ZO_RX), each copied to ZRAM1 (DMA source) and
 * written. All data is received even after a failure. Returns 0 if a block write failed. */
static uint8_t write10(uint32_t lba, uint16_t cnt)
{
    uint8_t n, i, ok = 1;
    while (cnt) {
        n = cnt > 4 ? 4 : (uint8_t)cnt;
        ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, (uint16_t)n * 512);
        ep_wait(EPB_EP1OUT, 0, 0);
        EPIRQ = EPB_EP1OUT;
        for (i = 0; i < n; i++) {
            zr_get(ZR->sec, ZO_RX + i * 512, 512);
            if (ok && ftlw_write(lba, ZR->sec)) ok = 0;
            lba++;
        }
        hal_watchdog();
        cnt -= n;
        msc_nsec += n;
    }
    twr = hal_ticks();
    return ok;
}

/* data-out phase: receive and drop `len` bytes (MODE SELECT, WRITE on a protected disk) */
static void data_out(uint32_t len)
{
    uint16_t k;
    while (len) {
        k = len > ZO_MAX ? ZO_MAX : (uint16_t)len;
        ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, k);
        ep_wait(EPB_EP1OUT, 0, 0);
        EPIRQ = EPB_EP1OUT;
        len -= k;
    }
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint16_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}

/* READ(10), default "flat" path: only the buffer discipline the console has proven on the device
 * (MCU owns both banks while filling, USB owns both while sending, USB offset ZO_TX = B2): up to 4
 * blocks are read straight into B2 (CPU address PAGE_BASE + ZO_TX), then sent as one transfer.
 * No overlap of NAND reads and USB transfers. Returns 0 if a block read failed. */
static uint8_t read10_flat(uint32_t lba, uint16_t cnt)
{
    uint8_t n, i, ok = 1;
    while (cnt) {
        n = cnt > 4 ? 4 : (uint8_t)cnt;
        CLKCTL2 |= CLK2_BANKS;                          /* B1 at 0x4000, B2 at 0x4800 */
        for (i = 0; i < n; i++)
            if (!blk_to_zram2(lba++, PAGE_BASE + ZO_TX + i * 512)) ok = 0;
        CLKCTL2 &= ~CLK2_BANKS;                         /* both banks to USB */
        hal_watchdog();
        send_in((uint16_t)n * 512);
        cnt -= n;
        msc_nsec += n;
    }
    return ok;
}

/* READ(10), optional (console "rdmode 1"): the stock 2 KB ping-pong (f_b21_1bbc): the MCU fills the bank it owns (seen at
 * PAGE_BASE) with up to 4 blocks while the SIE sends the other one (USB offset 0 = the bank USB
 * owns), then the banks swap (sInvXYMemMapping b21:1c36). Returns 0 if a block read failed. */
static uint8_t read10_pp(uint32_t lba, uint16_t cnt)
{
    uint8_t n, i, ok = 1, first = 1;
    CLKCTL2 = (CLKCTL2 & ~CLK2_BANKS) | CLK2_B2_MCU;    /* B1 to USB, MCU fills B2 */
    while (cnt) {
        n = cnt > 4 ? 4 : (uint8_t)cnt;
        for (i = 0; i < n; i++)
            if (!blk_to_zram2(lba++, PAGE_BASE + i * 512)) ok = 0;
        hal_watchdog();
        if (!first) ep_wait(EPB_EP2IN, 0, 0);           /* previous 2 KB gone */
        CLKCTL2 ^= CLK2_BANKS;                          /* swap: the filled bank goes to USB */
        ep_arm(EPI_EP2IN, EPMODE_NORMAL, MPS_BULK, 0, (uint16_t)n * 512);
        first = 0;
        cnt -= n;
        msc_nsec += n;
    }
    if (!first) ep_wait(EPB_EP2IN, 0, 0);
    EPIRQ = EPB_EP2IN;
    CLKCTL2 &= ~CLK2_BANKS;                             /* both banks to USB again */
    return ok;
}

static void command(void)
{
    uint8_t *cbw = ZR->cbw, *cdb = cbw + 15, *t = ZR->tx, op = cdb[0];
    uint16_t valid = 0;
    uint32_t lba;
    uint16_t cnt;
    uint8_t status = 0, done = 0, lk, la, w;
    uint16_t lw;
    memcpy(&dlen, cbw + 8, 4);                         /* little endian, as the Z80 */
    flags = cbw[12];
    msc_ncmd++;
    curop = op;
    lw = (uint16_t)dlen;
    lk = la = 0;
    if (msc_ejected && op != 0x12 && op != 0x03) {      /* after eject: "medium not present" */
        fail(SK_NOT_READY, ASC_NO_MEDIUM);
        status = 1;
        op = 0xFF;                                     /* no command action */
    }
    switch (op) {
    case 0xFF:
        break;
    case 0x00:                                         /* TEST UNIT READY */
    case 0x1E:                                         /* PREVENT/ALLOW MEDIUM REMOVAL */
    case 0x2F:                                         /* VERIFY(10) */
        break;
    case 0x1B:                                         /* START STOP UNIT: LoEj + !Start = eject */
        if ((cdb[4] & 3) == 2) { msc_ejected = 1; usb_dirty = 1; }
        /* fall through: data in place */
    case 0x35:                                         /* SYNCHRONIZE CACHE(10) */
        if (ftlw_flush()) { fail(SK_MEDIUM, ASC_WRITE_ERR); status = 1; }
        break;
    case 0x12:                                         /* INQUIRY */
        if (cdb[1] & 1) { fail(SK_ILLEGAL, ASC_BAD_FIELD); status = 1; break; }   /* no VPD pages */
        memcpy(t, inquiry, valid = sizeof inquiry);
        if (msc_wr) { t[25] = 'R'; t[26] = 'W'; }
        break;
    case 0x03:                                         /* REQUEST SENSE (fixed format, 18 bytes) */
        memset(t, 0, 18);
        t[0] = 0x70; t[2] = sense_key; t[7] = 10; t[12] = sense_asc;
        lk = sense_key; la = sense_asc;                /* logged: what the host was told */
        sense_key = sense_asc = 0;
        valid = 18;
        break;
    case 0x25:                                         /* READ CAPACITY(10) */
        put_be32(t, msc_last);
        put_be32(t + 4, 512);
        valid = 8;
        break;
    case 0x23:                                         /* READ FORMAT CAPACITIES */
        memset(t, 0, 12);
        t[3] = 8;
        put_be32(t + 4, msc_last + 1);
        t[8] = 0x02;                                   /* formatted media */
        t[10] = 0x02;                                  /* block length 0x000200 */
        valid = 12;
        break;
    case 0x1A:                                         /* MODE SENSE(6): header only, WP unless msc_wr */
        t[0] = 3; t[1] = 0; t[2] = msc_wr ? 0 : 0x80; t[3] = 0;
        valid = 4;
        break;
    case 0x5A:                                         /* MODE SENSE(10) */
        memset(t, 0, 8);
        t[1] = 6; t[3] = msc_wr ? 0 : 0x80;
        valid = 8;
        break;
    case 0x15:                                         /* MODE SELECT(6/10): data ignored */
    case 0x55:
        break;
    case 0x28:                                         /* READ(10) */
    case 0x2A:                                         /* WRITE(10) (protected: drain, DATA PROTECT) */
        w = op == 0x2A;
        lba = be32(cdb + 2);
        cnt = ((uint16_t)cdb[7] << 8) | cdb[8];
        lw = (uint16_t)lba; la = (uint8_t)cnt;
        if (w && !msc_wr) { fail(SK_PROTECT, ASC_WP); status = 1; break; }
        if (lba > msc_last || cnt > msc_last - lba + 1) { fail(SK_ILLEGAL, ASC_BAD_LBA); status = 1; break; }
        if (!(flags & 0x80) != w || dlen < (uint32_t)cnt * 512) { fail(SK_ILLEGAL, ASC_BAD_FIELD); status = 1; break; }
        if (!cnt) break;
        if (!w && ftlw_sync(lba, cnt)) { fail(SK_MEDIUM, ASC_READ_ERR); status = 1; break; }   /* zeros below */
        msc_stage = 1;
        if (!(w ? write10(lba, cnt) : msc_rdmode ? read10_pp(lba, cnt) : read10_flat(lba, cnt))) {
            fail(SK_MEDIUM, w ? ASC_WRITE_ERR : ASC_READ_ERR);
            status = 1;
        }
        dlen -= (uint32_t)cnt * 512;                   /* (a longer host length: zeros below) */
        if (!dlen) done = 1;
        break;
    default:
        fail(SK_ILLEGAL, ASC_BAD_OP);
        status = 1;
        break;
    }
    if (!done && dlen) {
        msc_stage = 1;
        if (flags & 0x80) data_in(status ? 0 : valid, dlen);
        else data_out(dlen);
    }
    if (status) { lk = sense_key; la = sense_asc; }
    msc_stage = 2;
    /* CSW: "USBS", tag, residue, status. Failure: residue = dCBWDataTransferLength (sFillErrCSW) */
    t = ZR->csw;
    t[0] = 'U'; t[1] = 'S'; t[2] = 'B'; t[3] = 'S';
    memcpy(t + 4, cbw + 4, 4);
    if (status) memcpy(t + 8, cbw + 8, 4); else memset(t + 8, 0, 4);
    t[12] = status;
    zr_put(ZO_TX, t, 13);
    send_in(13);
    msc_stage = 0;
    msc_lastop = curop;
    msc_laststatus = status;
    usb_log(status ? 'F' : 'C', curop, lk, la, lw);
}

/* a bus reset / unplug / MENU interrupted a command (longjmp out of a wait) */
void msc_abort_note(uint8_t why)
{
    if (msc_stage) usb_log('A', curop, msc_stage, why, 0);
    msc_stage = 0;
}

/* idle: wait for a CBW on EP1 OUT (EP1_OUT_ISR b21:16c2 is triggered by 53h bit 3) */
void msc_poll(void)
{
    uint8_t *c = ZR->cbw;
    if (ftlw_pending() && (uint16_t)(hal_ticks() - twr) >= HAL_TICK_HZ) ftlw_flush();   /* idle */
    if (usb_state != US_CONFIGURED || usb_halt) return;
    if (!armed) { msc_configured(); return; }
    if (!(EPIRQ & EPB_EP1OUT) && (BR & EPB_EP1OUT)) return;
    EPIRQ = EPB_EP1OUT;
    armed = 0;
    zr_get(c, ZO_RX, 31);
    if (c[0] != 'U' || c[1] != 'S' || c[2] != 'B' || c[3] != 'C') {
        usb_log('I', c[0], c[1], c[2], c[3]);
        usb_stall_bulk();                              /* until the host's reset recovery */
        return;
    }
    command();
    usb_dirty = 1;                                     /* counters on the LCD */
    msc_configured();                                  /* next CBW */
}
