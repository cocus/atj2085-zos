/* ATJ2085 I/O ports as C register variables (SDCC __sfr): ONE declaration per port, named like the
 * port define without the "P_" (P_PAVOL -> PAVOL). Included by regs.h; C only (regs.inc, the
 * assembler include, is generated from regs.h and never sees this file). Declarations that a
 * translation unit does not use cost no code.
 *
 * RULES (also in regs.h and README.md "Hardware registers"):
 *  - modules / core / hal / apps never write SFR(...) or __sfr themselves: #include "regs.h" and
 *    use these names. New port: add `#define P_xxx 0x..` to regs.h and one SFR line here
 *    (tools/check_sfr.py fails the build when an SFR / __sfr declaration shows up elsewhere);
 *  - bit/field constants (PAVOL_MAX, DAC_EN, ...) are in regs.h; they never equal a register name;
 *  - a module-local alias is fine only when it adds meaning (#define LCD_A0_PORT GPC_D).
 *
 * ACCESS HAZARDS (register semantics the declarations cannot express; details at each register in
 * regs.h and lisong_rebuild/analysis/):
 *  - MFP (0xEE): the GPO_A bits are backlight / LCD reset. zos reads it ONCE into a shadow
 *    (platform.c mfp_shadow) and writes the shadow modified; never read-modify-write it live.
 *  - BATT (0xD0): bits 3-0 level (free running); bits 7-4 are USB TX: NEVER write.
 *  - DMAIRQ (0x25): W1C; NEVER write bit 0 (DMA reset pulse). IRQST / USB_IRQ / USB_EPIRQ /
 *    USB_NAKST / USB_STALLST / USB_STAT bit 3 are write-1-to-clear: write only the bits to clear.
 *  - RTCT_LO/MI/HI are read-only and valid only after the RTCCTL_LATCH; ECC_CTL reads back the
 *    result byte (= ECC_SP9) after counting; DAC_H: the write pushes the FIFO word, write L, M, H
 *    in that order; USB_CNTH then USB_CNTL (CNTL arms the endpoint).
 *  - WDT / DSPCTL / DCDC: zos only reads where the module comment says so.
 *  - several names may share an address (ECC_CTL / ECC_SP9 = 0xCC): different roles, same port. */
#ifndef ATJ2085_SFR_H
#define ATJ2085_SFR_H
#ifndef REGS_NO_SFR

/* ---- clock, external bus ----------------------------------------------------------- */
SFR(CLKCTL, P_CLKCTL);            /* MCU clock control 1 */
SFR(EMLO, P_EMLO);                /* external page low; NAND bus phase of window writes (EM_DATA/CLE/ALE) */
SFR(EMHI, P_EMHI);                /* external page high: bits 5-3 = chip enable */
SFR(CLKCTL2, P_CLKCTL2);

/* ---- 0x4000-0x7FFF window (port 05: bits 7-4 = lane halves owned by the MCU, 3-0 = lane) --- */
SFR(PAGE, P_PAGE);

/* ---- DMA1 (0x06-0x13) / DMA2 (0x14-0x21) ------------------------------------------- */
SFR(DMA1_SA0, P_DMA1_SA0);
SFR(DMA1_SA1, P_DMA1_SA1);
SFR(DMA1_SA2, P_DMA1_SA2);
SFR(DMA1_SA3, P_DMA1_SA3);        /* 7 ext, 6 IPM/IDM/ZRAM2 select, 4-0 SA[28:24] */
SFR(DMA1_ISA, P_DMA1_ISA);
SFR(DMA1_DA0, P_DMA1_DA0);
SFR(DMA1_DA1, P_DMA1_DA1);
SFR(DMA1_DA2, P_DMA1_DA2);
SFR(DMA1_DA3, P_DMA1_DA3);        /* same layout; CE3 = 011 in DA[28:26] -> 0x8C with bit 7 (external) */
SFR(DMA1_IDA, P_DMA1_IDA);
SFR(DMA1_BCL, P_DMA1_BCL);        /* byte count - 1 (device-confirmed) */
SFR(DMA1_BCH, P_DMA1_BCH);
SFR(DMA1_MODE, P_DMA1_MODE);      /* 7-6 wait states, 5 DSP mode, 4/3 down-count, 2 DSP boot, 1/0 IO */
SFR(DMA1_CMD, P_DMA1_CMD);        /* 0 = start (self-clearing) */
SFR(DMA2_SA0, P_DMA2_SA0);
SFR(DMA2_SA1, P_DMA2_SA1);
SFR(DMA2_SA2, P_DMA2_SA2);
SFR(DMA2_SA3, P_DMA2_SA3);        /* (same layout as DMA1) */
SFR(DMA2_ISA, P_DMA2_ISA);
SFR(DMA2_DA0, P_DMA2_DA0);
SFR(DMA2_DA1, P_DMA2_DA1);
SFR(DMA2_DA2, P_DMA2_DA2);
SFR(DMA2_DA3, P_DMA2_DA3);
SFR(DMA2_IDA, P_DMA2_IDA);
SFR(DMA2_BCL, P_DMA2_BCL);
SFR(DMA2_BCH, P_DMA2_BCH);
SFR(DMA2_MODE, P_DMA2_MODE);
SFR(DMA2_CMD, P_DMA2_CMD);

/* ---- interrupts -------------------------------------------------------------------- */
SFR(DMAIRQ, P_DMAIRQ);            /* DMA status: bit 3 = DMA1 end (W1C); NEVER write bit 0 (DMA reset pulse) */
SFR(IRQST, P_IRQST);              /* master status (DSP/key W1C) */
SFR(IRQEN, P_IRQEN);              /* master enable */

/* ---- NAND sequencer ---------------------------------------------------------------- */
SFR(NAND_EN, P_NAND_EN);          /* 1 << CE */
SFR(NAND_CEMODE, P_NAND_CEMODE);
SFR(NAND_CMD, P_NAND_CMD);

/* ---- NAND hardware ECC (nand_ftl.md 1.5): 24-bit Hamming of the next 512 data bytes --- */
SFR(ECC_CTL, P_ECC_CTL);          /* write ECC_START: hash the next 512 data cycles (CPU or DMA) */
SFR(ECC_SP8, P_ECC_SP8);          /* result bytes in spare order: +8 = CD, +9 = CC, +10 = CF, +11 = CE */
SFR(ECC_SP9, P_ECC_SP9);
SFR(ECC_SP10, P_ECC_SP10);
SFR(ECC_SP11, P_ECC_SP11);

/* ---- DSP control + host interface mailbox ------------------------------------------ */
SFR(DSPCTL, P_DSPCTL);
SFR(DSPVER, P_DSPVER);
SFR(HDR0, P_HDR0);
SFR(HDR1, P_HDR1);
SFR(HDR2, P_HDR2);
SFR(HDR3, P_HDR3);
SFR(HDR4, P_HDR4);
SFR(HDR5, P_HDR5);                /* request (DSP->MCU) / reply (MCU->DSP) */

/* ---- PLLs (DSP clock, audio, 48 MHz) ----------------------------------------------- */
SFR(PLLCTL, P_PLLCTL);            /* DSP clock; bits 1-0 select the DAC PLL family (OS DAC_SetRate); 7 HOSCEN, 6 PLL1EN, 5 M48EN, 4-2 DSP clock */

/* ---- RTC, watchdog ----------------------------------------------------------------- */
SFR(RTCCTL, P_RTCCTL);            /* 7 alarm, 6 timer (10 ms) enable, 5 2 Hz IRQ enable, 4 clear, 3 load */
SFR(RTCST, P_RTCST);
SFR(RTCT_LO, P_RTCT_LO);          /* counter bits 7-0, 15-8, 23-16 (read-only, after the latch) */
SFR(RTCT_MI, P_RTCT_MI);
SFR(RTCT_HI, P_RTCT_HI);
SFR(WDT, P_WDT);
SFR(DCDC, P_DCDC);                /* DC-DC converter control/status (only read by zos: PERFTEST) */

/* ---- audio: DAC, PA, mixer --------------------------------------------------------- */
SFR(DACCTL, P_DACCTL);            /* 5 DACEN, 3 SDM, 1 mute */
SFR(DACRATE, P_DACRATE);          /* low nibble = rate code (mp3_driver_spec 3.2) */
SFR(DAC_L, P_DAC_L);              /* DAC FIFO word: PCM[3:0] in bits 7-4 */
SFR(DAC_M, P_DAC_M);              /*   PCM[11:4] (16-bit sample: low byte) */
SFR(DAC_H, P_DAC_H);              /*   PCM[19:12] (high byte); this write pushes the word (8 deep, L/R) */
SFR(DACFIFO, P_DACFIFO);
SFR(PAVOL, P_PAVOL);              /* 0..31: 4-0 headphone volume, 1.8 dB/step */
SFR(ADCMOD, P_ADCMOD);            /* ADC modulator tune (OS AI_Enable) */
SFR(AICTL, P_AICTL);              /* analog inputs: 7 ADC en, 6 MIC, 5 line, 4 FM stage, 3 dither, 2-0 mic gain (audio_hw.md: SDK comments for line/FM are swapped) */
SFR(MIXER, P_MIXER);              /* 7-5 ADC rate, 4-2 analog mixer unmute, 1-0 ADC source */
SFR(ADCDATA, P_ADCDATA);          /* ADC FIFO: R hi, R lo [, L hi, L lo] */
SFR(ADCBUS, P_ADCBUS);            /* 0 = 16 bit */
SFR(ADCPD2, P_ADCPD2);            /* OS ADC_Enable sets bit 7 (undocumented) */
SFR(ADCTUNE, P_ADCTUNE);          /* 1-0 input gain -6/-3/0/+3 dB */
SFR(ADCFIFO, P_ADCFIFO);          /* 5 not-empty (RO), 4 right only, 2 FIFO not-reset, 7/3 IRQ en/pending */
SFR(ADCPD9, P_ADCPD9);            /* OS ADC_Enable clears bits 1-0 (undocumented) */
SFR(AIGAIN, P_AIGAIN);            /* 6-4 line gain, 2-0 FM gain */
SFR(PACTL, P_PACTL);              /* 7 DAC analog, 6 mixer+PA enable, 1-0 PA bias */
SFR(CHIPID_FB, P_CHIPID_FB);      /* undocumented, v04 0xBB reply (spec 4.2) */
SFR(CHIPID_FC, P_CHIPID_FC);
SFR(CHIPID_FD, P_CHIPID_FD);

/* ---- pins: keys (GPIO B), I2C + LCD A0 (GPIO C), pin mode -------------------------- */
SFR(GPB_OE, P_GPB_OE);
SFR(GPB_IE, P_GPB_IE);
SFR(GPB_D, P_GPB_D);
SFR(GPC_EN, P_GPC_EN);            /* bits 0-2 output enable, 4-6 input enable (C0..C2); FM tuner I2C pins (see i2c.md) */
SFR(GPC_D, P_GPC_D);              /* data bits C0..C2 (C2 = LCD A0: keep it when writing C0/C1) */
SFR(MFP, P_MFP);                  /* bits 7-5 pin mode */

/* ---- power (lisong_rebuild/analysis/power.md) -------------------------------------- */
SFR(BATT, P_BATT);                /* bits 3-0 battery level (free running, 1/s), 7-4 USB TX: never write */
SFR(SARADC, P_SARADC);            /* SAR ADC (LRADC pin, remote control): bit 7 enable; off in standby */
SFR(PWRCTL, P_PWRCTL);            /* 7-6 LBD reset level, 5-4 LBNMI level, 3-2 IBIAS, 1 VCCOUT, 0 DCOP clk */
SFR(I2C_ADDR, P_I2C_ADDR);
SFR(I2C_CTL, P_I2C_CTL);
SFR(I2C_ST, P_I2C_ST);
SFR(I2C_DATA, P_I2C_DATA);

/* ---- USB full-speed SIE (lisong_rebuild/analysis/usb_msc.md 2.1-2.5, zos_usb.md) --- */
SFR(USB_PULLUP, P_USB_PULLUP);    /* internal D+ pull-up (soft connect) */
SFR(USB_STAT, P_USB_STAT);
SFR(USB_ADDR, P_USB_ADDR);        /* device address, bit 7 = enable */
SFR(USB_IRQ, P_USB_IRQ);          /* global status, W1C */
SFR(USB_EPIRQ, P_USB_EPIRQ);      /* per-endpoint done status, W1C (bit = 1 << EPI) */
SFR(USB_SETUPST, P_USB_SETUPST);  /* SETUP status: 7 DT, 6 valid, 5 lock, 4 ACK sent, 3-0 count */
SFR(USB_SETUP0, P_USB_SETUP0);    /* SETUP bytes 0..7 = ports 0x58..0x5F */
SFR(USB_SETUP1, P_USB_SETUP1);
SFR(USB_SETUP2, P_USB_SETUP2);
SFR(USB_SETUP3, P_USB_SETUP3);
SFR(USB_SETUP4, P_USB_SETUP4);
SFR(USB_SETUP5, P_USB_SETUP5);
SFR(USB_SETUP6, P_USB_SETUP6);
SFR(USB_SETUP7, P_USB_SETUP7);
SFR(USB_EPI, P_USB_EPI);          /* endpoint index: */
SFR(USB_EPMODE, P_USB_EPMODE);
SFR(USB_SAH, P_USB_SAH);          /* DMA start offset in the USB-owned part of ZRAM2 */
SFR(USB_SAL, P_USB_SAL);
SFR(USB_CNTH, P_USB_CNTH);        /* byte count: write H first (clears BR) ... */
SFR(USB_CNTL, P_USB_CNTL);        /*   ... then L (loads the count, sets BR = armed) */
SFR(USB_MPS, P_USB_MPS);          /* 7 force toggle, 6 toggle value, 5-0 max packet - 1 */
SFR(USB_BR, P_USB_BR);            /* buffer ready (armed / busy), RO, bit = 1 << EPI */
SFR(USB_NAKST, P_USB_NAKST);      /* W1C */
SFR(USB_STALLST, P_USB_STALLST);  /* W1C */

#endif /* REGS_NO_SFR */
#endif
