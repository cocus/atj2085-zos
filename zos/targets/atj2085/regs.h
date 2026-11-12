/* ATJ2085 I/O ports and bits used by zos. Sources: ATJ2085X Programming Guide v3.1 (PG),
 * datasheet v1.8 (DS), lisong_rebuild/analysis/{atj2085_hw,dma_clock,mp3_driver_spec,nand_ftl,
 * audio_hw,i2c,usb_msc}.md. Names: P_xxx = port number, XXX_yyy = bit / field / value of P_xxx.
 * build.py turns every numeric "#define NAME value" here into regs.inc for the assembler, so keep
 * values plain (hex/decimal, or (a | b) of names defined above). */
#ifndef ATJ2085_REGS_H
#define ATJ2085_REGS_H

/* ---- clock, external bus ----------------------------------------------------------------- */
#define P_CLKCTL        0x00    /* MCU clock control 1 */
#define CLK_WS_MASK     0xC0    /*   external memory wait states (LCD/NAND/ROM) */
#define CLK_WS3         0xC0
#define CLK_SRC         0x10    /*   0 LOSC / 1 HOSC 24.576 MHz; with CLK2_PLLCPU: 0 = 60, 1 = 48 MHz */
#define CLK_DIV_MASK    0x0F    /*   0 = /1 */
#define CLK_LOSC        0x00    /*   standby (stock STANDBY.AP): 32.768 kHz LOSC /1, 0 wait states */
#define P_EMLO          0x01    /* external page low; NAND bus phase of window writes: */
#define EM_DATA         0x00
#define EM_CLE          0x01
#define EM_ALE          0x02
#define ROM_BANK_BROM   0x00    /* P_EMLO with EMHI_CE0_ROM: 8 KB BROM 8000-9FFF */
#define ROM_BANK_TROM   0x02    /*   21 KB TROM 8000-D3FF */
#define P_EMHI          0x02    /* external page high: bits 5-3 = chip enable */
#define EMHI_CE0_ROM    0x00    /*   with P_EMLO = ROM_BANK_*: internal ROMs at 0x8000 */
#define EMHI_CE1_NAND   0x08
#define EMHI_CE3_LCD    0x18
#define P_CLKCTL2       0x70
#define CLK2_PLLCPU     0x08    /*   MCU clock from the PLL (see CLK_SRC) */
#define CLK2_DMADIV     0x07    /*   DMA clock divider */

/* ---- 0x4000-0x7FFF window (port 05: bits 7-4 = lane halves owned by the MCU, 3-0 = lane) --- */
#define P_PAGE          0x05
#define PAGE_HALVES     0xF0
#define PAGE_LANE       0x0F
#define PAGE_IPM_L      0xC0    /* DSP program RAM lanes (both halves to the MCU) */
#define PAGE_IPM_M      0xC1
#define PAGE_IPM_H      0xC2
#define PAGE_IDM_L      0x34    /* DSP data RAM lanes */
#define PAGE_IDM_M      0x35
#define PAGE_IDM_H      0x36
#define PAGE_IDMLO_L    0x24    /* IDM low half only (DSP keeps the rest) */
#define PAGE_IDMLO_M    0x25
#define PAGE_IDMLO_H    0x26
#define PAGE_IDMHI_L    0x14    /* IDM high half only */
#define PAGE_IDMHI_M    0x15
#define PAGE_IDMHI_H    0x16
#define PAGE_ZRAM2      0x07    /* ZRAM2, every DSP RAM half with the DSP */
#define PAGE_ZRAM2_STOCK 0xF7   /* boot value: ZRAM2, every half with the MCU */

/* ---- DMA1 (0x06-0x13) / DMA2 (0x14-0x21) ---------------------------------------------------- */
#define P_DMA1_SA0      0x06
#define P_DMA1_SA1      0x07
#define P_DMA1_SA2      0x08
#define P_DMA1_SA3      0x09
#define P_DMA1_ISA      0x0A
#define P_DMA1_DA0      0x0B
#define P_DMA1_DA1      0x0C
#define P_DMA1_DA2      0x0D
#define P_DMA1_DA3      0x0E
#define P_DMA1_IDA      0x0F
#define P_DMA1_BCL      0x10    /* byte count - 1 (device-confirmed) */
#define P_DMA1_BCH      0x11
#define P_DMA1_MODE     0x12
#define P_DMA1_CMD      0x13
#define P_DMA2_SA0      0x14
#define P_DMA2_SA1      0x15
#define P_DMA2_SA2      0x16
#define P_DMA2_SA3      0x17
#define P_DMA2_ISA      0x18
#define P_DMA2_DA0      0x19
#define P_DMA2_DA1      0x1A
#define P_DMA2_DA2      0x1B
#define P_DMA2_DA3      0x1C
#define P_DMA2_IDA      0x1D
#define P_DMA2_BCL      0x1E
#define P_DMA2_BCH      0x1F
#define P_DMA2_MODE     0x20
#define P_DMA2_CMD      0x21
#define DMA_A3_ZRAM1    0x00    /* address byte 3: internal ZRAM1 */
#define DMA_A3_DSPRAM   0x40    /*   internal IPM/IDM/ZRAM2 (lane in ISA/IDA) */
#define DMA_A3_EXT      0x80    /*   external; bits 4-0 = EMA 28-24 */
#define DMA_A3_LCD      0x8C    /*   external CE3 */
#define DMA_A3_NAND     0x84    /*   external CE1 (NAND data cycles: A16/A15 = 0) */
#define DMA_LANE_IDM_L  0x04    /* ISA/IDA: DSP data RAM lane (DMA_A3_DSPRAM) */
#define DMA_LANE_IDM_M  0x05
#define DMA_LANE_IDM_H  0x06
#define DMA_MODE_WS0    0x00    /* mode: DMA bus wait states (bits 7-6); without DMA_MODE_DSP one byte
                                     per DSP word, into the IDA lane only (PCMDEC bitstream) */
#define DMA_MODE_WS1    0x40
#define DMA_MODE_WS2    0x80
#define DMA_MODE_WS3    0xC0
#define DMA_MODE_DSP    0x20    /*   DSP transfer mode (3-lane cycling) */
#define DMA_MODE_DSTDN  0x10    /*   destination counts down */
#define DMA_MODE_SRCDN  0x08
#define DMA_MODE_DSTIO  0x02
#define DMA_MODE_SRCIO  0x01
#define DMA_CMD_START   0x01    /*   start; reads 1 while running */

/* ---- interrupts ---------------------------------------------------------------------------- */
#define P_IRQST         0x26    /* master status (DSP/key W1C) */
#define P_IRQEN         0x27    /* master enable */
#define IRQ_DSP         0x01
#define IRQ_UART        0x02
#define IRQ_USB         0x04
#define IRQ_I2C         0x08
#define IRQ_DMA         0x10
#define IRQ_RTC         0x20
#define IRQ_KEY         0x40
#define IRQ_ADC         0x80

/* ---- NAND sequencer ------------------------------------------------------------------------ */
#define P_NAND_EN       0x28    /* 1 << CE */
#define NAND_EN_CE1     0x02
#define P_NAND_CEMODE   0x29
#define P_NAND_CMD      0x2A
#define NAND_READ1      0x00
#define NAND_READ2      0x30
#define NAND_STATUS     0x70
#define NAND_STOP       0x37    /* stop sequencer, release CE */
#define NAND_CEMODE_SAMSUNG 0x01 /* P_NAND_CEMODE while CE1 is selected (BREC 0x3D56) */
/* chip commands (Samsung large page; written as a CLE cycle in EM_CLE phase or to P_NAND_CMD) */
#define NAND_CBREAD     0x35    /* copy-back read confirm (after NAND_READ1 + 5 address cycles) */
#define NAND_PROG1      0x80    /* serial data input (page register cleared), then 5 address cycles */
#define NAND_RNDIN      0x85    /* random data input: 2 cycles = column; 5 = copy-back program target */
#define NAND_PROG2      0x10    /* program confirm */
#define NAND_ERASE1     0x60    /* block erase: 3 row cycles (twice = two-plane erase) */
#define NAND_ERASE2     0xD0    /* erase confirm */
#define NAND_RESET      0xFF
#define NAND_ST_FAIL    0x01    /* status: last program/erase failed */
#define NAND_ST_READY   0x40    /*   ready */
#define NAND_ST_NWP     0x80    /*   not write protected */
#define NAND_ROW0_PLANE 0x40    /* row byte 0 bit 6 = block bit 0 = plane (64 pages/block) */
#define NAND_SPARE      0x800   /* column of the 64 spare bytes (16 per 512-byte sector) */

/* ---- NAND hardware ECC (nand_ftl.md 1.5): 24-bit Hamming of the next 512 data bytes -------- */
#define P_ECC_CTL       0xCC    /* write ECC_START: hash the next 512 data cycles (CPU or DMA) */
#define ECC_START       0x10
#define ECC_BUSY        0x10    /*   reads 1 while counting; afterwards = result byte (spare +9) */
#define P_ECC_SP8       0xCD    /* result bytes in spare order: +8 = CD, +9 = CC, +10 = CF, +11 = CE */
#define P_ECC_SP9       0xCC
#define P_ECC_SP10      0xCF
#define P_ECC_SP11      0xCE

/* ---- DSP control + host interface mailbox -------------------------------------------------- */
#define P_DSPCTL        0x2E
#define DSPCTL_RUN      0x80    /*   0 = reset held */
#define DSPCTL_DUMMYS   0x70
#define DSPCTL_PHASE    0x08
#define DSPCTL_CLKHOSC  0x04
#define DSPCTL_CLKEN    0x02
#define DSPCTL_IRQ2DSPN 0x01
#define DSPCTL_INIT     0x7B    /*   reset held, DUMMYS, phase, clock on, IRQ2DSP idle (stock) */
#define P_DSPVER        0x2F
#define DSPVER_INIT     0x03
#define P_HDR0          0x30
#define P_HDR1          0x31
#define P_HDR2          0x32
#define P_HDR3          0x33
#define P_HDR4          0x34
#define P_HDR5          0x35    /* request (DSP->MCU) / reply (MCU->DSP) */

/* ---- PLLs (DSP clock, audio, 48 MHz) -------------------------------------------------------- */
#define P_PLLCTL        0x42
#define PLL_HOSCEN      0x80
#define PLL_PLL1EN      0x40
#define PLL_M48EN       0x20
#define PLL_MIPS_MASK   0x1C    /*   DSP clock step: 000 12 .. 010 36 .. 011 48 MIPS */
#define PLL_MIPS_STEP   0x04
#define PLL_MIPS_12     0x00    /*   stock StartDsp(E=0): PCMDEC */
#define PLL_MIPS_36     0x08
#define PLL_MIPS_48     0x0C
#define PLL_AUDIO_MASK  0x03    /*   11 = 11.2896 MHz (44.1k family), 00 = 12.288 MHz */
#define PLL_AUDIO_441   0x03
#define PLL_AUDIO_PLL2  0x02
#define PLL_PLL2EN      0x02    /*   PG: bit 1 = PLL2 enable, bit 0 = 44.1k/48k select */
#define PLL_STANDBY_OFF (PLL_HOSCEN | PLL_PLL1EN | PLL_PLL2EN)  /* stock standby: 42 &= ~0xC2 */

/* ---- RTC, watchdog --------------------------------------------------------------------------- */
#define P_RTCCTL        0x43    /* 7 alarm, 6 timer (10 ms) enable, 5 2 Hz IRQ enable, 4 clear, 3 load */
#define RTCCTL_STANDBY  0x04    /*   register default: alarm, timer and 2 Hz IRQ off (stock standby) */
#define RTCCTL_LATCH    0x08    /*   latch the 24-bit 2 Hz counter for reading (stock: RMW |= 8, then read 45h) */
#define RTCCTL_CLEAR    0x10    /*   clear the counter (never done by zos) */
#define P_RTCST         0x44
#define P_RTCT_LO       0x45    /* counter bits 7-0, 15-8, 23-16 (read-only, after the latch) */
#define P_RTCT_MI       0x46
#define P_RTCT_HI       0x47
#define RTC_TICK        0x02    /* 10 ms timer pending */
#define RTC_HALFSEC     0x01
#define P_WDT           0x4E
#define WDT_EN          0x80
#define WDT_CLR         0x08
#define WDT_176MS       0x00
#define WDT_OFF         0x22    /* register default: disabled (stock standby value) */

/* ---- audio: DAC, PA, mixer ------------------------------------------------------------------- */
#define P_DACCTL        0x80
#define DAC_EN          0x20
#define DAC_SDM         0x08
#define DAC_MUTE        0x02
#define DAC_INIT        0x28    /* DAC_EN | DAC_SDM, unmuted */
#define P_DACRATE       0x81    /* low nibble = rate code (mp3_driver_spec 3.2) */
#define DACRATE_48      0x00    /*   48 kHz (with PLL_AUDIO 12.288 MHz) */
#define DACRATE_8K      0x07    /*   8 kHz (12.288 MHz family) */
#define DACRATE_441     0x08    /* codes >= 8: 44.1 kHz family (PLL_AUDIO_441) */
#define P_DAC_L         0x88    /* DAC FIFO word: PCM[3:0] in bits 7-4 */
#define P_DAC_M         0x89    /*   PCM[11:4] (16-bit sample: low byte) */
#define P_DAC_H         0x8A    /*   PCM[19:12] (high byte); this write pushes the word (8 deep, L/R) */
#define P_DACFIFO       0x8B
#define DACFIFO_FULL    0x20
#define DACFIFO_RUN     0x10    /*   0 = FIFO counter in reset */
#define DACFIFO_INIT    0x18    /*   running, QU = 8 */
#define DACFIFO_CODEC   0x05    /*   c_mp3decINIT value (counter in reset) */
#define P_PAVOL         0x9E    /* 0..31 */
#define PAVOL_MAX       31
#define PAVOL_MASK      0x1F
#define P_ADCMOD        0x9F    /* ADC modulator tune (OS AI_Enable) */
#define ADCMOD_MIC      0xDA
#define ADCMOD_LINEFM   0x7A
#define P_AICTL         0xD4    /* analog inputs (audio_hw.md: SDK comments for line/FM are swapped) */
#define AICTL_PAOFF     0x80    /*   held during PA_Disable (also ADC enable) */
#define AICTL_ADC       0x80
#define AICTL_DITHER    0x08
#define AICTL_MIC       0x40
#define AICTL_LINE      0x20
#define AICTL_FM        0x10    /*   FM input stage power */
#define AICTL_MICGAIN   0x07
#define P_MIXER         0xD5    /* 7-5 ADC rate, 4-2 analog mixer unmute, 1-0 ADC source */
#define MIX_FM          0x04    /*   (OS PA_Enable: fmin D5|=04, linein 08, micin 10) */
#define MIX_LINE        0x08
#define MIX_MIC         0x10
#define MIX_SRC_MASK    0x03    /*   ADC source: */
#define MIX_SRC_MIC     0x00
#define MIX_SRC_FM      0x01
#define MIX_SRC_LINE    0x02
#define MIX_RATE_MASK   0xE0    /*   ADC rate, 0x00 = 8 kHz */
#define MIX_RATE_16     0x40    /*   16 kHz (ogg_rec_rds.md 2.3; x 0.91875 with PLL_AUDIO_441) */
#define MIX_RATE_24     0x60    /*   24 kHz, = 22.05 kHz with PLL_AUDIO_441 */
#define MIX_RATE_32     0x80    /*   32 kHz */
#define P_ADCDATA       0x9A    /* ADC FIFO: R hi, R lo [, L hi, L lo] */
#define P_ADCBUS        0x9B    /* 0 = 16 bit */
#define P_ADCPD2        0xD2    /* OS ADC_Enable sets bit 7 (undocumented) */
#define ADCPD2_ON       0x80
#define P_ADCTUNE       0xD3    /* 1-0 input gain -6/-3/0/+3 dB */
#define ADCTUNE_GAIN    0x03
#define P_ADCFIFO       0xD7
#define ADCFIFO_NE      0x20    /*   not empty (RO) */
#define ADCFIFO_RONLY   0x10    /*   right channel only */
#define ADCFIFO_SYNC    0x40
#define ADCFIFO_RUN     0x04    /*   0 = FIFO in reset */
#define ADCFIFO_OFF     0x48    /*   OS ADC_Disable value */
#define P_ADCPD9        0xD9    /* OS ADC_Enable clears bits 1-0 (undocumented) */
#define ADCPD9_MASK     0x03
#define P_AIGAIN        0xD6    /* 6-4 line gain, 2-0 FM gain */
#define AIGAIN_FM_MASK  0x07
#define AIGAIN_FM_0DB   0x05    /*   stock FMRADIO AI_SetGain(2, 5) */
#define P_PACTL         0xDD
#define PA_DAC          0x80
#define PA_EN           0x40
#define PA_ON           0x02
#define P_CHIPID_FB     0xFB    /* undocumented, v04 0xBB reply (spec 4.2) */
#define P_CHIPID_FC     0xFC
#define P_CHIPID_FD     0xFD

/* ---- pins: keys (GPIO B), I2C + LCD A0 (GPIO C), pin mode ------------------------------------- */
#define P_GPB_OE        0xF0
#define P_GPB_IE        0xF1
#define P_GPB_D         0xF2
#define KEYB_ROWS       0x66    /* phase 1 inputs: */
#define KEYB_LEFT       0x02
#define KEYB_MENU       0x04
#define KEYB_VOL        0x20
#define KEYB_RIGHT      0x40
#define KEYB_DRIVE      0x40    /* phase 2: drive B6 high, read B1 = PLAY */
#define KEYB_PLAY       0x02
#define P_GPC_EN        0xF3    /* bits 0-2 output enable, 4-6 input enable (C0..C2) */
#define P_GPC_D         0xF4
#define GPC_SCL         0x01
#define GPC_SDA         0x02
#define GPC_LCD_A0      0x04
#define P_MFP           0xEE    /* bits 7-5 pin mode */
#define MFP_MODE_MASK   0xE0
#define MFP_F1_GPIO     0x00
#define MFP_F2          0x20    /*   C0/C1 = I2C */
#define MFP_BACKLIGHT   0x02    /*   GPO_A1: LCD backlight on (power.md; on/off only, no PWM) */
#define MFP_LCD_RESET   0x01    /*   GPO_A0: LCD reset, keep 1 */
/* ---- power (lisong_rebuild/analysis/power.md) ---------------------------------------------------- */
#define P_BATT          0xD0    /* bits 3-0 battery level (free running, 1/s), 7-4 USB TX: never write */
#define BATT_LEVEL      0x0F    /*   0.90 V + 0.04 V/step (icon calibration per board: board.h) */
#define BATT_OFF        4       /*   below this (no USB cable) zos powers off (stock kernel: 4/5/6) */
#define BATT_STANDBY_MIN 3      /*   stock standby does not even poll PLAY below this */
#define P_SARADC        0xD1    /* SAR ADC (LRADC pin, remote control): bit 7 enable; off in standby */
#define SARADC_EN       0x80
#define P_PWRCTL        0xDF    /* 7-6 LBD reset level, 5-4 LBNMI level, 3-2 IBIAS, 1 VCCOUT, 0 DCOP clk */
#define PWR_LBNMI_MASK  0x30    /*   stock standby: LBNMI level 00 (0.98 V) ... */
#define PWR_DCOPCLK     0x01    /*   ... and DC-DC switching clock 200 kHz (write; reads the DCDIS pin) */
#define P_I2C_ADDR      0x71
#define P_I2C_CTL       0x7A
#define I2C_EN          0x80
#define I2C_FAST        0x40
#define I2C_START       0x04    /*   conditions self-clear when done */
#define I2C_STOP        0x08
#define I2C_ACKBIT      0x01    /*   after a byte: 0 = ACK */
#define P_I2C_ST        0x7B
#define I2C_BF          0x80
#define P_I2C_DATA      0x7C

/* ---- USB full-speed SIE (lisong_rebuild/analysis/usb_msc.md 2.1-2.5, zos_usb.md) --------------
 * Register usage as in the stock USBDISK.AP (bank 0x21) unless zos_usb.md marks it inferred.
 * Endpoint index (EPI) selects the endpoint that P_USB_EPMODE..P_USB_MPS act on; P_USB_EPIRQ and
 * P_USB_BR have one bit per EPI (bit n = EPI n). */
#define P_USB_PULLUP    0x3E    /* internal D+ pull-up (soft connect) */
#define USB_PULLUP_EN   0x80
#define P_USB_STAT      0x50
#define USBST_SUSPEND   0x80    /*   RW: enter suspend; hardware clears it on resume */
#define USBST_VBUS      0x40    /*   RO: cable / VBUS present */
#define USBST_DP        0x20    /*   RO: D+ line */
#define USBST_DM        0x10    /*   RO: D- line */
#define USBST_ACTIVE    0x08    /*   W1C: bus activity seen */
#define P_USB_ADDR      0x51    /* device address, bit 7 = enable */
#define USB_ADDR_EN     0x80
#define P_USB_IRQ       0x52    /* global status, W1C */
#define USBIRQ_RESET    0x80
#define USBIRQ_CONN     0x40
#define USBIRQ_SETUP    0x20
#define USBIRQ_WAKE     0x10
#define USBIRQ_ALL      0xFF
#define P_USB_EPIRQ     0x53    /* per-endpoint done status, W1C (bit = 1 << EPI) */
#define P_USB_SETUPST   0x57    /* SETUP status: 7 DT, 6 valid, 5 lock, 4 ACK sent, 3-0 count */
#define SETUPST_LOCK    0x20    /*   still inside the control transfer (read after IN data) */
#define SETUPST_MASK    0xDF
#define SETUPST_OK      0x58    /*   valid | ACK sent | 8 bytes, DATA0 */
#define P_USB_SETUP0    0x58    /* SETUP bytes 0..7 = ports 0x58..0x5F */
#define P_USB_SETUP1    0x59
#define P_USB_SETUP2    0x5A
#define P_USB_SETUP3    0x5B
#define P_USB_SETUP4    0x5C
#define P_USB_SETUP5    0x5D
#define P_USB_SETUP6    0x5E
#define P_USB_SETUP7    0x5F
#define P_USB_EPI       0x60    /* endpoint index: */
#define EPI_CR          0       /*   control read (EP0 IN data stage) */
#define EPI_CW          1       /*   control write (EP0 OUT data / status stage) */
#define EPI_EP1IN       2
#define EPI_EP1OUT      3
#define EPI_EP2IN       4
#define EPI_EP2OUT      5
#define EPI_EP3IN       6
#define EPI_EP3OUT      7
#define EPB_CR          0x01    /* P_USB_EPIRQ / P_USB_BR bits */
#define EPB_CW          0x02
#define EPB_EP1IN       0x04
#define EPB_EP1OUT      0x08
#define EPB_EP2IN       0x10
#define EPB_EP2OUT      0x20
#define EPB_EP3IN       0x40
#define EPB_EP3OUT      0x80
#define P_USB_EPMODE    0x61
#define EPMODE_NORMAL   0x00
#define EPMODE_STALL    0x01
#define EPMODE_CWDONE   0x02    /*   EPI_CW: status stage (stock no-data request) */
#define EPMODE_CRLAST   0x03    /*   EPI_CR: data ends with a short packet */
#define EPMODE_ISO      0x02    /*   EP1/EP2 IN or OUT: isochronous (IN: TBC bytes per IN token, no
                                     toggle; PG p.29). EP0/EP3 are 8-byte endpoints, no ISO */
#define EPMODE_ISONBR   0x04    /*   ISO IN: answer a zero-length packet while not armed (else silence) */
#define P_USB_SAH       0x62    /* DMA start offset in the USB-owned part of ZRAM2 */
#define P_USB_SAL       0x63
#define P_USB_CNTH      0x64    /* byte count: write H first (clears BR) ... */
#define P_USB_CNTL      0x65    /*   ... then L (loads the count, sets BR = armed) */
#define P_USB_MPS       0x66    /* 7 force toggle, 6 toggle value, 5-0 max packet - 1 */
#define MPS_EP0_DATA1   0xC7    /*   EP0: 8 bytes, force DATA1 */
#define MPS_EP0         0x07    /*   EP0: 8 bytes, toggle untouched */
#define MPS_BULK        0x3F    /*   64 bytes, toggle untouched */
#define MPS_BULK_DATA0  0xBF    /*   64 bytes, force DATA0 (SET_CONFIGURATION, clear halt) */
#define MPS_INT8_DATA0  0x87    /*   8 bytes, force DATA0 */
#define P_USB_BR        0x68    /* buffer ready (armed / busy), RO, bit = 1 << EPI */
#define P_USB_NAKST     0x69    /* W1C */
#define P_USB_STALLST   0x6A    /* W1C */
#define CLK2_B1_MCU     0x20    /* P_CLKCTL2: ZRAM2 bank B1 (USB 0x000-0x7FF) to the MCU, 0 = USB */
#define CLK2_B2_MCU     0x10    /*   bank B2 (USB 0x800-0xFFF) to the MCU, 0 = USB */
#define CLK2_BANKS      0x30

/* ---- fixed addresses -------------------------------------------------------------------------- */
#define EXT_WINDOW      0x8000  /* external bus window (CE from P_EMHI): LCD data / NAND cycles */
#define LCD_DATA        0x8000  /* with EMHI_CE3_LCD: every access is an LCD data/command cycle */
#define IM1_VECTOR      0x0038
#define Z80_JP          0xC3
#define WDT_REBOOT      0x88    /* WDT_EN | WDT_176MS | WDT_CLR: reset in 176 ms */

/* ---- LCD controller (SSD1789A-compatible, 132x132 RGB565, on CE3 at LCD_DATA; A0 = GPC_LCD_A0) */
#define LCD_CASET       0x15    /* column window: start, end */
#define LCD_PASET       0x75    /* row (page) window: start, end */
#define LCD_RAMWR       0x5C    /* then pixel data, 2 bytes each (big-endian RGB565) */
#define LCD_DATCTL      0xBC    /* data control: p1, 0x01, 0x00 */
#define LCD_VOLCTR      0x81    /* contrast (electronic volume): p1 = CONTRAST_BASE + v, p2 */
#define LCD_DISOFF      0xAE    /* display off (stock UI_LCD_Sleep, then 250 ms) */
#define LCD_DISON       0xAF    /* display on, then 20 ms, then ... */
#define LCD_SLPOUT      0x94    /*   ... sleep out, 250 ms */
#define CONTRAST_BASE   0x19    /*   stock UI driver: v 0..38, saved default 6 */
#define CONTRAST_P2     0x07
#define DATCTL_ROWINV   0x01
#define DATCTL_COLINV   0x02
#define DATCTL_YFIRST   0x04
#define DATCTL_ZOS      0x03    /* zos: screen (x,y) = window (x, y + LCD_ROW0), row-major */
#define DATCTL_STOCK    0x02
#define LCD_ROW0        4       /* first visible controller row (panel mounted at 180 degrees) */

/* ---- C access (SDCC __sfr) -------------------------------------------------------------------- */
#if defined(__SDCC) && !defined(REGS_NO_SFR)
#define SFR(name, port) __sfr __at(port) name
#elif !defined(REGS_NO_SFR)
#define SFR(name, port) extern volatile unsigned char name   /* editors / static analysis */
#endif
#endif
