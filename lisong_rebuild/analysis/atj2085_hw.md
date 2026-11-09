# ATJ2085(X) Hardware Reference (for firmware reverse-engineering)

Sources and citation keys:

| Key | Document | Notes |
|-----|----------|-------|
| **PG** | `ATJ2085\ATJ2085X Programing Guide v3[1].1.pdf` (v3.1, 03/2006) | Primary. Page numbers below are the **printed** "Page N" footers; the PDF page index is N+1. The file `Chinese Files\bought_back\ATJ2085 Programing GuideV2.9.pdf` is actually v1.5 (08/2004). A text diff shows it is the same register content (only renumbered chapters and cosmetic changes), so nothing extra is in it. |
| **DS** | `ATJ2085\ATJ2085X datasheet v1.8.pdf` | Printed page numbers. |
| **PG87 / PG89** | `Chinese Files\bought_back\ATJ2087\...V1.2.pdf`, `ATJ2089\...V1.2.pdf` | Sibling chips. Used **only** where the 2085X guide is silent (MMC controller). Marked as such. |
| **S6B** | `ATJ2085\S6B33B0A.PDF` | Samsung S6B33B0A LCD driver. Instruction table on printed p.28. |
| **SSD** | `ATJ2085\SSD1789A-1.PDF` | Solomon SSD1789A. Command tables on pp.29-35 of 69. Pulled in because the firmware's LCD command set matches this chip, not the S6B33B (see §6). |
| **SCH** | `ATJ2085\atj2085-mp4--schematic_compress.pdf` | Reference board (mono NT7502 LCM, K9K1G08 NAND). |
| **TXT** | `ATJ2085\atj2085 stuff.txt` | Earlier notes: GPIOC2 = LCD D/C, GPIOA0 = LCD reset, an annotated SSD1789 init. |

Anything marked **(inferred)** is not stated in the documents.

---

## 0. Chip summary (PG p.3-5, DS p.4-5, 10-11)

- MCU: 8-bit, "instruction set compatible with Z80", 24.576 MHz typical, up to 60 MHz (DS p.11).
- DSP: 24-bit Harvard core, up to 72 MIPS. PM = 16K x 24. DM = (16K-256) x 24 (DS p.11). The block diagram calls these IPM (16Kx24 SRAM) and IDM (16Kx24 SRAM) (PG p.5). The diagram also shows a DSP **HIP** (Host Interface Port), the buses DSP PABUS/DABUS/DBBUS/PBBUS, and "DSP EDMWR-/EDMRD-" for DSP external data memory.
- On-chip memory: ZRAM1 16Kx8 and ZRAM2 4Kx8 (B1 2K + B2 2K) for the MCU, plus IPM and IDM. That totals "116 KB SRAM". ROM: BROM 8Kx8 and TROM 21Kx8, totalling "29 KB ROM" (DS p.11).
- The 2085X has **no SD/MMC controller, no UART registers, no SPDIF registers and no internal LCD controller documented**. The 2087/2089 have these. See §6 and §7.
- External bus: D[7:0], MWR-, MRD-, CLE, ALE, CE1-/CE2-/CE3- (pins 36-38). On the 64-pin part there are **no external address pins**, so external devices are addressed only through CE plus CLE/ALE.
  - CE1- = "Boot up Nand Flash chip enable (must be connected to Nand Flash)"
  - CE2- = "Nand Flash chip enable (optional)"
  - **CE3- = "8080 interface LCM chip enable"** (PG p.8, DS p.8)

---

## 1. Memory maps

### 1.1 Z80 (MCU) 64 KB logical space (PG p.21-22 figure + text, DS p.11 figure)

| MCU address | Size | Content | Selection |
|---|---|---|---|
| `0000-3FFF` | 16 KB | **ZRAM1** (also called B0) | Always (IA15=0, IA14=0) |
| `4000-7FFF` | 16 KB window | One of: **IPM-H / IPM-M / IPM-L** (16K each, byte lanes of the 24-bit PM), **IDM-H / IDM-M / IDM-L**, or **ZRAM2** (B1+B2, 2K+2K) at `4000-4FFF` | Port **05h** bits 7-4 (which 8K halves of IPM/IDM go to the MCU instead of the DSP) and bits 2-0 (which byte lane, or ZRAM2). ZRAM2 also needs port **70h** bits 5-4. |
| `8000-FFFF` | 32 KB window | **External memory / ROM bank** ("Extended Memory Space, MCU.A15=1"), shown as BANK0, BANK1, BANK2 ... of 32 KB each | Ports **01h** (EMA22-15) and **02h** (EMA28-23). **03h** adds a signed increment to 02:01. |
| `8000-9FFF` (bank 0) | 8 KB | **BROM** | IA15=1, Reg02=00h, Reg01=00h |
| `8000-D3FF` (bank 2) | 21 KB | **TROM** | IA15=1, Reg02=00h, Reg01=02h |

External address formation (PG p.11):

- `EMA[14:0]` = MCU A14..A0.
- `EMA[22:15]` = port 01h.
- `EMA[28:23]` = port 02h bits 5-0.
- `EMA[28:26]` (port 02h bits 5,4,3) decode the chip enable:

  | EMA28:26 | Port 02h value | Chip enable |
  |---|---|---|
  | 000 | 0x00 | CE0- |
  | 001 | 0x08 | CE1- |
  | 010 | 0x10 | CE2- |
  | 011 | 0x18 | CE3- |
  | 1xx | | reserved |

  Note that 0x18 selects CE3-, the 8080 LCM chip enable.
- "CE0- ONLY for external random access device, like MROM/NorFlash/Sram etc. CE1/2/3- can access both MROM and Nand Flash" (PG p.12).
- Port 03h (write-only): `New(02:01) = Old(02:01) + Reg03h`, where Reg03h is two's complement (PG p.12).

ZRAM2 mapping (port 70h bits 5-4, PG p.10-11):

| 70h[5:4] | MCU sees at `4000h` |
|---|---|
| 00 | nothing (both banks belong to the USB SIE) |
| 01 | B2 at `4000-47FF` |
| 10 | B1 at `4000-47FF` |
| 11 | B1+B2 at `4000-4FFF` |

Port 05h must also be `xxxxx111b` (PG p.13: "If bit5-4 of reg70h is equal to 00, MCU can't access ZRAM2 even bit2-0 is 111").

ZRAM2 is meant for ping-pong buffering (PG p.22). For example: the MCU runs in B0 while USB DMA reads B1 and DMA1/2 writes B2, then the roles swap.

### 1.2 Port 05h: Internal MROM/SRAM page register (PG p.12-13)

| Bit | Meaning |
|---|---|
| 7 | 0 = low 8K block of IPM mapped to DSP, 1 = mapped to MCU |
| 6 | 0 = high 8K block of IPM mapped to DSP, 1 = mapped to MCU |
| 5 | 0 = low 8K block of IDM mapped to DSP, 1 = mapped to MCU |
| 4 | 0 = high 8K block of IDM mapped to DSP, 1 = mapped to MCU |
| 3 | Concurrent CPU/DMA mode for IPM program swap. When DMA SRC is external memory and DST is IPM/IDM/FB, the MCU keeps running from ZRAM and IO 00h-2Fh while the DMA runs. |
| 2-0 | Lane select: 000 IPM low byte, 001 IPM mid, 010 IPM high, 011 reserved, 100 IDM low, 101 IDM mid, 110 IDM high, **111 ZRAM2** |

Values given in the PG p.13 table:

- One 8K half per value: `80/81/82h` = low 8K of IPM (L/M/H lane), `40/41/42h` = high 8K of IPM, `20/21/22h` = low 8K of IDM, `10/11/12h` = high 8K of IDM.
- Both halves as 16Kx8 at `4000-7FFF`: **`C0/C1/C2h`** = IPM L/M/H, **`30/31/32h`** = IDM L/M/H.
- (Inferred) With only one half mapped, the 8K most likely appears at `4000-5FFF` (low) or `6000-7FFF` (high). The figure does not state this.
- A sibling SDK (ATJ2099, `sdk_i_80h\...\startup.msa`) writes `F3h`/`F4h` to port 05 at startup, so the upper bits are not always zero in practice.

**A firmware that uses port 05h in FS/VM/NAND drivers is paging DSP RAM (or ZRAM2) into `4000-7FFF` to use it as MCU buffer space.**

### 1.3 DSP memory spaces

| Space | Size | DSP-side address (inferred) | Notes |
|---|---|---|---|
| PM (IPM) | 16K x 24 | `0000-3FFF` | Program memory. From the MCU side it is three byte lanes (L/M/H) via port 05h, or via DMA in "DSP mode" (§3). |
| DM (IDM) | (16K-256) x 24 usable | `0000-3EFF` (inferred) | DS p.11: "(16KB-256)*24bit Data Memory". The top 256 words are memory-mapped registers (inferred, see the next row). |
| DM-mapped registers | 256 words | `3F00-3FFF` (inferred range) | Documented: **`0x3FED`** = DAC PCM FIFO write (D23-D4 used, D3-0 discarded; the same FIFO as MCU ports 88/89/8Ah). **`0x3FEE`** low byte = mirror of port 8Bh (bit2 = DAC FIFO FULL) (PG p.52). |
| "XM/YM" | n/a | | The documents do not use XM/YM terms. The core is PM/DM Harvard (ADSP-21xx-like bus naming PA/DA/PB/DB, inferred). |
| External DM / TROM | 21K x 8 TROM | not established | The PG p.5 block diagram draws TROM connected to `DSP DB[23:8]`, in addition to the DSP external data-memory read/write signals. The 21 KiB TROM dump is organized as 7,168 little-endian 24-bit table words (Q23-style coefficients). This confirms a DSP-side data-bus connection; the DSP address range, read width/lane behavior, and any mapping/control register are not described. Separately, the MCU can read the same physical TROM through its banked window at `8000-D3FF` when `02h=00h, 01h=02h` (PG p.22). Do not conflate that MCU mapping with the DSP-side access. |

How the Z80 and DSP share memory:

- IPM and IDM are **owned per 8K half**. Each half is either on the DSP bus or on the MCU bus (05h bits 7-4). There is no simultaneous dual-port access.
- Data exchange paths are:
  1. The MCU maps an IDM half, reads or writes bytes lane by lane, then unmaps it.
  2. DMA1/2 to or from IPM/IDM in DSP mode.
  3. The HIP mailbox registers 30h-37h.
  4. Interrupts both ways (§3).

---

## 2. I/O port map (MCU `IN`/`OUT` space)

R/W: RW = read/write, RO = read-only, WO = write-only, W1C = write 1 to clear.

### 2.1 Bus controller, banking, clocks, DMA, CTC, IRQ, DSP (PG §5, p.9-21)

| Port | Name | R/W / default | Bit fields |
|---|---|---|---|
| **00h** | MCU Clock Control 1 | RW, 00h | 7-6: **external memory wait states** (00 = 0, 01 = 1, 10 = 2, 11 = 3). 5: reserved. 4: MCU clock source. If 70h.3=0: 0 = 32.768 kHz LOSC, 1 = 24.576 MHz HOSC. If 70h.3=1: 0 = 60 MHz (DSPCLK/4), 1 = 48 MHz. 3-0: divider (0000 /1, 0001 /2, 0010 /4 ... 1010 /1024, 1011 DC = stop, 1100 /256, 1101 /512, 1110 /1024, 1111 DC). Changes sync to the /1024 clock. Recovery from clock stop: reset, POR, key IRQ, alarm IRQ, SIRQ, or DCDIS NMI (PG p.9-10). |
| **01h** | EM Low Page | RW, 00h | 7-0: EMA22-15 (32 KB bank number, low part) (PG p.11) |
| **02h** | EM High Page | RW | 7-6: reserved. 5-0: EMA28-23. EMA28:26 decode CE0-/CE1-/CE2-/CE3- (see §1.1) (PG p.11-12) |
| **03h** | EM Page Inc/Dec | WO | Two's-complement value added to 02h:01h (PG p.12) |
| **04h** | MCU-A15 Control | RW, 00h | 7: watchdog reset occurred (W1C). 6: external reset occurred (W1C). 5: low-battery NMI status (W1C). 4: reserved. 3: DCDIS NMI flag (both edges, W1C). 2: **SIRQ- enable**. 1: **NMI enable** (LBNMI and DCDIS NMI). 0: **0 forces MCU A15=1** (boot from bank space), 1 = normal. "The boot code after power on reset must be jump 800xh followed by an IO write to 04h to set this bit" (PG p.12). |
| **05h** | Internal MROM/SRAM Page | RW, 00h | See §1.2 (PG p.12-13) |
| 06h-09h | DMA1 Source Address 0-3 | RW | 06: SA[7:0]. 07: SA[15:8]. 08: SA[23:16]. 09: bit7 = 1 external memory / 0 internal, bit6 = internal select (0 ZRAM, 1 IPM/IDM/ZRAM2), bit5 reserved, bits 4-0 = SA[28:24] (PG p.14) |
| 0Ah | DMA1 IPM/IDM/ZRAM2 SRC Address | RW | 2-0: DMA1ISA (same lane code as 05h[2:0]) |
| 0Bh-0Eh | DMA1 Destination Address 0-3 | RW | Same layout as 06-09 (0E: bit7 ext, bit6 int-sel, bits 4-0 DA[28:24]) (PG p.14-15) |
| 0Fh | DMA1 IPM/IDM/ZRAM2 DST Address | RW | 2-0: DMA1IDA |
| 10h/11h | DMA1 Byte Count L/H | RW | BC[14:0]. Maximum 32 KB (PG p.15) |
| **12h** | DMA1 Mode | RW, 00h | 7-6: wait states. **5: DSP transfer mode** (A14/A15 cycle 0,1,2 lanes, then A0-A13 increment). 4: DST down-count. 3: SRC down-count. **2: DSP BOOT**. 1: DST is IO. 0: SRC is IO (PG p.15) |
| **13h** | DMA1 Command | RW, 00h | 7: TC IRQ enable. 6: half-transfer IRQ enable. 5: continuous (auto-reload) mode. 4: priority. 3-2: external DRQ select (00 DRQ1A = UART2 TX, 01 DRQ1B reserved, 10 DRQ1C reserved, 11 DRQ1D = SPDIF TX). 1: external DRQ enable. **0: start** (rising edge loads counters, cleared at TC) (PG p.15-16) |
| 14h-17h | DMA2 Source Address 0-3 | RW | As DMA1 (PG p.16-17) |
| 18h | DMA2 IPM/IDM/ZRAM2 SRC | RW | 2-0 |
| 19h-1Ch | DMA2 Destination Address 0-3 | RW | As DMA1 |
| 1Dh | DMA2 IPM/IDM/ZRAM2 DST | RW | 2-0 |
| 1Eh/1Fh | DMA2 Byte Count L/H | RW | BC[14:0] |
| **20h** | DMA2 Mode | RW, 00h | As 12h (PG p.18) |
| **21h** | DMA2 Command | RW, 00h | As 13h. DRQ select: 00 DRQ2A = UART2 RX, **01 DRQ2B = ADC FIFO RX**, 10 reserved, 11 DRQ2D = SPDIF RX. With the ADC DRQ, the DMA clock must be MCUclk/4 (PG p.18-19) |
| 22h | CTC1 Pre-scale | RW | 7: CTC1 enable. 6-0: prescale /1../128. Clock = undivided MCU clock (PG p.19) |
| 23h/24h | CTC1 Period L/H | RW | TPERIOD[15:0] |
| **25h** | DMA/CTC IRQ Status | RW (W1C), 00h | 7: CTC IRQ pending. 6: DMA2 half. 5: DMA2 end. 4: DMA1 half. 3: DMA1 end. **2: SIRQ- pending**. 1: reserved. 0: software DMA1/DMA2 reset pulse (self-clearing) (PG p.19) |
| **26h** | Master Interrupt Status | RW, 00h | 7: ADC (RO). 6: keyboard (W1C). 5: RTC (RO). 4: DMA/CTC (RO). 3: SIRQ/I2C/SPI (RO). 2: USB (RO). 1: UART/IR/SPDIF (RO). **0: DSP (W1C)** (PG p.20) |
| **27h** | Master Interrupt Enable | RW, 00h | Same bit order: 7 ADC, 6 key, 5 RTC, 4 DMA/CTC, 3 SIRQ/I2C/SPI, 2 USB, 1 UART/IR/SPDIF, **0 DSP** (PG p.20) |
| 28h-2Dh | NAND | | See §2.3 |
| **2Eh** | DSP Control/Status | RW, **01h** | **7: DSP reset (0 = reset, 1 = run)**. 6-4: DUMMYS[0:2], IDM/IPM option select (normally 111b). 3: phase timing (0 = normal). **2: DSP clock source (0 PLL, 1 HOSC)**. **1: DSPCKEN** (0 = DSP clock stopped). **0: IRQ2DSP-, write 0 to assert IRQ2- to the DSP** (PG p.20) |
| **2Fh** | DSP Boot Mode | RW, 73h | 7-4: reserved. 3: NAND select (must be 0 when accessing NAND). 2-0: reserved (PG p.20-21) |
| **30h-35h** | HIP HDR0-HDR5 | RW | DSP Host Interface data registers (PG p.21) |
| **36h / 37h** | HIP HSR6 / HSR7 | RW | DSP Host Interface status registers (PG p.21) |
| 3Eh | USB internal pull-up | RW, 04h | 7: pull-up enable. 2-0: 1.1k..1.8k (PG p.24-25) |
| 3Fh | VDD/VCC voltage detect | RW, 00h | 7: VDD detect enable. 6: VCC detect enable. 5-3: VCC level 2.4-3.1 V. 2-0: VDD level 1.4-2.1 V (PG p.48-49) |
| 40h | HOSC control | RW, 04h | 5-4: 11 = external DAC 96 kHz fs. 3-2: oscillator GMMIN (PG p.46) |
| 41h | PLL tune | RW, 55h | 7-6: PLL1 charge pump. 5-4: PLL2 charge pump (PG p.46) |
| **42h** | PLL Control | RW, 00h | 7: HOSCEN. 6: PLL1EN. 5: M48EN (48 MHz). **4-2: DSP clock** (000 = 48 MHz/12 MIPS, 001 = 96/24, 010 = 144/36, 011 = 192/48, 100 = 240/60, 101 = 288/72, 110 = 336/84, 111 = 120/30). 1: PLL2EN. 0: FSSEL (0 = 12.288 MHz for the 48k family, 1 = 11.2896 MHz for the 44.1k family) (PG p.46-47) |
| 43h | RTC Control | RW, 04h | 7: alarm enable. 6: RTC timer enable. 5: 2 Hz IRQ enable. 4: clear counter. 3: latch counter for read. 0: overflow flag (RO) (PG p.42-43) |
| 44h | RTC IRQ Status | RW, 00h | 7: crystal OSC enable. 6: RTC clock source (1 = 32768 crystal; can only be set from 0 to 1, about 1 s after bit7). 2: alarm IRQ (W1C). 1: timer IRQ (W1C). 0: 2 Hz IRQ (W1C) (PG p.43) |
| 45h-47h | RTC Time L/M/H | RO | 24-bit up-counter at 2 Hz |
| 48h-4Ah | RTC Alarm L/M/H | RW, 22h | 24-bit down-counter at 2 Hz |
| 4Bh-4Dh | LOSC Divider L/M/H | RW, 22h | Down-counter on LOSC. `F_LDIVIRQ = FLOSC / ((4B+1)(4C+1)(4D+1))` (PG p.44) |
| **4Eh** | **Watch Dog** | RW, 22h | 7: WD enable (overflow causes WDRST- and a reboot). 6-4: clock and length (000 1 kHz/176 ms, 001 512 Hz/352 ms, 010 128 Hz/1.4 s, 011 32 Hz/5.6 s, 100 8 Hz/22.2 s, 101 4 Hz/45 s, 110 2 Hz/90 s, 111 1 Hz/180 s). **3: write 1 to clear the WD timer (self-clearing)**. 2-0: reserved (PG p.44) |
| 4Fh | DC-DC Control | RW, ECh | 7: internal NMOS switch. 6: DCOP1/2 output enable (write 0 once stable). 5-3: VCC 2.6-3.3 V. 2-0: VDD 1.6-2.3 V (PG p.49) |
| 50h-6Ah | USB SIE | | See §2.4 |
| **70h** | MCU and DMA clock / B1, B2 mapping | RW, 30h | 7: PRE output. 6: WP output (both valid in modes F1-F8 except F7). **5: B1 to MCU (1) or USB (0)**. **4: B2 to MCU (1) or USB (0)**. 3: MCU clock = PLL DSP clock. 2-0: DMA clock divider /1../128 (PG p.10-11) |
| 71h | I2C Address | RW, 00h | 7-1: slave address. 0: R/W (master) or address match (slave) (PG p.38) |
| 7Ah | I2C Control | RW, 00h | 7: enable. 6: 400k (master) or start-IRQ enable (slave). 5: IRQ enable. 4: 0 = master, 1 = slave. 3-2: 01 START, 10 STOP, 11 repeated start. 1: release bus (slave). 0: ACK enable / last received bit (PG p.36-37) |
| 7Bh | I2C Status | RO*, 00h | 7: buffer flag. 6: STOP seen. 5: START seen. 4: R/W. 3: D/A. **2: IRQ pending (W1C)**. 1: write collision. 0: overflow (PG p.37-38) |
| 7Ch | I2C Data | RW | Data or address byte |
| 80h | DAC Control | RW, 03h | 5: DACEN. 3: DACMODE (1 = on-chip SDM). 2: internal DAC digital output enable. 1: mute (default 1). 0: 1-bit or 2-bit quantizer (PG p.51) |
| 81h | DAC Sample Rate | RW, 00h | 3-0: 0000 48k, 0010 24k, 0011 12k, 0100 32k, 0110 16k, 0111 8k, 1000 44.1k, 1010 22.05k, 1011 11.025k (PG p.51-52) |
| 88h/89h/8Ah | DAC PCM Out L/M/H | WO | 88[7:4] = PCM[3:0], 89 = PCM[11:4], 8A = PCM[19:12]. The write to 8Ah pushes a 20-bit sample into the playback FIFO. Same FIFO as DSP `0x3FED` (PG p.52) |
| 8Bh | FIFO Status | RW, 15h | 5: DAC FIFO FULL (RO). 4: FIFO counter reset (0 = reset). 3-0: quantizer level QU (PG p.52) |
| 91h | DAC dither | RW, 07h | 3: dither. 2: dynamic dither. 1-0: amplitude (PG p.52-53) |
| 9Ah | Audio ADC data | RO | Read sequence: R hi, R lo, L hi, L lo (PG p.54) |
| 9Bh | Audio ADC bus | RW, 00h | 0: 16-bit (0) or 18-bit (1) ADC (PG p.58) |
| 9Eh | DAC analog volume | RW, 40h | 7-6: VRDA buffer bias. 5: OP bias x1.5. **4-0: headphone volume, 32 steps of -1.8 dB** (PG p.53) |
| 9Fh | ADC modulator tune | RW, 5Dh | Capacitor selects (PG p.55-56) |
| **C0h** | Key Scan Data | RW | 12 internal latched bytes read through a pointer. Any write clears and resets (PG p.38-39) |
| **C1h** | Key Scan Control | RW, 00h | 7: key-scan enable. 6-5: debounce 40/80/160/320 ms. 2-0: KEYI[2:0] mask (1 = key input, 0 = usable as GPIO) (PG p.39) |
| CCh | Flash ECC control | RW, 00h | 7-5: ECC[2:0]. 4: ECC start (self-clearing). 2-0: ECC[13:11] (PG p.35) |
| CDh | Flash ECC 0 | RW | ECC[10:3] |
| CEh | Flash ECC 1 | RW, xxx10xxxb | 7-5: ECC'[2:0] (RO). 4-3: ECC block type (00 128 B/20 bit, 01 256 B/22 bit, 10 512 B/24 bit, 11 2 KB/28 bit). 2-0: ECC'[13:11] |
| CFh | Flash ECC 2 | RW | ECC'[10:3] (PG p.36) |
| D0h | Battery-monitor ADC | 7-4 RW (0101b), 3-0 RO | 7: USB TX force clock/data. 6-4: USB TX bias. **3-0: battery ADC result** (PG p.49-50) |
| D1h | SARADC (LRADC) control | RW, 04h | 7: enable. 6: input = LRADC pin. 5-4: fs 8k/4k/2k/1k. 3-2: bias (PG p.54) |
| D3h | Audio ADC tune | RW, 03h | 1-0: input gain -6/-3/0/+3 dB (PG p.55) |
| D4h | Audio ADC control 1 | RW, 05h | 7: ADC enable. 6: MIC stage. 5: line-in. 4: FM. 3: dither. 2-0: MIC gain x22..x57 (PG p.56) |
| D5h | Audio ADC control 2 | RW, 00h | 7-5: fs 8/12/16/24/32/48k. 4/3/2: MIC/line/FM to mixer (1 = unmute). 1-0: source (00 MIC, 01 FM, 10 line, 11 mixer) (PG p.56-57) |
| D6h | Analog input gain | RW, 55h | 6-4: line gain. 3: USB force-out select. 2-0: FM gain (-7.5..+3 dB) (PG p.57-58) |
| D7h | Audio ADC FIFO | RW, 00h | 7: data-ready IRQ enable. 6: FIFO sync/clear DRQ. 5: not-empty. 4: right-only. **3: IRQ pending (W1C)**. 2: FIFO reset (0 = reset). 1-0: threshold 8/4/1 of 12 (PG p.58) |
| D8h | LRADC data | RO | LRADC bits 10-3 (PG p.58) |
| DDh | DAC analog block | RW, 00h | 7: analog DAC enable. 6: mixer and PA enable. 5-0: op-amp and PA bias (PG p.53-54) |
| DEh | SARADC IRQ | RW, 80h | 5: LRADC ready IRQ enable. **0: pending (W1C)** (PG p.58) |
| DFh | Power Control | RW, 28h | 7-6: LBD (system reset) level 0.90/0.98/1.06/1.14 V. 5-4: LBNMI level 0.98/1.06/1.14/1.22 V. 3-2: IBIAS (RCOSC). 1: VCCOUT. 0: read = DCDIS pin, write = DCOP clock (PG p.50) |
| ECh | Samsung Flash BA register 2 | RW, 00h | See §2.3 |
| **EEh** | **MFP Select / GPO_A[2:0] data** | RW, **01h** | **7-5: MFP select** (000 = F1 GPIO, 001 = F2, 010/011 reserved, 1xx reserved). 4: CE0S control (1 = treat as CE0S=H). **3: GPO_A2 enable**. **2-0: GPO_A[2:0] output data** (PG p.40) |
| **EFh** | GPIO_B / KEY config | RW, 00h | 6/5/4: B6/B5/B4 as KEYO2/1/0 (1) or GPIO (0). 2/1/0: B2/B1/B0 as KEYI2/1/0 (1) or GPIO (0). Bits 7 and 3 must not be written with 1. If any of bits 2-0 is set, set the matching C1h bit as well (PG p.40) |
| **F0h** | GPIO_B Output Enable | RW, 00h | Bits 6,5,4,2,1,0 = B6,B5,B4,B2,B1,B0 (1 = output) (PG p.40-41) |
| **F1h** | GPIO_B Input Enable | RW, 00h | Same bit positions (1 = input) (PG p.41) |
| **F2h** | GPIO_B Data | RW, 00h | Same bit positions (PG p.41) |
| **F3h** | GPIO_C OE/IE | RW, 07h | 6/5/4: C2/C1/C0 input enable. 2/1/0: C2/C1/C0 output enable (PG p.41) |
| **F4h** | GPIO_C Data | RW, 03h | 2-0: C2..C0 (PG p.41-42) |
| **FEh** | GPIO_G0 OE/IE | RW, 00h | 4: G0 input enable. 0: G0 output enable (PG p.42) |
| **FFh** | GPIO_G0 Data | RW, 00h | 0: G0 (PG p.42) |

Ports that the 2085X guide does **not** document are effectively absent or undocumented. These are:

- 38h-3Dh, 72h-79h, 7Dh-7Fh, C2h-CBh, E0h-EBh, EDh, F5h-F9h.
- In the ATJ2087 and ATJ2089 guides these ranges hold SPDIF (38-3B), UART/IR (72-79), SPI (7D-7F), the internal segment LCD (C2-CB), the **MMC controller (E0-EB, ED)** and extra GPIO (F5-F9).
- The 2085X master IRQ register still has "UART/IR/SPDIF" and "SPI" bits, and the DMA DRQ lists name UART2 and SPDIF. So the silicon may share the 2087 core and these blocks may be partly present but unbonded or untested (inferred).

### 2.2 GPIO pin functions

The package-pin and register cross-reference is in [§6.2](#62-gpio-package-pin-reference).
Board-specific wiring examples follow.

Reference schematic wiring (SCH, ATJ2085 + NT7502 mono LCM + K9K1G08U0M NAND):

- LCMCE- = **CE3-** (pin 36). LCMA0 = **GPO_A0** (pin 45, shared with ICEDI). LCMRST = **GPO_A1** (pin 43, shared with ICECK). LCM data = D[7:0], with MWR- and MRD- as 8080 WR/RD.
- Backlight ELENA = **GPIO_C2** (pin 28), driving an NPN to the EL/LED.
- LEDs: BLUE via GPIO_G0 (net "GPIO1"), RED via KEYO0/B4.
- Keys: a 2x3 matrix KEYO1/KEYO2 x KEYI0/KEYI1/KEYI2 (K1-K5).
- NAND: FLASHCE1- = CE1- (pin 38), FLASHCE2- (CE2-) is on a 0R option. ALE, CLE, MWR- (WE), MRD- (RE) and D0-7 connect directly. FM (acfmx01, TEA5767-like) on FMDATA/FMCLK (I2C).
- **Your Lisong board differs.** The firmware and TXT put LCD A0/RS on **GPIO_C2** (F4h.2) and LCD reset on **GPO_A0** (EEh.0). So treat the pin assignment as board-specific.

### 2.3 NAND / SMC state machine (PG §8, p.33-36)

| Port | Name | Bits |
|---|---|---|
| 28h | NAND Flash Enable | 3/2/1: enable flash access on CE3-/CE2-/CE1- (this enables ALE/CLE outputs). 0: state machine busy (RO). CE enable also needs 01h/02h to decode to that CE. |
| 29h | CE- Driving Mode | 3/2/1: bank 3/2/1 new mode (CE don't-care) or old mode. **0: flash type, 1 = Samsung NAND, 0 = MXIC NOR** |
| **2Ah** | **Samsung NAND CMD** | RW. Writing any command except 37h starts the hardware sequencer: one CLE cycle plus 0-4 ALE cycles depending on the command and the ECh.7 capacity bit. Commands: 80h (3/4 address cycles) program input, 8Ah copy-back, 02h gapless read, 00h/01h read 1, 50h read spare, 03h read 3, 90h (1) read ID, FFh reset, **10h** program confirm, 11h dummy program, 60h (2/3) block erase, **70h** read status, 71h, B0h suspend, **D0h** erase confirm, **37h stops the SM and deasserts CE- (old mode)**. Write 37h before touching other devices on the shared bus, such as the LCM. |
| 2Bh | CA | WO: column address [7:0] |
| 2Ch | RA | WO: row address [15:8] (1Mx8) or [16:9] (8Mx8 and larger) |
| 2Dh | BA1 | 7: PA24. 6: PA23. 5-4: PA22-21. 3-0: PA19-16 or PA20-17 |
| ECh | BA2 | 7: capacity (1 = high capacity, 64M+, which adds an address cycle). 6/5/4: force CE1-/CE2-/CE3- low in new mode. 1: PA26. 0: PA25 |
| CCh-CFh | ECC | Hardware Hamming ECC over 128/256/512/2048 bytes (CEh[4:3]). ECC = computed, ECC' = second (read-back) set. Registers detailed in §2.1. |

Data phase (inferred from the guide): after the sequencer issues command and address, data bytes are read or written with ordinary MCU memory cycles (MRD-/MWR-) inside the `8000-FFFF` window with the flash CE decoded by 01h/02h. DMA1/2 can do this with SA3/DA3 bit7=1 (external).

### 2.4 USB SIE (PG §7, p.22-32)

| Port | Name | Notes |
|---|---|---|
| 3Eh | Pull-up control | See §2.1 |
| 50h | Global status/control | 7: suspend (RW). 6: USBPWR (RO, Vbus). 5: D+ (RO). 4: D- (RO). 3: bus-active (W1C). 2-0: force drive / D+ / D- (remote wakeup) |
| 51h | Device address | 7: software enable (must be 1 to respond). 6-0: address |
| 52h | Global IRQ (W1C) | 7: bus reset. 6: connect/disconnect (check 50h.0). 5: EP0 SETUP. 4: WAKEUP |
| 53h | Endpoint IRQ (W1C) | 7: EP3OUT. 6: EP3IN. 5: EP2OUT. 4: EP2IN. 3: EP1OUT. 2: EP1IN. 1: control-write. 0: control-read |
| 54h/55h | Frame number H/L | 54.7: CRC5 check. 54[2:0]: FN[10:8] |
| 57h | SETUP status | 7: DT. 6: valid. 5: SETUP lock (W1C). 4: ACK sent. 3-0: count |
| 58h-5Fh | SETUP data 0-7 | RO |
| 60h | Endpoint index (EPI) | 0 CR, 1 CW, 2 EP1IN, 3 EP1OUT, 4 EP2IN, 5 EP2OUT, 6 EP3IN, 7 EP3OUT |
| 61h | Endpoint mode | CR/CW: 1 complete, 0 stall. IN: 2 ISONBR, 1 ISO, 0 STALL. OUT: 1 ISO, 0 STALL |
| 62h/63h | Start address H/L | **Buffer pointer into MCU system memory** (USB DMA). MA = SA + CBC + offset. Typically ZRAM2 B1/B2 |
| 64h/65h | Byte counter H/L | Read = CBC, write = TBC. **A write to 64h clears BR. A write to 65h sets BR** (write 65h last) |
| 66h | MPS / data toggle | 7: SPR (read) or force DT (write). 6: DT. 5-0: MPS-1 (1..64) |
| 68h | Buffer-ready status | RO, per endpoint |
| 69h | NAK / status-stage success | 7-2 W1C |
| 6Ah | STALL status | 7-2 W1C |

---

## 3. DSP host interface (everything the documents give, plus marked inferences)

### 3.1 Registers involved

| Function | Register | Bits (documented) |
|---|---|---|
| Reset | **2Eh.7** | 0 = DSP held in reset, 1 = run. Default 2Eh = 01h, so the DSP is in reset at power-up (PG p.20) |
| Clock | 2Eh.1 DSPCKEN, 2Eh.2 source (0 PLL / 1 HOSC), **42h[4:2]** PLL multiplier (12-84 MIPS), 42h.6 PLL1EN, 42h.7 HOSCEN | PG p.20, 46-47 |
| Memory option | 2Eh[6:4] DUMMYS = 111b normally | PG p.20 |
| Boot mode | 2Fh (default 73h). Bit3 "NAND select" must be 0 for NAND | PG p.20-21 |
| Memory ownership | **05h[7:4]**: per-8K-half ownership of IPM/IDM (1 = MCU, 0 = DSP). 05h[2:0] lane select | PG p.12-13 |
| MCU to DSP interrupt | **2Eh.0 IRQ2DSP-**. Write 0 to assert IRQ2- on the DSP, then write 1 to release (inferred: level, active low) | PG p.20 |
| DSP to MCU interrupt | **26h.0** DSP IRQ status (W1C), **27h.0** enable | PG p.20 |
| Mailbox | **30h-35h HDR0-HDR5** (data), **36h HSR6, 37h HSR7** (status) | PG p.21. Bit meanings are **not documented**. |
| DMA to/from DSP RAM | DMAx SA3/DA3 bit6=1 (IPM/IDM/ZRAM2) with DMAxISA/IDA[2:0] lane. **Mode bit5 "DSP mode"** auto-cycles the lane (A14/A15 = 0,1,2) per word. **Mode bit2 "DSP BOOT"** | PG p.14-18 |
| Concurrent load | 05h.3. MCU keeps running from ZRAM + IO 00-2Fh while DMA moves external memory into IPM/IDM | PG p.12-13 |
| DSP-side DAC | DSP writes PCM to DM `0x3FED`, polls `0x3FEE`.bit2 (FIFO full) | PG p.52 |

### 3.2 Download / start / stop sequence

Inferred from the registers above; Actions does not document the procedure:

1. Configure the clocks: HOSC (42h.7), PLL1 (42h.6), DSP frequency (42h[4:2]).
2. Hold the DSP in reset: `2Eh.7=0`, with DSPCKEN as required.
3. Give IPM/IDM to the MCU: `05h = C0h` (IPM low lane, both halves). Then `C1h` (mid lane) and `C2h` (high lane). Copy each 16K-byte lane to `4000-7FFF`. Repeat with `30/31/32h` for IDM initial data.
4. Alternatively, DMA from NAND or external memory into IPM:
   - SA3 bit7=1 (external) with SA = the EMA address.
   - DA3 bit6=1 (IPM/IDM) with DA1IDA lane 000.
   - Set **mode bit5 = DSP mode** so one 3-byte group fills L/M/H of one 24-bit word. Bit2 "DSP BOOT" is presumably used for this.
   - Optionally set 05h.3 so the MCU runs concurrently.
5. Return the memory to the DSP: `05h[7:4] = 0000`.
6. Set `2Eh` = clock enabled + PLL source + `DUMMYS=111` + `bit7=1`. The DSP starts at PM address 0 (inferred).
7. Run-time communication:
   - Command/parameter bytes go through HDR0-5. HSR6/HSR7 are status.
   - The MCU kicks the DSP with a 2Eh.0 pulse (0 then 1).
   - The DSP signals the MCU through the DSP IRQ (26h.0, acknowledged by writing 1 to 26h.0).
   - Bulk data (bitstream in, PCM, etc.) goes through IDM halves owned in turn by MCU and DSP (05h bits 5/4), or by DMA into IDM.
8. Stop: `2Eh.7=0` (reset) and/or `2Eh.1=0` (clock stop).

(Inferred) The naming HDR0-HDR5 / HSR6-HSR7 is identical to the Analog Devices ADSP-2111 Host Interface Port. In that HIP, HSR6/HSR7 carry per-HDR "written by host / written by DSP" flags plus host-interrupt and soft-reset bits. If the Actions core is an ADSP-21xx derivative (the PA/DA/PB/DB bus naming supports this), expect the same semantics. **Verify against firmware usage.**

### 3.3 Audio data paths

- **DAC**:
  - The playback FIFO holds 8 x 20-bit samples. It can be fed by the MCU (88h, 89h, 8Ah; the write to 8Ah commits) or by the DSP (`0x3FED`).
  - There is **no DAC DRQ** in the DMA1/DMA2 trigger lists. In normal playback the DSP pushes PCM itself (inferred).
- **ADC**: 9Ah FIFO with IRQ (D7h) or DMA2 DRQ2B (with the DMA clock at MCUclk/4).
- **NAND to DSP**:
  - The MCU reads NAND pages, either by CPU or by DMA1/2 from the external window to ZRAM, IDM or ZRAM2.
  - Then it hands buffers to the DSP by switching 05h ownership or by a DMA into IDM.
  - DMA1/2 **halt the MCU** while active (PG p.22 "DMA MODE NOTE"), except in the 05h.3 concurrent mode. USB DMA (and the "DMA3/DMA4" named in that note) does not halt the MCU.

---

## 4. Interrupts

| Item | Documented facts |
|---|---|
| CPU interrupt mode / vector | **Not documented** in the 2085X PG or DS. The Actions ATJ2099 SDK startup (`68478825Action2099...\startup.msa`) does `di; im 1` with a handler at `0038h` and an initial `out (04h),01h`. **(Inferred) The ATJ2085 also uses Z80 IM1**: every maskable IRQ goes to **RST 38h / 0038h**, and NMI goes to **0066h**. |
| NMI sources | Low-battery NMI (04h.5, level set in DFh[5:4]) and DCDIS-pin NMI (04h.3, both edges). Enabled by **04h.1**. Flags are W1C in 04h (PG p.12). |
| Master status / enable | **26h / 27h** (PG p.20): |
| | bit7: ADC, ack in D7h.3 (audio) / DEh.0 (LRADC) |
| | bit6: keyboard (W1C in 26h) |
| | bit5: RTC, ack in 44h[2:0]; enables in 43h[7:5] |
| | bit4: DMA/CTC, ack in 25h[7:3]; enables in 13h/21h[7:6], 22h.7 |
| | bit3: SIRQ / I2C / SPI. SIRQ: enable 04h.2, ack 25h.2. I2C: enable 7Ah.5, ack 7Bh.2 |
| | bit2: USB, ack in 52h/53h (W1C). The IRQ stays active until every pending bit is cleared |
| | bit1: UART/IR/SPDIF, no 2085X registers |
| | bit0: DSP (W1C in 26h) |
| Ack rule | Bits marked "RO" in 26h are summaries. Clear the source register. Key and SIRQ flags latch even when disabled (PG p.20). |
| Wake from clock stop | Reset, POR, keyboard IRQ, alarm IRQ, SIRQ, DCDIS NMI (PG p.10) |
| Watchdog | 4Eh. Not an interrupt: it resets the chip and sets the 04h.7 flag. |

Software RST vectors (08h, 10h, ... 38h) are a firmware convention and not covered by the documents. **If IM1 is used, `RST 38h` and the hardware IRQ share the same entry point.** The 38h handler must tell them apart, for example by checking 26h or a flag (inferred).

---

## 5. Boot sequence

Documented (PG p.12, 21-22; DS p.8, 11):

1. After reset, **04h.0 = 0 forces A15 = 1**, so instruction fetches from 0000h actually hit `8000h+` in the bank window.
2. With 01h = 02h = 00h, bank 0 / CE0 space holds the **8 KB BROM at 8000-9FFF**. BROM must jump to `800xh` and then write 04h.0 = 1 for normal operation.
3. Pin/bit **CE0S** (EEh.4) controls the default pin mode (F1 if CE0S=H, otherwise F4). (Inferred) This selects the external CE0 ROM versus internal boot and the alternate pin mode for factory/test.
4. **TROM** is a 21 KiB DSP table ROM, not a second MCU boot/recovery image. It is readable by the MCU through bank 2 (`01h=02h, 02h=00h`, `8000-D3FF`) and the programming-guide block diagram connects it to DSP `DB[23:8]`; see "TROM contents and interfaces" below.
5. CE1- is the "Boot up Nand Flash chip enable (must be connected to Nand Flash)". BROM boots from NAND on CE1-.

**BREC / FW stages are not documented.** (Inferred from the s1mp3 ecosystem) BROM loads the BREC from the first NAND blocks into ZRAM1, and BREC then loads the FW ("FWIMAGE") system and AP banks from NAND.

---

## 6. Cross-check of firmware-derived facts

| # | Claim (from firmware RE) | Verdict | Documented register / evidence |
|---|---|---|---|
| 1a | Port 00h = AP bank select | **CONTRADICTED** | 00h is **MCU Clock Control 1**: bits 7-6 external-memory wait states, bit4 clock source, bits 3-0 clock divider (PG p.9-10). Bank/page registers are **01h** (EMA22-15) and **02h** (EMA28-23) (PG p.11). If the firmware really puts bank numbers in 00h, either the analysis mislabels 01h, or the code targets a different chip revision. Re-check. |
| 1b | Port 02h saved and set to 18h around LCD access | **CONFIRMED** (and explained) | 02h = EM High Page. `18h` means EMA28:26 = 011, i.e. **CE3-**, the "8080 interface LCM chip enable" pin 36 (PG p.8, p.11-12). Saving and restoring 02h restores the NAND/ROM CE afterwards. |
| 1c | `00h |= C0h` maps the external bus / LCD CS at 8000h | **CONTRADICTED as described / functionally explained** | 00h[7:6] = 11 means **3 wait states** for external memory access (PG p.9), which slows the bus for the 8080 LCM. The CS decode comes from 02h (CE3) together with A15=1 (`8000-FFFF`). |
| 2 | Port 05h used by FS/VM/NAND drivers (paging?) | **CONFIRMED** | 05h = Internal MROM/SRAM Page register. It maps IPM/IDM byte lanes (16K each) or ZRAM2 into `4000-7FFF` (PG p.12-13, p.21). The drivers use DSP RAM or ZRAM2 as buffers. |
| 3a | LCD data and command written to memory 8000h | **CONFIRMED** (inferred mechanism) | Any MCU write in `8000-FFFF` is an external MWR- cycle on the CE decoded by 02h (PG p.11, p.21). CE3- is the LCM CE (PG p.8). |
| 3b | Port F4h bit2 = LCD A0/RS | **CONFIRMED (register); wiring is board-specific** | F4h = GPIO_C data, bit2 = **GPIO_C2** (PG p.41-42). TXT agrees (GPIOC2 = D/C). The reference SCH instead uses GPO_A0 for A0 and C2 for the backlight. |
| 3c | Port F3h bit6/bit2 configured before LCD reset | **CONFIRMED** | F3h bit6 = GPIO_C2 input enable, bit2 = GPIO_C2 output enable (PG p.41). Setting bit2 and clearing bit6 makes C2 an output for A0. |
| 3d | Port EEh bit0 = LCD reset | **CONFIRMED (register)** | EEh[2:0] = GPO_A[2:0] output data. bit0 = **GPO_A0** (pin 45, also ICEDI) (PG p.40). TXT agrees (GPIOA0 = reset). |
| 3e | Port EEh bit1 = backlight | **CONFIRMED (register)** | bit1 = **GPO_A1** (pin 43, also ICECK). Board-specific; the reference SCH uses GPO_A1 as LCMRST. |
| 3f | Port EEh bit2 toggled by the key driver from port FFh bit0 | **CONFIRMED (registers)** | EEh.2 = **GPO_A2** data, which only drives if **EEh.3 (GPOA2 enable)** = 1. FFh.0 = **GPIO_G0** data, FEh.4 = G0 input enable (PG p.40, 42). The function (e.g. hold switch / power latch or LED) is board-specific. **Caution:** EEh[7:5] is the MFP select and bit4 is CE0S, so read-modify-write must keep them. |
| 4 | Port 4Eh read 5x, then written with bit3 set, inside delay loops | **CONFIRMED (watchdog clear)** | 4Eh = Watch Dog register. **bit3 = write 1 to clear the WD timer** (self-clearing). bit7 = enable, bits 6-4 = timeout (PG p.44). The 5 reads are not explained; most likely padding or delay. The ATJ2099 SDK does `in a,(4eh); or 08h; out (4eh),a`. |
| 5a | Port F0h &= 99h | **CONFIRMED** | F0h = GPIO_B **output enable**. Clearing bits 1,2,5,6 makes **B1, B2, B5, B6** non-outputs (PG p.40-41). |
| 5b | Port F1h \|= 66h / init 77h | **CONFIRMED** | F1h = GPIO_B **input enable**. 66h = B1, B2, B5, B6. 77h = B0, B1, B2, B4, B5, B6 (PG p.41). |
| 5c | Port F2h read & 66h | **CONFIRMED** | F2h = GPIO_B data. Reads B1 (KEYI1), B2 (KEYI2), B5 (KEYO1), B6 (KEYO2) as plain inputs (PG p.41). |
| 5d | Init EFh=0, C1h=0, FEh=10h | **CONFIRMED** | EFh=0: all GPIO_B pins are GPIO, not KEYI/KEYO (PG p.40). C1h=0: hardware key-scan disabled (PG p.39). FEh=10h: **GPIO_G0 input enable** (PG p.42). So keys are read by software, not by the scan engine. |
| 6a | Port 01h = 1 command latch / 2 address latch / 0 data | **NOT COVERED (register exists, meaning differs)** | 01h is the EM Low Page, EMA22-15 (PG p.11). Values 1 and 2 set **EMA15** and **EMA16**. The guide never says CLE/ALE follow EMA15/16 in manual mode. CLE and ALE are dedicated pins enabled by 28h[3:1], normally driven by the 2Ah sequencer. (Inferred) In manual (non-SM) access, CLE = EMA15 and ALE = EMA16. This is consistent with the firmware. Check that 02h = 08h (CE1) or 10h (CE2) during NAND ops. |
| 6b | NAND data at memory 8000-81FF | **CONFIRMED (mechanism)** | `8000-FFFF` is the external window (PG p.21). 200h bytes = one 512-byte page. The address within the window is irrelevant to NAND (no address pins). |
| 6c | Port 2Ah receives NAND command codes (70h, D0h, 10h) | **CONFIRMED** | 2Ah = Samsung NAND Flash CMD register. 70h read status, D0h start block erase, 10h page program (true). Writing it starts the hardware CMD/ADDR sequencer; 37h stops it and releases CE (PG p.33-34). |
| 6d | Ports 06h-13h | **CONFIRMED** | DMA1 registers: SA0-3 (06-09), ISA (0A), DA0-3 (0B-0E), IDA (0F), BC (10-11), Mode (12), Command (13) (PG p.14-16). |
| 6e | Ports CCh-CFh | **CONFIRMED** | Flash ECC control (CC) and ECC/ECC' result registers (CD-CF) (PG p.35-36). |
| 7 | SD/MMC controller at E0h-EEh | **NOT COVERED for the 2085X / partly CONTRADICTED** | The 2085X PG documents no MMC block. **ECh = Samsung Flash BA register 2** (PG p.35) and **EEh = MFP select / GPO_A data** (PG p.40). E0h-EBh and EDh are an MMC controller **on the ATJ2087/2089** (PG87 p.40-43): E0 control, E1 command, E2-E5 argument, E6 response, E7 CRC7, E8 data in, E9/EA CRC16, EB data out, ED clock select. The firmware's MMC driver is probably shared SDK code that is unused on 2085 hardware (inferred). |
| 8 | RST vectors 08h fast API, 10h bank API, 18h storage, 20h display, 28h FS, 30h syserr, 38h | **NOT COVERED** | Software convention. Note that under IM1 (inferred, §4) the maskable IRQ also enters at **0038h**. |
| 9 | LCD = S6B33B, 132x132 RGB565 | **CONTRADICTED (controller identity)** | None of the listed opcodes form a valid S6B33B init. S6B33B opcodes are 02, 10, 20, 22, 24, 26, 28, 2A-2F, 30, 32, 34, 40, 42, 43, 45, 50, 51, 53, 55-57, 59, 5A, FC and test modes F9-FF (S6B p.28). The sequence matches the **Solomon SSD1789A** (132 RGB x 132, 65K colour) / Epson S1D15Gxx-style set exactly. TXT already annotated it as SSD1789. "132x132, 16 bpp" is **CONFIRMED** by the SSD1789A (p.1-2). |

### 6.1 LCD init opcodes: S6B33B meaning versus actual (SSD1789A) meaning

| Opcode | S6B33B0A (S p.28) | SSD1789A (SSD pp.29-35) | Parameters seen in TXT |
|---|---|---|---|
| D1 | undefined | **Internal oscillator ON** (D1 on / D2 off) | - |
| 94 | undefined | **Sleep OUT** (94 exit / 95 enter) | - |
| 20 | DC-DC Select (1 B) | **Power Control**: X0 reference generator, X1 regulator/follower, X3:2 booster 4x-7x | 0B: 6x boost, ref + regulator on |
| 81 | undefined | **Contrast level** (6-bit) **and internal regulator resistor ratio** (2 params) | 21, 07 |
| F3 | test area (F9-FF are test modes; F3 undefined) | **Not in the SSD1789A Rev1.0 table** (vendor or analog setup; unknown) | 90 05 60 84 |
| FB | **Test Mode** ("not permitted") | **Set biasing ratio** | 02 = 1/9 |
| F2 | undefined | **Frame frequency** (F3:F0) **and N-line inversion** (N5:0, N6 = do not reset per frame) | 08, 50 |
| F7 | undefined | **PWM/FRC select** (fixed 28h, mode, fixed 05h) | 28 0E 05 = 5-bit PWM + 1-bit FRC |
| A7 | undefined | **Inverse display** (A6 = normal, A7 = inverse; SSD p.45). TXT's "normal" label is wrong per the datasheet; the panel is probably normally-black. | - |
| CA | undefined | **Display control**: duty selection | 00 1F 00 |
| BC | undefined | **Data output scan direction / RGB arrangement / colour mode** | 00 01 00 |
| BB | undefined | **COM output scan direction** | 01 |
| F1 | undefined | **COM sequence** (odd/even split) | E0 00 00 |
| 15 | undefined | **Column address set** (start, end) | 00 83 = 0..131 |
| 75 | undefined | **Page (row) address set** (start, end) | 00 83 = 0..131 |
| 44 | undefined (S6B33B uses 42/43/45) | **First display COM / start line** (0-131) | 04 |
| 82 | undefined | **Temperature compensation gradient** | 02 |
| AF | undefined | **Display ON** | - |
| 5C | undefined | **Write display data** (enter RAM-write mode; data follows with A0=1) | - |
| 5D | undefined | **Read display data / status read** (first byte dummy) | - |
| AE | undefined | **Display OFF** | - |

### 6.2 GPIO and LRADC package-pin reference

Package pins and GPIO alternate functions (PG p.6-9, 40-42; DS p.11-12).
The LRADC is an analog input, not a GPIO; its control/data registers are D1h/D8h (PG p.54, 58).

| Pin | Ball | Alternate function | Data reg / bit | Direction regs | Lisong firmware use |
|---|---|---|---|---|---|
| 45 | GPO_A0 | ICEDI | EEh.0 | output only (no ICE) | LCD reset: driven low, then high during UI init. |
| 43 | GPO_A1 | ICECK | EEh.1 | output only | LCD backlight, on/off. |
| 44 | GPO_A2 | ICEDO | EEh.2 | enable EEh.3 | KY_HARD's Beep entry mirrors GPIO_G0 onto this bit; what the board connects it to is unknown. |
| 27 | GPIO_B0 | KEYI0 | F2h.0 | F0h.0 / F1h.0 | Initialized as an input, but not sampled by the KY_HARD button scan. No use identified. |
| 29 | GPIO_B1 | KEYI1 | F2h.1 | F0h.1 / F1h.1 | Button input: VolAdd in the normal scan phase and Next in its second phase. |
| 35 | GPIO_B2 | KEYI2 | F2h.2 | F0h.2 / F1h.2 | Button input: Play/Pause. |
| 40 | GPIO_B4 | KEYO0 | F2h.4 | F0h.4 / F1h.4 | Initialized as an input, but not sampled by the KY_HARD button scan. No use identified. |
| 41 | GPIO_B5 | KEYO1 | F2h.5 | F0h.5 / F1h.5 | Button input: key code 0Dh (likely MODE/MENU; exact label uncertain). |
| 42 | GPIO_B6 | KEYO2 | F2h.6 | F0h.6 / F1h.6 | Button input: Prev; also driven during the second scan phase. |
| 63 | GPIO_C0 | I2C_SCL | F4h.0 | F3h.0 / F3h.4 | Software-bit-banged I2C clock for the FM tuner. |
| 62 | GPIO_C1 | I2C_SDA, SIRQ- | F4h.1 | F3h.1 / F3h.5 | Software-bit-banged I2C data for the FM tuner. |
| 28 | GPIO_C2 | (none) | F4h.2 | F3h.2 / F3h.6 | LCD A0/RS (command/data select). |
| 1 | GPIO_G0 | (none) | FFh.0 | FEh.0 / FEh.4 | Configured as an input; KY_HARD's Beep path copies its level to GPO_A2. Board-level function is unknown. |
| 22 | LRADC | Analog input | D8h (sample) | N/A | No LRADC sample read was identified in the inspected Lisong firmware. STANDBY.AP disables LRADC via D1h.7 on entry and reenables it on wake. |

When the ICE port is connected (ICEEN- active), GPO_A[2:0] become ICE pins
(PG p.40, "Hardware Auto Detection").

GPIO firmware-use entries are based on the stock Lisong UI, key, and FM drivers; the LRADC
entry is based on the Lisong standby routine. See
[`drivers/README.md`](../drivers/README.md), [`analysis/i2c.md`](./i2c.md), and
[`analysis/power.md`](./power.md). “No use identified” means no use in the inspected
firmware paths, not proof that the pins are unconnected on every board revision.

---

## 7. Surprises and things the firmware may rely on that are not obvious

1. **00h is the clock register.** Any `out (00h),a` changes the wait states, the MCU clock source and the clock divider. Writing 1011b or 1111b to bits 3-0 **stops the CPU clock**; only key, alarm or SIRQ IRQs, NMI or reset revive it. Code that or's C0h into 00h before LCD writes is adding wait states, not selecting a chip.
2. **CE select lives in 02h bits 5-3.** 02h=00h is CE0 (BROM/ROM), 08h is CE1 (boot NAND), 10h is CE2 (second NAND), 18h is CE3 (LCM). With 02h=00h, 01h selects internal BROM (bank 0) or TROM (bank 2).
3. **03h auto-adds to the page.** Writes to 03h silently change 01h/02h, which matters when tracing bank state.
4. **NAND and LCM share D[7:0], MWR- and MRD-.** In old CE mode, 37h must be written to 2Ah to release the NAND CE- before talking to the LCM (PG p.34). Look for `ld a,37h; out (2Ah),a` in the display path.
5. **The DMA halts the CPU.** DMA1/2 stop the MCU unless 05h.3 concurrent mode is set; in that mode the MCU may only touch ZRAM and IO 00h-2Fh. Code running during DMA must live in ZRAM1 (`0000-3FFF`).
6. **The 4000-7FFF window is multiplexed** between 6 DSP byte lanes and ZRAM2 via 05h and 70h. The same MCU address means different RAM depending on 05h. Track 05h when decompiling buffer code. A DSP-owned half (05h bit = 0) is invisible to the MCU.
7. **DSP code is loaded as three byte lanes.** A 24-bit word's L/M/H bytes sit at the same offset in three different 05h lanes, or are interleaved L, M, H in the source stream when DMA "DSP mode" (12h/20h bit5) is used. DSP images in the firmware are probably stored as consecutive 3-byte words.
8. **DSP to MCU IRQ is bit0 of 26h/27h (W1C). MCU to DSP is 2Eh.0, active low.** Mailboxes are 30h-37h, with HDR0-5 / HSR6-7 semantics apparently ADSP-2111-style (inferred).
9. **The DAC can be fed by the MCU** (88/89/8Ah, the 8Ah write commits) as well as by the DSP (DM `3FEDh`). Key-click or beep code may write the DAC directly. 9Eh[4:0] is the headphone volume.
10. **Watchdog.** Default 4Eh = 22h (disabled, 128 Hz clock select). Reset cause is in 04h.7 (WD) and 04h.6 (external), both W1C. Firmware may branch on these at boot.
11. **Battery and power.** D0h[3:0] is the 4-bit battery level. DFh sets the LB-reset and LB-NMI thresholds. 04h.5 is the LB-NMI flag. 4Fh and 3Fh control the DC-DC and voltage detect. 50h.6 USBPWR (RO) is the USB-plug detect, and 52h.6 is the connect/disconnect IRQ.
12. **The GPO_A pins are also the ICE pins.** With an ICE attached (ICEEN- low) the LCD reset and backlight on EEh[1:0] do not work.
13. **Write restrictions on EEh and the GPIO registers.** EEh resets to 01h (GPO_A0 = 1, so LCD reset is idle-high if GPO_A0 is reset). "Reserved bits CAN NOT be written with 1" applies to F0-F4, FE, FF and EF. Masks like 99h/66h/77h respect this (bits 7 and 3 stay 0).
14. **Key scan engine.** C0h/C1h exist (3x3, 12-byte read FIFO) but this firmware polls GPIO instead (EFh=0, C1h=0). The keyboard IRQ (26h.6) will therefore not fire.
15. **The I2C master is at 7Ah/7Bh/71h/7Ch, and 71h holds the slave address.** This is probably used for the FM tuner (TEA5767-type module on C0/C1 in the SCH).
16. **Undocumented port ranges.** Any firmware access to 38h-3Dh, 72h-79h, 7Dh-7Fh, C2h-CBh, E0h-EBh, EDh or F5h-F9h targets blocks documented only for ATJ2087/2089 (SPDIF, UART, SPI, segment-LCD, MMC, extra GPIO). On a 2085X these are probably dead code from a shared SDK.
17. **DMA to IO.** Mode bits 1/0 make DST/SRC an IO port, e.g. streaming into 2Ah or out of 9Ah. With an external DRQ (13h/21h bit1) the DMA paces on ADC FIFO / UART / SPDIF requests.
18. **USB buffers.** The USB SIE DMA addresses MCU memory directly via 62h/63h. With 70h bits 5/4 = 0, ZRAM2 B1/B2 belong to USB and are invisible to the MCU.

### TROM contents and interfaces (dumped 2026-11-06 with the zos USB console `rom 2 ...`, zos/tools/romdump.py)
`analysis/trom_21k.bin` (21504 B, md5 a1099869ab074dd254737ee89c9c42f6). Method check: the same command on bank 0
returned a BROM byte-identical to `brom_8k.bin`. The TROM is **not MCU code**: read as little-endian 24-bit words it is
the **DSP table ROM** (7168 Q23 words). It starts with FFT/DCT twiddle factors: 0x5A8279 = cos(pi/4), 0x30FBC5 =
sin(pi/8), 0x7641AE = cos(pi/8), 0x18F8B8 = sin(pi/16) ... The first ~3K words are smooth cosine/window tables; the
rest has large deltas (integer tables, likely Huffman/decoder tables). It plays no part in the MCU boot.

The PG p.5 block diagram explicitly connects TROM to the DSP's `DB[23:8]` bus and shows separate
DSP external-data-memory read/write signals. Thus the table ROM is not just an MCU-bank-readable
asset: the DSP has a hardware data-bus path to it. The guide does not state the DSP-visible address
range, how the 8-bit ROM supplies 24-bit table words, whether a special mapping mode is required,
or whether all of the ROM is visible to the DSP. Those require DSP code/board-level confirmation.
The host bank mapping (`02h=00h, 01h=02h`) is a distinct, documented MCU access path.
