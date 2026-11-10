/* newbrec USB mass storage: Bulk-Only Transport + a minimal SCSI set. ONE LUN = the whole NAND as raw
 * 512-byte data sectors in physical order: LBA = block * 256 + page * 4 + sector (64 pages of 4 sectors
 * per block). READ(10) reads the data columns of the page (no ECC check, bad blocks included).
 * WRITE(10): the first sector of a block (page 0, sector 0) erases the block first; each sector is then
 * programmed with the hardware ECC and the BREC-style spare (FF x8, ECC x4, FF FF 55 FF), so the host
 * must write a block's sectors in ascending order. The FTL markers of the stock format (spare bytes
 * 0-7) are NOT written by this path (M1: documented limitation). READ path = the stock ping-pong of
 * the two USB buffer banks (device-verified in zos), data phases always move exactly
 * dCBWDataTransferLength bytes. Own code, derived from zos/modules/usbdisk/msc.c. */
#include <stdint.h>
#include <string.h>
#include "usb.h"

SFR(CLKCTL2, P_CLKCTL2);
SFR(EPIRQ, P_USB_EPIRQ);
SFR(BR, P_USB_BR);

uint8_t msc_ejected, msc_lastop, msc_laststatus;
uint16_t msc_ncmd, msc_nrd, msc_nwr;
static uint8_t armed, sense_key, sense_asc, cbw[32], tx[40], sec[512];
static uint32_t last, dlen;

#define SK_NOT_READY 0x02
#define SK_MEDIUM    0x03
#define SK_ILLEGAL   0x05
#define ASC_NO_MEDIUM 0x3A
#define ASC_READ_ERR  0x11
#define ASC_WRITE_ERR 0x0C
#define ASC_BAD_OP    0x20
#define ASC_BAD_LBA   0x21
#define ASC_BAD_FIELD 0x24

static const uint8_t inquiry[36] = {
    0x00, 0x80, 0x02, 0x02, 31, 0, 0, 0,
    'Z', 'O', 'S', ' ', ' ', ' ', ' ', ' ',
    'N', 'E', 'W', 'B', 'R', 'E', 'C', ' ', 'R', 'A', 'W', ' ', 'N', 'A', 'N', 'D',
    '0', '.', '1', '0'
};

void msc_set_geometry(uint32_t blocks) { last = blocks * 256 - 1; }
void msc_reset(void) { armed = 0; }

void msc_configured(void)
{
    ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, 64);
    armed = 1;
}

uint8_t msc_setup(void)
{
    uint8_t rq = usb_su[1], z = 0;
    if (usb_su[0] == 0x21 && rq == 0xFF) { ep0_ack_class(); return 1; }     /* BOT reset */
    if (usb_su[0] == 0xA1 && rq == 0xFE) { ep0_reply(&z, 1); return 1; }    /* GET_MAX_LUN = 0 */
    return 0;
}

static void fail(uint8_t key, uint8_t asc) { sense_key = key; sense_asc = asc; }

static void send_in(uint16_t n)
{
    ep_arm(EPI_EP2IN, EPMODE_NORMAL, MPS_BULK, ZO_TX, n);
    ep_wait(EPB_EP2IN, 0);
    EPIRQ = EPB_EP2IN;
}

/* data-in phase: `valid` bytes from tx, then zeros, `len` bytes in total */
static void data_in(uint16_t valid, uint32_t len)
{
    uint16_t k, v;
    uint8_t m;
    while (len) {
        k = len > 2048 ? 2048 : (uint16_t)len;
        v = valid < k ? valid : k;
        if (v) zr_put(ZO_TX, tx, v);
        if (k > v) {
            m = CLKCTL2;
            CLKCTL2 = m | CLK2_BANKS;
            memset((void *)(PAGE_BASE + ZO_TX + v), 0, k - v);
            CLKCTL2 = m;
        }
        valid -= v;
        send_in(k);
        len -= k;
    }
}

static void data_out(uint32_t len)
{
    uint16_t k;
    while (len) {
        k = len > 2048 ? 2048 : (uint16_t)len;
        ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, k);
        ep_wait(EPB_EP1OUT, 0);
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

static void rowcol(uint32_t lba)
{
    uint32_t row = lba >> 2;
    nreq.c0 = 0;
    nreq.c1 = (uint8_t)(lba & 3) << 1;           /* column = 512 * sector */
    nreq.r0 = (uint8_t)row;
    nreq.r1 = (uint8_t)(row >> 8);
    nreq.r2 = (uint8_t)(row >> 16);
}

/* READ(10): the MCU fills the bank it owns (seen at 0x4000) with up to 4 sectors while the SIE sends
 * the other bank (USB offset 0), then the banks swap. Sectors of one page are read in one NAND read. */
static uint8_t read10(uint32_t lba, uint16_t cnt)
{
    uint8_t n, j, k, ok = 1, first = 1;
    CLKCTL2 = (CLKCTL2 & ~CLK2_BANKS) | CLK2_B2_MCU;    /* B1 to USB, MCU fills B2 */
    while (cnt) {
        n = cnt > 4 ? 4 : (uint8_t)cnt;
        for (j = 0; j < n; j += k) {
            k = 4 - (uint8_t)(lba & 3);
            if (k > n - j) k = n - j;
            rowcol(lba);
            nreq.buf = PAGE_BASE + (uint16_t)j * 512;
            nreq.len = (uint16_t)k * 512;
            nand_read();
            lba += k;
        }
        tgt_wdt();
        if (!first) ep_wait(EPB_EP2IN, 0);
        CLKCTL2 ^= CLK2_BANKS;                          /* swap: the filled bank goes to USB */
        ep_arm(EPI_EP2IN, EPMODE_NORMAL, MPS_BULK, 0, (uint16_t)n * 512);
        first = 0;
        cnt -= n;
        msc_nrd += n;
    }
    if (!first) ep_wait(EPB_EP2IN, 0);
    EPIRQ = EPB_EP2IN;
    CLKCTL2 &= ~CLK2_BANKS;
    return ok;
}

/* WRITE(10): up to 4 sectors per transfer into B1, then erase-on-first-sector + program each */
static uint8_t write10(uint32_t lba, uint16_t cnt)
{
    uint8_t n, i, ok = 1;
    while (cnt) {
        n = cnt > 4 ? 4 : (uint8_t)cnt;
        ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, (uint16_t)n * 512);
        ep_wait(EPB_EP1OUT, 0);
        EPIRQ = EPB_EP1OUT;
        for (i = 0; i < n; i++, lba++) {
            zr_get(sec, ZO_RX + i * 512, 512);
            if (!ok) continue;
            rowcol(lba);
            if (!(uint8_t)lba) {                        /* page 0, sector 0 of a block: erase it */
                if (nand_erase()) { ok = 0; continue; }
                rowcol(lba);
            }
            nreq.buf = (uint16_t)sec;
            if (nand_prog()) ok = 0;
        }
        tgt_wdt();
        cnt -= n;
        msc_nwr += n;
    }
    return ok;
}

static void command(void)
{
    uint8_t *cdb = cbw + 15, op = cdb[0], flags = cbw[12], status = 0, done = 0, w;
    uint16_t valid = 0, cnt;
    uint32_t lba;
    memcpy(&dlen, cbw + 8, 4);
    msc_ncmd++;
    if (msc_ejected && op != 0x12 && op != 0x03) {
        fail(SK_NOT_READY, ASC_NO_MEDIUM);
        status = 1;
        op = 0xFF;
    }
    switch (op) {
    case 0xFF:
    case 0x00: case 0x1E: case 0x2F: case 0x35:    /* TEST UNIT READY, PREVENT, VERIFY, SYNC CACHE */
    case 0x15: case 0x55:                          /* MODE SELECT: data ignored */
        break;
    case 0x1B:                                     /* START STOP UNIT: eject */
        if ((cdb[4] & 3) == 2) { msc_ejected = 1; usb_dirty = 1; }
        break;
    case 0x12:                                     /* INQUIRY */
        if (cdb[1] & 1) { fail(SK_ILLEGAL, ASC_BAD_FIELD); status = 1; break; }
        memcpy(tx, inquiry, valid = sizeof inquiry);
        break;
    case 0x03:                                     /* REQUEST SENSE */
        memset(tx, 0, 18);
        tx[0] = 0x70; tx[2] = sense_key; tx[7] = 10; tx[12] = sense_asc;
        sense_key = sense_asc = 0;
        valid = 18;
        break;
    case 0x25:                                     /* READ CAPACITY(10) */
        put_be32(tx, last);
        put_be32(tx + 4, 512);
        valid = 8;
        break;
    case 0x23:                                     /* READ FORMAT CAPACITIES */
        memset(tx, 0, 12);
        tx[3] = 8;
        put_be32(tx + 4, last + 1);
        tx[8] = 0x02;
        tx[10] = 0x02;
        valid = 12;
        break;
    case 0x1A:                                     /* MODE SENSE(6): header only, not protected */
        tx[0] = 3; tx[1] = tx[2] = tx[3] = 0;
        valid = 4;
        break;
    case 0x5A:                                     /* MODE SENSE(10) */
        memset(tx, 0, 8);
        tx[1] = 6;
        valid = 8;
        break;
    case 0x28:                                     /* READ(10) */
    case 0x2A:                                     /* WRITE(10) */
        w = op == 0x2A;
        lba = be32(cdb + 2);
        cnt = ((uint16_t)cdb[7] << 8) | cdb[8];
        if (lba > last || cnt > last - lba + 1) { fail(SK_ILLEGAL, ASC_BAD_LBA); status = 1; break; }
        if (!(flags & 0x80) != w || dlen < (uint32_t)cnt * 512) { fail(SK_ILLEGAL, ASC_BAD_FIELD); status = 1; break; }
        if (!cnt) break;
        if (!(w ? write10(lba, cnt) : read10(lba, cnt))) { fail(SK_MEDIUM, w ? ASC_WRITE_ERR : ASC_READ_ERR); status = 1; }
        dlen -= (uint32_t)cnt * 512;
        if (!dlen) done = 1;
        break;
    default:
        fail(SK_ILLEGAL, ASC_BAD_OP);
        status = 1;
        break;
    }
    if (!done && dlen) {
        if (flags & 0x80) data_in(status ? 0 : valid, dlen);
        else data_out(dlen);
    }
    memcpy(tx, "USBS", 4);                          /* CSW: signature, tag, residue, status */
    memcpy(tx + 4, cbw + 4, 4);
    if (status) memcpy(tx + 8, cbw + 8, 4); else memset(tx + 8, 0, 4);
    tx[12] = status;
    zr_put(ZO_TX, tx, 13);
    send_in(13);
    msc_lastop = cdb[0];
    msc_laststatus = status;
}

/* idle: wait for a CBW on EP1 OUT */
void msc_poll(void)
{
    if (usb_state != US_CONFIGURED || usb_halt) return;
    if (!armed) { msc_configured(); return; }
    if (!(EPIRQ & EPB_EP1OUT) && (BR & EPB_EP1OUT)) return;
    EPIRQ = EPB_EP1OUT;
    armed = 0;
    zr_get(cbw, ZO_RX, 31);
    if (cbw[0] != 'U' || cbw[1] != 'S' || cbw[2] != 'B' || cbw[3] != 'C') {
        usb_stall_bulk();
        return;
    }
    command();
    usb_dirty = 1;
    msc_configured();
}
