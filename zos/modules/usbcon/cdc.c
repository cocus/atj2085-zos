/* USB CDC-ACM debug console: a COM port on the PC (Windows: usbser.sys, no driver needed).
 * Data: EP1 OUT (host -> device, one 64-byte packet at a time at USB offset ZO_RX) and EP2 IN
 * (device -> host from ZO_TX). Notification EP 0x81 is never armed (always NAK), which is allowed.
 * Class requests: SET/GET_LINE_CODING (stored, ignored: there is no real UART), SET_CONTROL_LINE_STATE
 * (DTR shown on the LCD), SEND_BREAK.
 *
 * Received packet length: the stock code never receives short bulk packets of unknown length, so
 * the length is taken from the data: the receive area is zero-filled before arming and the text
 * ends at the first NUL (fine for a text console). The SIE's byte counter read-back after a packet
 * (64h/65h, meaning not established) is kept in cdc_cbc and shown by "usb" so it can be checked on
 * the device - a binary protocol (DFU-style, see zos_usb.md) needs it.
 *
 * Console commands (numbers are hex, "0x" optional):
 *   help | ver | usb | peek ADDR [LEN [PAGE]] | poke ADDR VAL | in PORT | out PORT VAL
 *   sec LBA (user-disk block hexdump) | fw N (firmware-area sector hexdump) | reset (reboot)
 *   wr [on|off] (mass-storage writes, default off) */
#include <stdint.h>
#include <string.h>
#include "../common/usb/usb.h"
#include "../../core/config.h"
#include "board.h"

SFR(EPIRQ, P_USB_EPIRQ);
SFR(BR, P_USB_BR);
SFR(EPI, P_USB_EPI);
SFR(CNTH, P_USB_CNTH);
SFR(CNTL, P_USB_CNTL);
SFR(WDT, P_WDT);

uint16_t cdc_nrx, cdc_ndrop;
uint8_t cdc_dtr;
static uint16_t cdc_cbc;
static uint8_t armed, hello;
static uint16_t txn;                      /* bytes waiting in ZR->tx */
static char line[64];
static uint8_t llen, lastc;
static uint8_t coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };    /* 115200 8N1 */

/* ---- output ------------------------------------------------------------------------------------ */
/* send ZR->tx to the host. Never a multiple of 64 in one transfer (no zero-length packet needed).
 * A host that does not read (no terminal open) gets the output dropped after 1 s. */
static void flush(void)
{
    uint16_t n;
    while (txn) {
        n = txn;
        if (!(n & 63)) n--;
        zr_put(ZO_TX, ZR->tx, n);
        ep_arm(EPI_EP2IN, EPMODE_NORMAL, MPS_BULK, ZO_TX, n);
        if (!ep_wait(EPB_EP2IN, W_SVC, 100)) {
            ep_disarm(EPI_EP2IN);
            txn = 0;
            cdc_ndrop++;
            return;
        }
        EPIRQ = EPB_EP2IN;
        if (txn < n) return;          /* a SETUP served meanwhile reset the console */
        txn -= n;
        memmove(ZR->tx, ZR->tx + n, txn);
    }
}

static void putc_(char c)
{
    if (txn == sizeof ZR->tx) flush();
    ZR->tx[txn++] = c;
}

static void puts_(const char *s) { while (*s) putc_(*s++); }
static void nl(void) { putc_('\r'); putc_('\n'); }

static void hex(uint32_t v, uint8_t digits)
{
    while (digits--) putc_("0123456789ABCDEF"[(uint8_t)(v >> (digits * 4)) & 15]);
}

/* 16 bytes per line: "ADDR: xx .. xx  ascii" */
static void dump(const uint8_t *p, uint16_t addr, uint16_t n)
{
    uint8_t i, k;
    while (n) {
        k = n > 16 ? 16 : (uint8_t)n;
        hex(addr, 4); puts_(": ");
        for (i = 0; i < 16; i++) {
            if (i < k) { hex(p[i], 2); putc_(' '); } else puts_("   ");
        }
        putc_(' ');
        for (i = 0; i < k; i++) putc_(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
        nl();
        p += k; addr += k; n -= k;
        hal_watchdog();
    }
}

/* ---- commands ------------------------------------------------------------------------------- */
static char *tok;

static uint8_t arg(uint32_t *v)          /* next hex number of the line, 0 = none */
{
    uint8_t c, any = 0;
    while (*tok == ' ') tok++;
    if (tok[0] == '0' && tok[1] == 'x') tok += 2;
    *v = 0;
    for (;; tok++) {
        c = *tok;
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
        else break;
        *v = (*v << 4) | c;
        any = 1;
    }
    return any && (*tok == ' ' || !*tok);
}

static uint8_t word(const char *w)
{
    uint8_t n = strlen(w);
    if (strncmp(tok, w, n) || (tok[n] && tok[n] != ' ')) return 0;
    tok += n;
    return 1;
}

static void usage(const char *s) { puts_("usage: "); puts_(s); nl(); }

static void dec(uint16_t v)
{
    char t[6];
    uint8_t i = 5;
    t[5] = 0;
    do { t[--i] = '0' + v % 10; v /= 10; } while (v && i);
    puts_(t + i);
}

/* battery ADC (power.md): port D0 bits 3-0, 0.90 V + 0.04 V/step, new value about once a second */
static void cmd_batt(void)
{
    uint8_t raw = port_in(P_BATT), l = raw & BATT_LEVEL;
    puts_("battery: port D0 = "); hex(raw, 2);
    puts_("  level "); dec(l); puts_("/15 = ~"); dec(900 + 40 * l); puts_(" mV");
    nl();
    puts_("icon scale (board.h): battery "); dec(BATT_EMPTY_BAT); putc_('-'); dec(BATT_FULL_BAT);
    puts_(", USB power "); dec(BATT_EMPTY_USB); putc_('-'); dec(BATT_FULL_USB);
    puts_("; debounced state "); hex(HAL_BATT, 2); nl();
    puts_("USB power (port 50 bit 6): "); puts_(port_in(P_USB_STAT) & USBST_VBUS ? "yes" : "no");
    puts_("  (the level reads the charger while it is plugged in)"); nl();
}

/* copy from the internal ROMs: P_EMHI = CE0 and P_EMLO = bank map BROM (0) / TROM (2) at 0x8000
 * (atj2085_hw.md 5). This module runs at 0x4000-0x7FFF and dst is in ZRAM1, so the window can be
 * switched; interrupts off meanwhile (nothing may touch the LCD), then back to the LCD (CE3, data). */
static void rom_copy(uint8_t bank, uint16_t a, uint8_t *dst, uint16_t n)
{
    __asm__("di");
    port_out(P_EMHI, EMHI_CE0_ROM);
    port_out(P_EMLO, bank);
    memcpy(dst, (const void *)a, n);
    port_out(P_EMLO, EM_DATA);
    port_out(P_EMHI, EMHI_CE3_LCD);
    __asm__("ei");
}

static void cmd_cfg(void)
{
    puts_("settings at "); hex(ZCFG_ADDR, 4); puts_(": magic "); hex(ZCFG->magic, 2);
    puts_(ZCFG->magic == ZCFG_MAGIC ? " ok" : " NOT SET"); nl();
    puts_("  lang "); dec(ZCFG->lang); puts_("  contrast "); dec(ZCFG->contrast);
    puts_("  backlight "); dec(ZCFG->backlight); puts_("  flags "); dec(ZCFG->flags); nl();
}

static const char help[] =
    "zos USB debug console (numbers in hex)\r\n"
    "  ver                      version / disk info\r\n"
    "  usb                      USB controller registers + counters\r\n"
    "  batt                     battery ADC level / voltage, USB power\r\n"
    "  cfg                      zos settings (language, contrast, backlight)\r\n"
    "  rom BANK ADDR [LEN]      internal ROM dump (0 = BROM 8000-9FFF, 2 = TROM 8000-D3FF)\r\n"
    "  peek ADDR [LEN [PAGE]]   memory dump (PAGE: read 4000-7FFF of that page)\r\n"
    "  poke ADDR VAL            write a byte\r\n"
    "  in PORT / out PORT VAL   I/O ports (out 05 refused: it unmaps this module)\r\n"
    "  sec LBA                  user disk block (= FAT volume sector)\r\n"
    "  fw N                     firmware area sector (FWIMAGE.FW)\r\n"
    "  wr [on|off]              mass-storage writes (default on, 24 MHz only)\r\n"
    "  reset                    reboot the player (watchdog)\r\n";

static void cmd_wr(void)
{
    puts_("mass-storage writes "); puts_(msc_wr ? "ON" : "off"); nl();   /* (next USBDISK session) */
}

static void cmd_usb(void)
{
    uint8_t p;
    puts_("ports 50-6A:");
    for (p = P_USB_STAT; p <= P_USB_STALLST; p++) {
        if (!((p - P_USB_STAT) & 7)) { nl(); hex(p, 2); putc_(':'); }
        putc_(' ');
        hex(port_in(p), 2);
    }
    nl();
    puts_("3E="); hex(port_in(P_USB_PULLUP), 2);
    puts_(" 70="); hex(port_in(P_CLKCTL2), 2);
    puts_(" 42="); hex(port_in(P_PLLCTL), 2); nl();
    puts_("state "); hex(usb_state, 2); puts_(" setups "); hex(usb_nsetup, 4);
    puts_(" resets "); hex(usb_nreset, 4); nl();
    puts_("ctrl-out ok "); hex(usb_lcok, 2); puts_(" timeout "); hex(usb_lcfail, 2);
    puts_("  rx pkts "); hex(cdc_nrx, 4); puts_(" last cbc "); hex(cdc_cbc, 4);
    puts_(" tx drops "); hex(cdc_ndrop, 4); nl();
    puts_("line coding "); hex(*(uint32_t *)coding, 8); puts_(" baud, dtr "); hex(cdc_dtr, 2); nl();
}

/* mass-storage event log, oldest first (format: usb.h logent_t) */
static void cmd_log(void)
{
    uint8_t i, n = usb_logn, p = (usb_logpos - usb_logn) & (LOG_N - 1);
    logent_t *e;
    puts_("mass-storage log, "); hex(n, 2); puts_(" entries (hex)"); nl();
    for (i = 0; i < n; i++, p = (p + 1) & (LOG_N - 1)) {
        e = &usb_logv[p];
        hex(i, 2); puts_("  ");
        switch (e->k) {
        case 'R': puts_("bus reset #"); hex(e->w, 4); break;
        case 'U': puts_("cable out"); break;
        case 'S':
            puts_("SETUP "); hex(e->a, 2); putc_(' '); hex(e->b, 2);
            puts_(" vh "); hex(e->c, 2); puts_(" len "); hex(e->w, 4);
            if (e->a == 0xFF) puts_(" BAD SETUP STATUS");
            break;
        case 'C': case 'F':
            puts_("SCSI "); hex(e->a, 2);
            puts_(e->k == 'C' ? " ok  " : " FAIL");
            if (e->a == 0x28 || e->a == 0x2A) {
                puts_(" lba "); hex(e->w, 4); puts_(" n "); hex(e->c, 2);
                if (e->k == 'F') { puts_(" sense "); hex(e->b, 2); }
            } else {
                puts_(" len "); hex(e->w, 4);
                if (e->k == 'F' || e->a == 0x03) { puts_(" sense "); hex(e->b, 2); putc_('/'); hex(e->c, 2); }
            }
            break;
        case 'I': puts_("INVALID CBW "); hex(e->a, 2); hex(e->b, 2); hex(e->c, 2); hex(e->w, 2); break;
        case 'A':
            puts_("SCSI "); hex(e->a, 2); puts_(e->b == 1 ? " ABORTED in data" : " ABORTED in CSW");
            puts_(e->c == AB_RESET ? " by bus reset" : e->c == AB_UNPLUG ? " by unplug" : " by MENU");
            break;
        case 'H': puts_("halt ep "); hex(e->a, 2); puts_(e->b ? " set" : " cleared"); break;
        default: putc_(e->k);
        }
        nl();
    }
}

static void execute(void)
{
    uint32_t a, b, c;
    uint8_t ok;
    xfer_t x;
    tok = line;
    while (*tok == ' ') tok++;
    if (!*tok) return;
    if (word("help") || word("?")) puts_(help);
    else if (word("ver")) {
        puts_("zos USBCON module 0.2, CDC-ACM console, 1209:0002 (pid.codes test PID)"); nl();
        puts_("disk: last LBA "); hex(msc_last, 8); puts_(" (512-byte blocks), RAM 008A = ");
        hex(*(volatile uint16_t *)0x008A, 4); puts_(", boot sector total "); hex(msc_boot_total, 8); nl();
        puts_("READ(10) path "); puts_(msc_rdmode ? "1 (stock ping-pong)" : "0 (flat)"); nl();
        cmd_wr();
    }
    else if (word("wr")) {
        while (*tok == ' ') tok++;
        if (word("on")) {
            if (port_in(P_CLKCTL2) & CLK2_PLLCPU) { puts_("refused: CPU at 48 MHz"); nl(); }
            else msc_wr = 1;
        } else if (word("off")) msc_wr = 0;
        cmd_wr();
    }
    else if (word("usb")) cmd_usb();
    else if (word("batt")) cmd_batt();
    else if (word("cfg")) cmd_cfg();
    else if (word("log")) {
        while (*tok == ' ') tok++;
        if (word("clear")) { usb_logn = usb_logpos = 0; puts_("log cleared"); nl(); }
        else cmd_log();
    }
    else if (word("rdmode")) {
        if (arg(&a)) msc_rdmode = a ? 1 : 0;
        puts_("READ(10) path "); hex(msc_rdmode, 1); nl();
    }
    else if (word("peek")) {
        if (!arg(&a)) { usage("peek ADDR [LEN [PAGE]]"); return; }
        if (!arg(&b)) b = 16;
        if (b > 512) b = 512;
        if (arg(&c)) {                    /* another page: copy through the core (ZRAM1 code) */
            if (a < PAGE_BASE || a + b > PAGE_BASE + PAGE_SIZE) { puts_("ADDR must be 4000-7FFF"); nl(); return; }
            x.dst = (uint16_t)ZR->sec; x.src = (const void *)(uint16_t)a; x.len = (uint16_t)b;
            page_read((uint8_t)c, &x);
            dump(ZR->sec, (uint16_t)a, (uint16_t)b);
        } else dump((const uint8_t *)(uint16_t)a, (uint16_t)a, (uint16_t)b);
    }
    else if (word("rom")) {                /* internal ROMs through the 0x8000 window, CE0 */
        if (!arg(&c) || !arg(&a)) { usage("rom BANK ADDR [LEN]   (bank 0 = BROM 8000-9FFF, 2 = TROM 8000-D3FF)"); return; }
        if (!arg(&b)) b = 16;
        if (b > 512) b = 512;
        if (a < EXT_WINDOW || a + b > 0x10000UL) { puts_("ADDR must be 8000-FFFF"); nl(); return; }
        rom_copy((uint8_t)c, (uint16_t)a, ZR->sec, (uint16_t)b);
        dump(ZR->sec, (uint16_t)a, (uint16_t)b);
    }
    else if (word("poke")) {
        if (!arg(&a) || !arg(&b)) { usage("poke ADDR VAL"); return; }
        *(volatile uint8_t *)(uint16_t)a = (uint8_t)b;
    }
    else if (word("in")) {
        if (!arg(&a)) { usage("in PORT"); return; }
        hex(a, 2); puts_(" = "); hex(port_in((uint8_t)a), 2); nl();
    }
    else if (word("out")) {
        if (!arg(&a) || !arg(&b)) { usage("out PORT VAL"); return; }
        if ((uint8_t)a == P_PAGE) { puts_("refused: port 05 maps this module's code"); nl(); return; }
        port_out((uint8_t)a, (uint8_t)b);
    }
    else if (word("sec") || word("fw")) {
        ok = tok[-1] == 'w';
        if (!arg(&a)) { usage(ok ? "fw N" : "sec LBA"); return; }
        if (!(ok ? hal_fw_read((uint16_t)a, ZR->sec) : hal_blk_read(a, ZR->sec))) {
            puts_("read failed"); nl(); return;
        }
        dump(ZR->sec, 0, 512);
    }
    else if (word("reset")) {
        puts_("rebooting"); nl();
        flush();
        WDT = WDT_REBOOT;                 /* watchdog fires in 176 ms */
        for (;;) ;
    }
    else { puts_("unknown command, try help"); nl(); }
}

static void prompt(void) { puts_("zos> "); }

static void input(char c)
{
    if (c == '\n' && lastc == '\r') { lastc = c; return; }       /* CR LF = one Enter */
    lastc = c;
    if (c == '\r' || c == '\n') {
        nl();
        line[llen] = 0;
        execute();
        llen = 0;
        prompt();
    } else if (c == 8 || c == 127) {
        if (llen) { llen--; puts_("\b \b"); }
    } else if (c >= 32 && c < 127 && llen < sizeof line - 1) {
        if (c >= 'A' && c <= 'Z') c += 32;
        line[llen++] = c;
        putc_(c);                                                 /* echo */
    }
}

/* ---- class hooks ---------------------------------------------------------------------------- */
void cdc_reset(void) { armed = 0; txn = 0; llen = 0; cdc_dtr = 0; }

/* receive area zero-filled, EP1 OUT armed for one packet */
void cdc_configured(void)
{
    zr_zero(ZO_RX, 64);
    ep_arm(EPI_EP1OUT, EPMODE_NORMAL, MPS_BULK, ZO_RX, 64);
    armed = 1;
}

uint8_t cdc_setup(void)
{
    uint8_t rt = usb_su[0], rq = usb_su[1];
    if (rt == 0x21 && rq == 0x20) {                /* SET_LINE_CODING (7 bytes OUT) */
        if (ep0_out(7)) memcpy(coding, ZR->ep0, 7);
        return 1;
    }
    if (rt == 0xA1 && rq == 0x21) { ep0_reply(coding, 7); return 1; }     /* GET_LINE_CODING */
    if (rt == 0x21 && rq == 0x22) {                /* SET_CONTROL_LINE_STATE: bit 0 = DTR */
        uint8_t d = usb_su[2] & 1;
        if (d && !cdc_dtr) hello = 1;              /* terminal opened: greet it */
        cdc_dtr = d;
        usb_dirty = 1;
        ep0_ack_class();
        return 1;
    }
    if (rt == 0x21 && rq == 0x23) { ep0_ack_class(); return 1; }          /* SEND_BREAK */
    return 0;
}

void cdc_poll(void)
{
    uint8_t i, *r = ZR->rx;
    if (usb_state != US_CONFIGURED) return;
    if (!armed) { cdc_configured(); return; }
    if (hello) { hello = 0; nl(); puts_("zos USB console - type help"); nl(); prompt(); flush(); }
    if (!(EPIRQ & EPB_EP1OUT) && (BR & EPB_EP1OUT)) return;
    EPIRQ = EPB_EP1OUT;
    armed = 0;
    EPI = EPI_EP1OUT;
    cdc_cbc = ((uint16_t)CNTH << 8) | CNTL;        /* diagnostic only (meaning not established) */
    zr_get(r, ZO_RX, 64);
    cdc_nrx++;
    usb_dirty = 1;
    for (i = 0; i < 64 && r[i]; i++) input(r[i]);
    flush();
    cdc_configured();
}
