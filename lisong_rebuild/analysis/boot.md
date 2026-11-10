# Power-on sequence: who draws the hourglass, what the seconds are spent on

Firmware: `my-ximpro.fw` (stock LSD_501_04) on the NAND dump `readback-my-ximpro.s1clone`, run in
`atj2085emu` with the real BREC init (`--nand`). Scope: research only, nothing on the device, nothing under
`zos/` touched. Method: PC/port/NAND tracing of the real BREC + kernel code (scripts were throw-away, in the
session scratchpad), LCD screenshots with `headless.py ... --shots "T:file.png"`, static disassembly of
BREC (`BRECF641.BIN`, md5 9e3174977a640d24fa0f2683c3645233 = the copy in `ref/myximpro/`), BROM and the UI driver.

## 1. Short answer

* **The hourglass is drawn by BREC** (the 16 KB boot record loaded by the mask ROM), function
  **BREC 0x3000-0x30C7**, called first thing from BREC 0x1B12 (`call 0x3000`, 0x1B12 is called from
  BREC main 0x0200/0x0209). No kernel and no AP is running yet. Bitmap = 1440 bytes at **BREC 0x3161**
  (20 x 36 px, RGB565 big-endian), blue background fill (0x4410 words of 0x001F = 132x132 px), init table at **0x3100**.
* **During the hourglass the CPU is running BREC's own boot work, not waiting for anything external**:
  (a) NAND/flash detect, (b) the **FTL mount** (spare-area scan of all 4096 units + table rewrite, ~15 % of
  the time to MAIN) and above all (c) a **checksum verification of the whole firmware image** (BREC 0x0600
  -> 0x0875: every directory entry except `.$$$` and `.RES`, 72 files, 3190 sectors = 1.6 MB, ~51 % of the
  BREC time) and finally loading SYSCFG.SYS. The hourglass is just the "busy" picture painted before these steps.
* The hourglass ends when **MAIN.AP installs the UI driver** (`UI_6B33*.DRV`, init at 0x4813): it runs the same
  LCD init sequence again (LCD reset pulse, backlight OFF, one 200-unit reset-settle wait plus four 200-unit
  table waits, black fill, backlight ON).
  That costs another ~26 M T-states. Then MAIN.AP draws the startup animation.
* **TROM is not involved.** It is not boot code at all (see 5).

## 1.1 BREC control flow and handoff

The relevant call path in the captured BREC is:

```
BROM: load/check BREC into RAM 0000-3FFF
  -> BREC 0200: set initial stack, call 1B12
       -> 3000: select LCD and initialise/paint hourglass
       -> 150D: detect flash devices
       -> 0A36: mount the FTL (scan and rewrite its tables)
       -> 2075, then 0600: check the firmware image; any failed check takes the 2063 error path
  -> BREC 0216: find and load SYSCFG.SYS and its vector page
  -> return to 0200: set SP=0400 and jump to 0600
       -> SYSCFG.SYS owns execution; it loads MAIN.AP in the stock boot
```

BREC is executing from the 16 KB image at address zero; these calls do not involve an OS or LCD driver.
On the successful path, `0216` loads the kernel image at `0600` and its vector page at `0000`. After
`0216` returns, the `0200` entry installs the kernel stack and transfers control to `0600`. On boot-check failure,
BREC goes to `2063`, the recovery/error path that reaches the mask-ROM ADFU recovery code; this is
distinct from the earlier BROM path used when the BREC itself cannot be loaded or verified.

## 2. The hourglass code (BREC 0x3000)

```
3003  in a,(4Eh) ; or 40h ; out (4Eh)       ; (watchdog/ctl bit, as in BREC elsewhere)
3009  save port 02 -> (314Fh) ; out (02),18h ; CE3 = LCD window at 0x8000
3012  out (EEh),09h ; EE &= ~04 ; F3 = (F3 & ~40h)|04h
3024  delay 10  ; EE bit0 = 0 (LCD reset low) ; delay 10 ; EE bit0 = 1 ; delay 100
3041  walk table at 3100h: FDh x = command x (A0=F4.2 low), FEh n = delay n units, FFh = end, else data (A0 high)
3068  fill 0x4410 px with 001Fh (blue)               ; 132x132 GRAM
307E  cmd 15h (col) 36h..49h, 75h (row) 2Eh..51h, 5Ch (write RAM) -> 20 x 36 window
30A4  ldir 3161h -> 8000h, 05A0h bytes              ; hourglass pixels
30AF  delay 200 ; delay 100 ; EE bit1 = 1           ; BACKLIGHT ON
30BF  restore port 02 ; ret
```

* Delay routine 0x30E6 (`C` = count of 250-iteration loops of 5 x `in a,(4Eh)` + `set 3,a` + 2 x `out (4Eh)`):
  1 unit = **~24.4 k T-states** in the emulator, i.e. 0.99 ms if the CPU runs at 24.576 MHz with 1 T per clock
  (see 6: this is the best clue about the real clock).
* The init table (0x3100) is an SSD13xx-style command set (`15h/75h/5Ch` column/row/RAM write, `AFh` display
  on, `81h` contrast ... ) and contains four `FE C8` waits (4 x 200 units). The UI driver's reset setup adds
  a separate 200-unit settle wait before using its copy of the table.
* The UI driver contains matching reset, command-table and fill routines (runtime 0x482A.. in
  `UI_6B33S/L/T`, `UI_6B_JP/KR/L2-L5/TH.DRV`, file offset 0x2A21), with a black fill (`ld de,0000h`,
  0x4889) instead of blue and no bitmap. Its `UI_6B33S` table uses `81 38 06` where BREC uses
  `81 21 07`; the initialization sequence is shared, but not every setting is identical.
* BREC uses F3/F4 for GPIO_C2, the LCD command/data select; port EE bit0 toggles LCD reset and bit1
  enables the backlight.

### 2.1 LCD hardware setup and paint operation

The routine temporarily maps the LCD onto the external memory window rather than using a display
driver or DMA:

1. It saves port `02h`, writes `18h` to it (CE3, the LCD chip select), and later restores the saved
   page. With CE3 selected, a CPU store to `8000h` is an LCD bus write. The ATJ2085X guide identifies
   port `02h` as the external-memory high-page/CE selection; see [atj2085_hw.md](atj2085_hw.md).
2. It sets port `EEh` to `09h` (reset deasserted, backlight off in the observed wiring), configures
   GPIO_C2 as an output via port `F3h`, then pulses LCD reset low/high with `EEh.0`, with programmed
   delays between transitions. Port `F4h.2` is the command/data select: low before command bytes,
   high before parameter or pixel bytes. The code writes each byte to `8000h`; no LCD DMA is used.
3. It interprets the byte table at `3100h`: `FDh` means the next byte is a command, `FEh` means a
   delay count, `FFh` ends the table, and other bytes are parameters/data. The commands configure
   power, contrast, scan/output mode and the full 132x132 address range, then turn the display on
   and enter RAM-write mode. The command set matches the SSD1789A-style interpretation in
   [atj2085_hw.md](atj2085_hw.md#61-lcd-init-opcodes-s6b33b-meaning-versus-actual-ssd1789a-meaning);
   that is stronger evidence than the old S6B33B label, but the exact LCD part on this board has
   not been confirmed from a board-level measurement.
4. It writes `4410h` pixels of RGB565 `001Fh` (blue) to fill the 132x132 GRAM. It then sets a
   smaller column/row window (`36h..49h`, `2Eh..51h`, 20x36 pixels), re-enters RAM-write mode,
   and streams the 1440-byte bitmap at `3161h` directly to `8000h`. The bitmap is RGB565 byte
   data, big-endian. Finally it waits 200 and 100 delay units, sets `EEh.1` to enable the backlight,
   and restores port `02h`.

The initialization table itself, with the parser's `FD`/`FE` markers omitted, is:

```
D1; wait C8; 94; 20 0B; wait C8; 81 21 07; wait C8;
F3 90 05 60 84; FB 02; F2 08 50; wait C8;
F7 28 0E 05; A7; CA 00 1F 00; BC 00 01 00; BB 01;
F1 E0 00 00; 94; 15 00 83; 75 00 83; 44 04; 82 02; AF; 5C
```

The four `C8` table delays are 200 units apiece. `D1`, `94`, `20`, `81`, `FB`, `F2`, `F7`,
`CA`, `BC`, `BB`, `F1`, `15`, `75`, `44`, `82`, `AF`, and `5C` correspond to the controller
operations listed in the hardware note; `F3` remains undocumented in the SSD1789A Rev. 1.0 table.
The blue background and icon are painted after the init table returns, so the image is in GRAM
before the backlight becomes visible.

## 3. Timeline (emulator, T-states from BREC entry 0x0200)

Seconds are given for the two candidate clocks: **A = 24.576 MHz, 1 T = 1 clock** (HOSC, what BROM selects with
`out (00h),10h`; consistent with the 1 ms delay units) and **B = 12 MHz** (the emulator's pacing GUESS, `soc.py CPU_HZ`).
NAND busy/ready time is NOT modelled in the emulator (reads are instantaneous), so real times are >= these.

| T-states | A (s) | B (s) | what | code |
|---|---|---|---|---|
| 0 | 0 | 0 | BREC starts (BROM has loaded + sum-checked it, ~ms). LCD still off/black, backlight off | BREC 0x0200 |
| 0 - 22.5 M | 0.92 | 1.88 | LCD reset pulse (10 + 10 + 100 units) and init table with four 200-unit waits; screen black | 0x3000 |
| 22.5 - 23.6 M | 0.95 | 1.96 | blue fill, then hourglass bitmap written to LCD RAM (not yet visible: backlight off) | 0x3068, 0x30A4 |
| 23.6 - 30.94 M | 1.26 | 2.58 | final 200 + 100 unit waits | 0x30AF |
| **30.94 M** | **1.26** | **2.58** | **backlight ON: hourglass visible** (EE bit1, pc 0x30BF) | |
| 30.96 - 32.2 M | 1.31 | 2.68 | flash detect (CE1 ID, family 1, geometry), `0x150D` | 0x150D, 0x0A36 |
| 32.2 - 51.84 M | 2.11 | 4.32 | **FTL mount**: 4093 loop iterations at 0x22F3 (units 2..0xFFF), 2 spare reads each = 8187 reads, then table rewrite: 16 page programs + 4 erases (temp units + table block, both zones) | 0x2075, 0x22F3-0x2406 |
| 51.84 - 105.49 M | 4.29 | 8.79 | **firmware image verification**: header `55 AA F0 0F` + checksum, directory (32-byte entries), then per file `0x0875` = 32-bit sum over the file's sectors; 72 files, 3190 sector reads (reads 8256 -> 11446), ~16.8 k T per sector. Skips `.$$$` and `.RES` entries (fonts/tables/pictures) | BREC 0x0600, 0x06DB, 0x0875, 0x091F |
| 105.50 - 105.61 M | 4.30 | 8.80 | load SYSCFG.SYS (dir lookup of "SYSCFG  SYS", 0x0216/0x023A, ~30 sectors) to 0x0600 | 0x0216, 0x3FDB |
| 105.61 M | 4.30 | 8.80 | kernel (SYSCFG) starts; vectors at 0x0000 now the kernel's | 0x0600 (SYSCFG) |
| 105.6 - 109.5 M | 4.46 | 9.13 | kernel init, MAIN.AP loaded and started; MAIN does a VM write of sector 0 (UD write+flush at 107.47 M: ~257 programs + 4 erases) and installs drivers (RST 10h) | |
| **109.53 M** | **4.46** | **9.13** | **UI driver init: LCD reset low, EE = 09h = backlight OFF**; hourglass is gone from the user's view (real LCD is reset, see note) | UI_6B33S.DRV 0x4813/0x4829 |
| 109.53 - 135.55 M | 5.52 | 11.30 | LCD init again: 20 reset units, one 200-unit reset-settle wait, four 200-unit table waits, matching command-table flow with driver-specific values | 0x4855, delay 0x460C |
| 135.55 - 135.75 M | 5.52 | 11.31 | backlight ON (`or 2` at 0x4825), black fill of 132x132 | 0x4883 |
| ~137 - 150 M | ~5.9 | ~12.2 | MAIN.AP reads LOGO.RES / animation data from NAND and starts drawing; first animation content ~145-150 M, full colour frame by 170 M | MAIN.AP |

Screenshots (in the session scratchpad `s_<T>.png`): blank at 1-20 M, **hourglass on blue at 40, 60, 80, 100, 110, 120 M**, black at
140 M, animation at 170 M. Note: the emulator's LCD model does not clear the GRAM on the reset pulse, so
its screenshots still show the hourglass for 109.5-135.5 M; on the real device the backlight is off then
(EE 09h) and the LCD controller was just reset, so that period is dark/blank (inferred, not verified on hardware).

Visible to the user (hourglass on, backlight on): **30.94 M -> 109.53 M = 78.6 M T-states = 3.2 s (A) or 6.5 s (B)**,
followed by ~26 M (1.1 s A / 2.2 s B) of dark screen, then black, then the animation.

### Where the 135 M T-states to the first black screen go
| phase | M T-states | share |
|---|---|---|
| BREC LCD init + hourglass (0x3000) | 30.9 | 22.8 % |
| flash detect | 1.2 | 0.9 % |
| FTL mount scan + table rewrite | 19.7 | 14.5 % |
| FW image checksum (0x0600) | 53.7 | 39.5 % |
| SYSCFG load | 0.1 | 0.1 % |
| kernel + MAIN start (incl. VM write) | 3.9 | 2.9 % |
| UI driver LCD re-init (mostly fixed delays, 24.4 M of it) | 26.0 | 19.2 % |

**Dominant cost: the BREC firmware checksum (40 %), then the LCD init waits twice (42 % together), then the mount (15 %).**
Nothing is "waiting for hardware to become ready": the fixed delays are programmed waits, the rest is CPU work.

## 4. Details of the BREC phases

* **Mount** (nand_ftl.md 2.4): unconditional on every boot. Per unit it reads two spares (bad marker, 0xBB
  table marker, logical number), then erases the temp units and appends a new copy of the table (identical to
  the previous one in the dump). So every power-on **writes NAND** (16 programs + 4 erases) during the ~1.6 s
  after the hourglass appears; a power loss there is the risky window (the rule for two units with the same L
  is the still-unanalysed BREC 0x23A4-0x2406).
* **Firmware verification**: BREC 0x0600 reads the FW header at logical sector 0 (and a second copy at +0x4000 bytes,
  compares), checks `55 AA F0 0F` and the word-sum of the header, then walks the directory. Every entry whose
  extension is not `$$$` (0x24 at +8) and not `R?S` (`.RES`) is summed: expected value in the entry
  (+0x14..0x17 area, RAM 0x0C74/0x0C76), computed by 0x091F (unrolled 16-bit adds into IY / DE). A mismatch jumps to
  the error path 0x2063 (retry/`jr`, no boot). Total image is 6.4 MB (12500 sectors); verified part is 1.6 MB.
  The cost per sector (~16.8 k T) is CPU-bound (checksum loop) rather than NAND-bound in the emulator.
* **SYSCFG load** (BREC 0x0216-0x0373): the directory is searched for the 8.3 name `SYSCFG  SYS`, the file
  is read to RAM 0x0600.., a final block goes to 0x0000 (vector page), then `jp 0x0600`. `ld sp,0400h` first.

## 5. TROM

* Static scan of the BROM (8 KB, `brom_8k.bin`, code 0x8030 reset, loader 0x8100->0x4000, recovery 0x9000->0x0000):
  the only `out (01h)` values are 0 and 1/2 inside the NAND page-read helper (running at 0x4540-0x456A, **with
  port 02 = 08h = CE1**, i.e. NAND address-lane selection, not ROM banking) and the restore of the saved
  value. No `out (03h)`. There is no instruction sequence that makes 01h = 02h with 02h = 00h (the bank-2 = TROM
  condition, atj2085_hw.md). BROM only runs BREC (verified by `0x55AA` at BREC+0x3FFC and the 16-bit sum at +0x3FFE)
  or, if that fails, the ADFU recovery code at 0x9000.
* Dynamic check, emulator, 300 M T-states of BREC + kernel + MAIN: port 01 values {0,1,2}, port 02 values
  {00,08,10,18,20,28,30}; the state (01h=02, 02h=00) never occurs; port 03 is never written.
* And TROM is already identified as the DSP coefficient table ROM (24-bit Q23 FFT/DCT twiddles etc.,
  `analysis/trom_21k.bin`, device-readable with 02=00/01=02), not boot code. The user's suspicion that TROM
  causes the boot delay is therefore ruled out: the delay is BREC software (BROM+TROM take ~nothing).

## 6. How sure are the seconds?

* T-state counts are exact for the emulated code. The conversion to seconds depends on the real clock: BROM
  sets `out (00h),10h` = HOSC 24.576 MHz /1, 0 wait states (dma_clock.md: boot clock is set by BROM). The delay
  routine unit (250 loops = 24.4 k T) is almost exactly 1 ms at 24.576 MHz and the table values (10, 100, 200)
  look like ms, so **column A (total ~5.5 s to the black screen, hourglass 3.2 s) is the likelier one**, column B
  is the emulator's old 12 MHz guess. A stopwatch/video of the real power-on (hourglass duration, dark gap,
  animation start) decides it: A predicts hourglass ~3.2 s + ~1.1 s dark + animation at ~5.9 s.
* Not modelled in the emulator: NAND page-read busy times (the mount and the checksum do ~11.4 k page
  reads, i.e. extra real time of order 0.3-0.6 s if ~25-50 us each), watchdog, battery behaviour.

## 7. What this means for zos

1. **zos launched as an .AP cannot influence any of this.** An AP only runs after the kernel and MAIN.AP are up
   and the user picked it (~300 M T-states in the emulator). The hourglass, the mount, the 1.6 MB checksum and
   the later LCD re-init all happen before. Nothing in zos can shorten the boot as long as it is hosted that way.
2. **The hourglass cannot be removed or replaced without replacing BREC.** BREC is in NAND blocks 0/1,
   verified by BROM with a plain 16-bit word sum, so a modified BREC is technically easy to sign. But if BROM
   accepts a bad BREC there is no key/GPIO recovery (usb_msc.md 4.2: ROM ADFU only when BREC is unloadable),
   so this is exactly the "no destructive flash" risk class. Patching BREC is NOT needed for the stated goal
   (see 4 and below) and should stay out of scope; if ever done: develop and test it in the emulator, which
   runs the real BREC from the dump, and flash both copies only after the emulator boots to a correct state.
3. **The mount scan cannot be skipped without patching BREC** (unconditional, no "clean" flag found). It is not
   a table *load*: it rebuilds the maps from the spares and rewrites the table every boot. A zos that
   replaced BREC would have to do the FTL init itself (geometry, table at `0x3C80` descriptors, the cached
   word reader, bad-block/unit rules, temp-unit erase). Keeping the stock BREC keeps all of that and gives
   zos/the stock drivers the FTL RAM state for free. (Bonus risk to know: mount writes NAND on every boot.)
4. **A zos-native boot that keeps BREC intact is possible and saves most of the time** by changing only the
   FWIMAGE (the vendor-updater path, BREC and BROM untouched):
   * BREC requires only: header `55 AA F0 0F`, valid directory with correct per-file sums, and a `SYSCFG  SYS`
     entry (loaded at 0x0600, entered at 0x0600 with a vector page at 0x0000). It does not care what that file is.
     zos (or a thin zos kernel) can be the SYSCFG.SYS replacement, and the FWIMAGE can list just a few files.
   * Because BREC checksums every non-`.$$$`/`.RES` file at every boot (40 % of the time), **a lean image**
     removes most of it: with only SYSCFG.SYS + a few drivers the verify phase drops from 53.7 M to a few M. The cost
     is then BREC LCD init (30.9 M) + detect + mount (19.7 M) ~ 52-55 M T-states to the first zos instruction
     (2.2 s at A, 4.5 s at B) instead of 105.6 M to the kernel / 145-150 M to the stock animation.
   * The 26 M UI-driver LCD re-init is done by MAIN.AP's driver install, not by the kernel. A zos that is MAIN
     (the existing ZMAIN idea) or a zos kernel simply does not repeat it: BREC already left the LCD initialised,
     backlight ON and showing the blue/hourglass screen, so zos only has to start drawing (it must then not
     pulse the LCD reset). That removes both the dark gap and ~1 s/2.2 s. If zos wants a different splash than the
     hourglass it can only replace it after BREC has drawn it (from ~1.3 s / 2.6 s), never before.
   * Note the ZMAIN build is documented as not working yet (zos-build-deploy memory); this analysis does not explain
     why. Things to check there given the above: ZMAIN must keep the BREC-required directory/checksums valid; MAIN.AP is entered with DI and 0x0037 raised;
     the stock MAIN installs UI_6B33S/ky_hard/vm_nanf drivers itself (so a zos MAIN has to do/replace this).
5. Reducing the stock boot time with the least risk, in order: (a) leaner FWIMAGE (fewer verified files),
   (b) zos as MAIN without the LCD re-init, (c) only then consider a BREC patch (drop the second spare read /
   skip the four 200-unit waits in the 0x3000 table: ~20 M T-states = 0.8 s A; the mount skip needs the FTL rewrite described above).
6. The VM write at 107.47 M (UD write + flush of sector 0 with a copy-back of a whole 128-sector unit, ~257
   programs + 4 erases, ~2 M T) also happens on every boot of the emulated stock firmware; whether the real
   device does it every boot or only when settings changed is not established (emulator state).

## 8. Reproduce
```
cd atj2085emu
..\lisong_rebuild\.venv\Scripts\python.exe headless.py ..\my-ximpro.fw --nand ..\readback-my-ximpro.s1clone ^
  --ticks 400000000 --quiet --shots "40000000:a.png,60000000:b.png,140000000:c.png,170000000:d.png"
```
Hooks that gave the timeline: `emu.add_hle(addr, fn, passthru=True)` on BREC 0x3068/0x30A4/0x30B9, 0x150D,
0x0A36, 0x2075, 0x22F3, 0x0600, 0x0875, 0x3FDB, kernel 0x0600 and UI driver 0x4813; `emu._plog` wrapper for
port 01/02/03/EE/F3/F4 writes (note: patching `ATJ2085._out` at class level misses early writes in this
emulator; wrap the instance `_plog` instead).
