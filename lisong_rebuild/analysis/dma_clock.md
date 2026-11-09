# ATJ2085: DMA and system clock (findings for zos)

Evidence keys: **PG** = ATJ2085X Programming Guide v3.1 (printed page numbers, see atj2085_hw.md), **DS** = ATJ2085X datasheet v1.8,
file:address = disassembly in `lisong_rebuild\disasm_v04\`, `disasm_ref\`, `drivers\disasm\`.
**(inf)** = inference, not stated by any document and not measured. **(dev)** = measured on the device by the user.

## 1. DMA

### 1.1 Channels and registers (PG p.14-19, confirmed against stock code)
Two memory DMA channels, identical layout. Both are "memory/IO to memory/IO" engines, 15-bit byte count (max 32 KB per start).

| | DMA1 | DMA2 |
|---|---|---|
| source addr SA0..SA3 | 06 07 08 09 | 14 15 16 17 |
| source IPM/IDM/ZRAM2 select (ISA) | 0A | 18 |
| dest addr DA0..DA3 | 0B 0C 0D 0E | 19 1A 1B 1C |
| dest select (IDA) | 0F | 1D |
| byte count L/H | 10 11 | 1E 1F |
| mode | 12 | 20 |
| command | 13 | 21 |
| status (shared, W1C) | 25: bit3 DMA1 end, bit4 DMA1 half | 25: bit5 DMA2 end, bit6 DMA2 half |

Address registers: SA/DA[7:0], [15:8], [23:16]; byte 3: bit7 = 1 external bus, 0 internal; bit6 (internal only) = 0 ZRAM1, 1 IPM/IDM/ZRAM2
(then ISA/IDA picks the lane); bits 4-0 = address bits 28:24. So an external address is the same 29-bit EMA the CPU forms from ports
01/02 + A14:0, and **bits 28:26 pick the chip enable**: CE3 (LCD) = 011 -> DA3 = **0x8C** (bit7 external, bits 3,2). (Port 02 = 0x18 is the same
thing in CPU space: bits 4,3 = EMA27,26.) CE1 (NAND) would be 0x84: do not use.

Mode (12h / 20h): 7-6 wait states for the DMA bus cycle, 5 DSP mode (3-lane cycling), 4 dest down-count, 3 source down-count, 2 DSP boot, 1 dest is IO, 0 source is IO.
Command (13h / 21h): 7 TC IRQ en, 6 half IRQ en, 5 continuous (auto reload), 4 priority, 3-2 external DRQ select, 1 external DRQ en, **0 start**
(rising edge loads the counters, self-clears at terminal count). The DMA clock divider is port **70h bits 2-0** (/1../128, default /1); with the ADC DRQ
the DMA clock must be MCUclk/4 (PG p.18-19).

Transfer width: always bytes. DSP mode moves 3-byte groups (lane cycling L/M/H per 24-bit word). There is **no 16-bit mode and no fixed-address mode**:
only up (default) or down counting per side (mode bits 3/4), or "IO" (mode bits 0/1) which means the endpoint is a port instead of an address.

Completion: poll command bit0 (stock does: `in a,(0x13); bit 0,a; jr nz` e.g. MAIN_AP seg_bank63.s:28DE, MUSIC_AP seg_bank0b.s:2AA8) or status 25h
end flags / IRQ (13h bit7 + 27h bit4 = master enable bit "DMA/CTC").

**The CPU is halted while a DMA runs** (PG p.22 "DMA MODE NOTE"). Only exception: port 05h bit3 "concurrent" mode, only for *external source -> IPM/IDM/frame buffer*
destination; the CPU then keeps running from ZRAM and may only touch ports 00-2F. So no CPU/DMA overlap is available for LCD streaming (destination external).
USB DMA (and "DMA3/4" in that note) are separate and do not halt the CPU.

### 1.2 What the stock firmware uses DMA for (evidence)
* **DMA1 only in v04**: `DmaDriver_*` writes the 13-byte descriptor to ports 06..12, `out (0x13),1`, polls bit0. Occurrences: MAIN_AP bank63 28:99 (GIF/AMV
  decoder data, DSP images, `DSCDec.DSP/GifDec.DSP` strings next to it), MUSIC_AP bank0B 2A63 / bank0F 27D3 (MP3/ADPCM bitstream to the DSP data RAM, "StartMoveBitstream"),
  VOICE_AP bank09, FMRECORD/LINE/RECORD bank0C (PCM). All are ZRAM <-> DSP-RAM lane moves, always with port 05h set first (05h = 0x24/0x25/0x26/0x34/0x40).
  Convention seen: ZRAM endpoint = byte3 0x00 + ISA/IDA register value 0x08; lane endpoint = byte3 0x40 + ISA/IDA = the port-05-like value.
* **DMA2 only in the older 913f_b firmware** (RECORD/MUSIC/VOICE/FMRECORD/BUILDLIST/PDDRM/M-RECORD, e.g. RECORD_AP bank37 2108-2166: mem-to-mem, mode 0, cmd 1, poll).
* **LCD: no DMA.** UI_6B33*.DRV never touches 06-13 (CPU `ld (0x8000),a`; port 02 = 0x18 + port 00 |= 0xC0 around it, UI_6B33S_DRV seg_text.s:45DC).
* **NAND/FTL: no DMA.** None of the UD_*/VM_NANF/FS_* drivers write ports 06-21; data moves by CPU through the 0x8000 window with the 2Ah sequencer.
* **DSP boot/load**: `InitDSPCode` is OS code using DMA1/05h (see dsp_interface.md section 5); not re-verified here.

=> Both channels are idle while zos runs hosted. **Recommendation: LCD = DMA2** (DMA1 stays free for the DSP bitstream/PCM feed zos will need for audio;
the stock player uses DMA1 for exactly that). The prototype defaults to DMA1 (layout proven by the v04 code), `-DPERF_DMA2` switches the whole module to DMA2.

### 1.3 Can DMA speed up the LCD? Verdict: **probably yes, 2-4x for fills/blits, no CPU overlap. Unverified on hardware.**
Why it is plausible:
* The CPU needs 7 T + 3 wait states = 10 T per store = 407 ns/byte at 24.576 MHz (DS p.29 "write cycle time 407 ns" is exactly 10 cycles). Measured full fill 15.6 ms (dev) =
  32768 bytes x 476 ns (loop overhead included). Model check: `ld (hl),b` x16 + 26 T of loop = 186 T per 16 bytes -> 2048 x 186 T / 24.576 MHz = **15.5 ms** = measured.
  So the measurement independently confirms **CPU = 24.576 MHz with 3 wait states** (inf, strong).
* The DMA bus cycle has its own wait-state field (mode bits 7-6) and no instruction fetch/loop: expect 2-5 clocks per byte (inf) -> 4-8 ms per full screen. The LCD side (SSD1789A,
  8080 write cycle) is the limit; the minimum write cycle time is not known (the SSD1789A PDF is not text-extractable here).
* Destination address is irrelevant to the LCD (no address pins, CE3 only), so the ordinary **incrementing** destination works: no fixed-destination mode needed.
  A0 (GPIO C2 = F4.2) must stay 1 (zos leaves it at 1 after every command).
* Fill: no "fixed source" either, so fill from a repeated buffer: 256-byte pattern (128 pixels = one row), 128 restarts of a 256-byte DMA per screen. Restart cost with
  re-programming all 14 registers is ~250 T (10 us); a start-only restart (counters reload from the registers on the rising edge of bit0, per PG) is ~20 T. Max 32 KB per
  start, so a larger pattern (e.g. 2 KB) cuts restarts further, but ZRAM1 space is scarce (361 bytes free above the zos image).
* Blit from RAM: source = ZRAM1 buffer (<= 16 KB) or a DSP lane page (SA3 = 0x40 + ISA lane code; lane addressing/ownership rules are unverified, see risks).
* Glyphs (48 px = 96 B): per-glyph DMA setup (~250 T) vs the CPU's ~0.26 ms: not worth it; keep the unrolled asm. Gains are for fills, image blits, scroll redraws.

Unknowns the prototype is built to answer (visually + by timing): (a) is BC = n or n-1 (stock uses BC = 0x01FF for a 512 B sector, suggesting n-1), (b) does the DMA bus cycle
with 0-3 wait states keep the LCD happy, (c) do the source/dest counters reload on every start (quick restart), (d) the real speed.

### 1.4 Risks
* A wrong external address could select NAND (CE1/CE2) and write garbage into the flash controller bus. DA3 = 0x8C (CE3) and port 02 = 0x18 both decode to the LCD, so any decode scheme hits the LCD.
* Hosted constraint: the DMA source buffer in the prototype is ZRAM1 0x2B00-0x2BFF (free gap between the zos image end 0x2A97 and the launcher stub 0x2C00; contents saved and restored).
  If core grows past 0x2B00 move it. The module itself lives in a DSP lane page, so DMA must never target that lane; the prototype never uses a lane.
* A DMA that never finishes freezes the CPU (it is halted); the zos watchdog then resets into the stock firmware (harmless). The prototype also polls with a timeout in case the CPU is not halted.
* Do not write 25h bit0 (software DMA reset pulse).

## 2. System clock

### 2.1 Facts
* Crystal: **24.576 MHz HOSC** (DS p.14 section 4.7). 32.768 kHz LOSC for RTC/standby. PLL1 generates multiples of 48 MHz for DSP/USB; PLL2 22.5792/24.576 for audio.
* **Port 00h = MCU clock control** (PG p.9-10): bits 7-6 external-memory wait states (0-3), bit 4 clock source, bits 3-0 divider (0 /1, 1 /2, 2 /4 ... 10 /1024; 11 and 15 = stop CPU).
  Source bit4 (if 70h.3 = 0): 0 = LOSC 32 kHz, 1 = HOSC 24.576 MHz. If **70h.3 = 1**: bit4 0 = 60 MHz (= DSPCLK/4), 1 = 48 MHz. Changes synchronise to the /1024 clock.
  **70h bit3 = "MCU clock = PLL DSP clock"**; bits 2-0 DMA clock divider; bits 5-4 B1/B2 to MCU or USB (default 70h = 0x30).
* **Stock normal run state = HOSC /1**: STANDBY_AP seg_text.s:0A42-0A53 (wake-up path) does `42 |= 0x80` (HOSCEN), then `out (0x00),0x10`; the standby path (09DC) does `out (0x00),0` = LOSC 32 kHz
  + `42 &= 0x3D` (HOSC/PLL off). Together with the timing model in 1.3 this gives CPU = 24.576 MHz, 3 wait states during LCD. The LCD driver ORs 0xC0 into port 00 around every LCD access
  (UI_6B33S_DRV seg_text.s:45F2) and restores it after; zos sets 0xC0 for the whole session (platform.c hal_init), so the effective value is 0xD0.
* The boot clock is set by BROM/hardware reset: BRECF641.BIN and SYSCFG.SYS contain no `out` to ports 00/42/70/4F (scanned); default 00h = 0 (LOSC) so the BROM must switch to HOSC. (inf)
* **DSP clock = port 42h bits 4-2** (000 = 48 MHz/12 MIPS, 001 = 96/24, 010 = 144/36, 011 = 192/48, 100 = 240/60, 101 = 288/72, 110 = 336/84, 111 = 120/30), 42h.6 PLL1EN, 42h.5 M48EN,
  42h.7 HOSCEN, 42h.1 PLL2EN, 42h.0 FSSEL. Stock music start (MUSIC_AP seg_bank02.s:2560): `42 = (42 & 0xE3) | 0x0B` -> bits 4-2 = 010 = **144 MHz / 36 MIPS**, PLL2 on, 44.1k family;
  then `42 |= 0x40` (PLL1 on), delay, `2E = 0x7B`, `2F = 0x03`. DSP request 0x77 ("needs more MIPS") steps bits 4-2 up one (cap field 0x0C = 011 = 192 MHz / 48 MIPS). The stock firmware never goes above 48 MIPS.
  USB mode (USBDISK_AP seg_bank20.s:1648): `42 |= 0xE0` (HOSCEN+PLL1EN+M48EN), 70h bits 5-4 mapping; no change to the MCU clock.
* DS p.11: "MCU up to 60 MHz by software" (only possible through the PLL path 70.3 = 1). DS p.30 (IVDD vs frequency, VDD = 2.0 V): MCU at 24.576 MHz /1 = 5.9 mA, /2 3.3, /4 1.9 ...;
  table stops at 24.576 MHz. DSP rows: 6-48 MIPS at VDD = 2.0 V, **60/72/84 MIPS need VDD = 2.3 V**.
* **Power**: PMU has two DC-DC converters + one linear regulator (DS p.14). Defaults VCC 3.0 V, VDD 2.0 V. **Port 4Fh** (PG p.49): bit7 internal NMOS switch, bit6 DCOP output enable, bits 5-3 VCC 2.6-3.3 V,
  bits 2-0 VDD 1.6-2.3 V (0.1 V steps; reset value 0xEC). Stock MAIN_AP seg_bank40.s:14C8 writes **0xA5** at start = VCC 3.0 V, VDD 2.1 V (inf: step mapping 1.6 + 0.1 x n). The user's board mode
  (DCDIS/BATSEL pins) decides whether VDD comes from the DC-DC or the regulator; unknown.
* **What does not depend on the CPU clock** (so zos' tick survives clock changes): the 10 ms tick is the RTC/LOSC divider (4B-4D, 43h/44h), the watchdog (4Eh, 1 kHz..1 Hz = LOSC family, inf), GPIO.
  **What does**: external bus timing in T units (DS p.24: address/data setup 0.5-1.5 T), NAND state machine and ECC (MCU clock), CTC (22h-24h, "undivided MCU clock"), DMA clock (70h, divider of MCU/PLL clock, inf),
  zos busy loops (key scan settle loop `for i<32`), stock drivers' software delays (calibrated for 24.576 MHz: the 5 us I2C padding, USB delays, UD_* waits). Any stock call (bridge_enter) must run at the stock clock.
* Timer base for the 10 ms tick: unchanged (LOSC-derived). Ticks and the 2 Hz event stay right at any CPU clock; verified conceptually only, the prototype demonstrates it (spin count scales, ticks do not).

### 2.2 Is it safe to bump the Z80 clock from zos? Verdict: **step down is safe; step up to 48 MHz is plausible but only the prototype can prove it; 60 MHz needs VDD 2.3 V (do not).**
* Down (safe, documented): `out (0x00), (0xD0 & 0xE0) | 0x01` -> 12.288 MHz (divider /2). Restore: `out (0x00),0xD0` (the value read at entry). No effect on ticks.
* Up to 48 MHz (documented in PG, **never used by stock** for the MCU, inf for the glitch-free behaviour):
  1. `out (0x42), in(0x42) | 0xE0` (HOSCEN, PLL1EN, M48EN: exactly the stock USB enable); wait >= 30 ms for lock (stock waits ~70 us after an already-running PLL).
  2. `out (0x00), (cur & 0xE0) | 0x10` (bit4 = 48 MHz when 70.3 = 1, divider /1, keep wait states).
  3. `out (0x70), in(0x70) | 0x08` -> **CPU now at 48 MHz** (the only switch). Bits 5-4 of 70h (B1/B2) untouched.
  4. Restore in reverse: `out (0x70), saved` (HOSC back), `out (0x00), saved`, `out (0x42), saved`.
  Expected: 2x everything on the CPU side; LCD fill 15.6 ms -> ~8 ms **if** the LCD accepts a 208 ns write cycle (10 T at 48 MHz). If not, keep 70.3 = 1 but raise nothing else: wait states are already at the maximum (3),
  so the only remedy is a lower clock (e.g. 00h divider /2 on the 48 MHz source = 24 MHz, pointless) or doing LCD streaming by DMA (DMA clock is separately divided by 70h bits 2-0).
* Wait states: port 00h bits 7-6 apply to **external** memory (the LCD, NAND window, ROM bank). Internal ZRAM/DSP-lane code needs none (inf); at 48 MHz keep 3 for the LCD; NAND timing (reads/programs through the
  2Ah sequencer and the 8000 window) was tuned for 24.576 MHz: do **not** run NAND, the stock FS bridge, or I2C/FM at 48 MHz. zos must drop to HOSC before any bridge call and before exit.
* Voltage: stock VDD = 2.1 V (inf). The DS says DSP at 48 MIPS is specified at VDD 2.0 V, so a 48 MHz MCU clock is inside the validated envelope (inf: same PLL/clock tree); 60 MHz would require the 2.3 V setting `out (0x4F), 0xA7`
  (VDD step 111 = 2.3 V) and the DS gives no MCU-at-60-MHz current/spec. Not recommended and not implemented.
* Watchdog: kicked only from main loop; a clock hang resets (clock source for the WD is LOSC based, inf). Keep kicking inside long loops (prototype does).
* What zos must change for a permanent speed-up: (1) per-mode clock policy (idle: divide down /2.../16 with HALT, as DS p.30 shows 5.9 -> 0.58 mA), (2) bracket every bridge/NAND/I2C access with "clock = stock",
  (3) keep the 10 ms tick on RTC (already), (4) LCD wait states stay 3 unless the SSD1789A write cycle is verified, (5) do not touch 4Fh.
* Power/battery: PLL1 on adds a few mA; M48/PLL1 are also what stock turns on for USB and DSP playback, so the battery monitor/low-battery NMI thresholds (DFh) already cover it.

## 3. Prototype: zos\modules\perftest (PERFTEST.ZM)
Menu (RIGHT/LEFT select, PLAY run, MENU exit). Top lines print ports 00/42/70 (read-only). Items:
1. CPU CLOCK MEASURE: counts a calibrated 26 T/iteration loop over 20 ticks (200 ms) -> `CPU X.01MHZ nnnn` (expect ~2450 at 24.576 MHz; ISR load and tick granularity give ~1 % error).
2. LCD FILL BY CPU: 32 full fills via `hal_lcd_fill`, prints us per fill (expect ~15600).
3. LCD FILL BY DMA: 32 full fills by DMA from a 256-byte red pattern at ZRAM1 0x2B00, 128 starts of 256 B per screen; holds the final red screen 1.2 s (look: solid red?) then prints us per fill and the DMA end flag.
   Settings (PLAY toggles/cycles): DMA COUNT (N-1 or N), DMA WAIT STATES (3,2,1,0), DMA RESTART (FULL = reprogram all registers per row, QUICK = only the start bit).
4. CLOCK /2 TEST and CLOCK 48MHZ TEST: confirmation screen (PLAY = go, any other key cancels), change the clock, measure for 200 ms with no LCD access, restore ports 00/42/70, report MHz x 100.
   Refuses ("UNEXPECTED CLK STATE") unless port 00 low 5 bits = 0x10 and 70.3 = 0.
Build: `python zos\build.py` (PERFTEST.ZM 2.7 KB). Not run on hardware or emulator by me (the emulator models DMA as instant memory-to-memory without an external destination).

### What to check on the device
1. Item 1 should read about 2450 (= 24.5 MHz). A very different number means the stock clock is not HOSC /1 (the ports line shows the values).
2. Item 2 should be ~15600 us (matches the earlier measurement).
3. Item 3 with the defaults (N-1, WS 3, FULL): is the screen solid red afterwards? If it is a different colour (e.g. blue/green) the count convention is off by one byte: set DMA COUNT = N and retry.
   Stripes/garbage = the DMA/LCD path is not as assumed (report). If solid: note the us/fill, then try WS 2, 1, 0 (watch for flicker/noise = LCD too slow) and RESTART = QUICK (garbage in later rows = counters do not reload).
4. Items 4 (/2): expect ~1225. 48 MHz: expect ~4900 and ticks unaffected; if the screen freezes the watchdog should reboot to stock; report what the numbers/behaviour were. Do this one last.
