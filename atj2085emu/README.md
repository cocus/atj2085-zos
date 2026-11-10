# atj2085emu: emulator for the "Ximpro" ATJ2085 player (LISONG LSD_501_04, SDK V35.37)

**Status: the real MUSIC.AP plays MP3 files from an emulated user disk.** The real firmware
boots from the boot record (BREC) through SYSCFG, KCFG.AP and MAIN.AP to the colour main menu.
With `--disk`, the stock drive/FS code mounts a FAT16 volume (real UD_SF641.DRV install path,
real FS_FAT16.DRV; only the UD driver's sector entry is HLE'd). MUSIC.AP lists the files
(long names, ID3 titles), runs its duration scan (MC_LOAD), and plays them. The DSP is a
high-level model of MP3DEC.DSP's host protocol; the MP3 is decoded on the PC (pymp3). The player
clock, progress bar, total time and the bitrate update, and the PCM goes to the speakers
(`--audio`) and/or a WAV file (`--wav`).

**New: `--nand DUMP` = hardware-level NAND.** With the device's s1clone dump the emulator runs the
real BREC init (0x1B12: flash detect + FTL mount), the real firmware reader (0x3FDB) and the real
UD_SF641.DRV RST 18h path against an emulated NAND chip; no storage HLE is left. It boots with the
device's own settings and MUSIC.AP lists and plays the real MP3 from the dump (see "NAND model").

```
python run_emu.py [..\my-ximpro.fw] [--vm vm.bin] [--run] [--disk DIR|IMG] [--audio] [--wav out.wav]   # GUI
python headless.py [fw] --ticks N --png out.png --log out.log [--keys T:KEY:ms,...]
                   [--disk DIR|IMG] [--disk-mb 64] [--disk-save img] [--wav out.wav] [--audio]
                   [--shots T:file.png,...] [--dsp-verbose] [--no-dsp]
                   [--nand DUMP.s1clone [--nand-overlay ovl.bin]]       # instead of --disk
#   KEY = MENU LEFT RIGHT PLAY K0D, or USB+ / USB- (plug / unplug the USB cable)
python tools/make_test_mp3.py testdisk          # regenerate the two test MP3s (needs lameenc)
python tools/compare_wav.py out.wav a.mp3 ...   # locate + SNR against an independent decode
python tools/test_paging.py                     # port-0x05 window self-test
```
Use `..\lisong_rebuild\.venv\Scripts\python.exe`. Dependencies: `z80`; for audio also
`numpy`, `pymp3` (decoder used by the DSP model), `sounddevice` (live output), `soundfile`
(reference decoder in compare_wav only) and `lameenc` (test-file generator only). All were
installed into that venv with pip.

**Play an MP3 (headless):** boot to the menu (300 M T-states), MENU enters MUSICA, PLAY starts:
```
python headless.py --disk testdisk --ticks 660000000 --keys 300000000:MENU:200,380000000:PLAY:200 ^
                   --wav work/play_full.wav --shots 560000000:screenshots/07_player_tone_6s.png
```
GUI: `python run_emu.py --disk testdisk --audio --run`, then Enter (MENU) on "MUSICA", Space
(PLAY). Live audio follows the sound card's clock; the WAV path follows the emulated clock.

Speed: about 80 M T-states/s headless (playback included) and about 9 M T-states/s in the GUI
(default 400 k T-states per 33 ms frame). The menu appears after about 300 M T-states.

## Screenshots (`screenshots/`, made by `headless.py`; logs are in `logs/`)
| file | run | what is on the LCD |
|---|---|---|
| 01_boot_logo.png | 80 M T-states | "ximpro" logo over a blue sky and flowers (LOGO.RES animation, mid-sequence) |
| 02_main_menu.png | 300 M | main menu: "> MUSICA" title, CD + note icon, bottom bar with `|<<`, blue "menu" button, `>>|` |
| 03_menu_after_LEFT.png | LEFT at 300 M, shot at 340 M | menu moved to "> JUEGOS" (playing-card icon) |
| 03b_menu_after_RIGHT.png | RIGHT at 300 M | "> VIDEO" (film strip + camcorder icon) |
| 04_music_ap_disk_error.png | no `--disk`; MENU at 300 M, shot at 318 M | MUSIC.AP: white "Disk error!" on blue |
| 04b_back_in_menu_no_disk.png | no `--disk`; MENU at 300 M, shot at 500 M | back in the main menu after MUSIC.AP exited |
| 05_music_stopped_sweep.png | `--disk testdisk`, MENU at 300 M, shot 330 M | player after the duration scan: 001/002, ROOT, "Sweep mono 22k.mp3" (long name), total 0:00:08, stopped |
| 06_player_sweep_3s.png | + PLAY at 380 M, shot 420 M | playing, clock 0:00:03, progress bar |
| 07_player_tone_6s.png | shot 560 M | track 2 auto-started: 002/002, ID3 title "Tone 440-660 atj2085emu", 128KBPS, 0:00:06 / 0:00:12 |
| 08_player_tone_11s.png | shot 620 M | 0:00:11 |
| 09_after_both_tracks.png | end at 660 M | track 2 finished, player stopped (repeat off) |
| 10_launcher_lists_disk.png | homebrew-launcher-v04.fw, `--disk work/hbdisk`, LEFT, MENU | the homebrew launcher (GAME.AP) lists KEYTEST.AP / SNAKE.AP through the real FS driver |
| 11_after_usb_plug.png | USB+ at 300 M | USBDISK.AP ran (message 0x91) and returned (no USB host is emulated) |
| 12_nand_main_menu.png | `--nand`, 300 M | main menu with the device's saved theme (teal, big font), from the dump |
| 13_nand_music_resume.png | `--nand`, long PLAY at 330 M, MENU at 420 M, shot 800 M | MUSIC.AP on the real disk: 001/001 ROOT, "Live @ The Paradiso…" (ID3), 192KBPS, 3:55:04, paused at the saved resume point 0:28:56, JAZZ EQ, VOL 31 |
| 14_nand_playing.png | + PLAY at 850 M and 950 M, shot 1100 M | playing (clock restarts at 0:00:07 because the DSP model ignores the resume time; the audio is from 28:57) |

Logs: `logs/play_full.log` (DSP event list at the end), `logs/compare_wav.txt`, `logs/launcher.log`,
`logs/usb_plug.log`. WAV: `work/play_full.wav` (20.06 s: sweep then tone).

The UI language is Spanish. The VM is blank on a fresh run, so KCFG and MAIN write
their default settings first.

## Architecture
```
run_emu.py / headless.py
emu/soc.py      ATJ2085 model: z80.Z80Machine + port I/O + memory callbacks + HLE hooks + traces,
                port-0x05 window, DMA1, user-disk RST 18h HLE
emu/nand.py     hardware NAND controller + chip model over an s1clone dump (--nand), read-only + overlay
emu/disk.py     user disk backing store; FAT16 image builder (8.3 + VFAT long names) from a folder
emu/dsp.py      HLE of MP3DEC.DSP's host protocol + host-side MP3 decode (pymp3)
emu/audio.py    PCM sink: WAV (44.1 kHz stereo, seamless linear resampling) + live sounddevice
emu/lcd.py      SSD1789A-compatible LCD controller -> 132x132 RGB565 GRAM, 128x128 visible
emu/keys.py     GPIO-B key matrix as scanned by KY_HARD.DRV
emu/rom.py      internal mask-ROM C runtime (loaded from rom/comlib.s28)
emu/fwimage.py  AFI / FWIMAGE.FW parser
emu/gui.py      tkinter: LCD x3, buttons + keyboard, Run/Pause/Step/Reset/breakpoints,
                tabs: CPU (regs+disasm with ROM symbols), Memory, Ports, Syscalls, Log
tools/          zdis.py, xref.py, findb.py, romcalls.py, trace.py / trace2.py (single-step tracer,
                trace2 with keys/disk/DSP), systrace.py (named syscall trace), seg2bin.py
                (apdis .s -> binary), dumpdis.py, probe.py, frames.py, gui_smoke.py,
                make_test_mp3.py, compare_wav.py, test_paging.py
```
The CPU core is Kosarev's C `z80` package. The emulator uses breakpoints for HLE and
tracing, and memory callbacks only for the LCD window. Everything else runs natively.

### Boot path (real firmware code, with two HLE shortcuts)
1. BREC (`BRECF641.BIN`, from the AFI) is loaded at 0x0000-0x3FFF. Execution starts at
   0x0200.
2. **HLE:** `call 0x1B12` (BREC hardware, NAND and FTL init) is skipped. With `--disk` the
   HLE writes the flash-detect results the kernel needs (see "Storage model"). Port 0x05 is
   set to 0xF7 (the stock runtime value measured on the device).
3. **HLE:** the firmware-read entry at **0x3FDB** is served from FWIMAGE.FW.
4. BREC's own loader then copies SYSCFG.SYS: file bytes 0..0x2CFF go to 0x0600 and
   bytes 0x3700..0x377F go to 0x0000 (the RST vector page). It then does
   `jp 0x0600` (evidence: BREC 0x0216-0x0374).
5. SYSCFG runs KCFG.AP. KCFG installs vm_nanf.drv and UI_6B33S.DRV and checks the VM
   magic 0xDEED. SYSCFG then runs MAIN.AP. MAIN installs ky_hard.drv and the UI
   driver for the selected language (UI_6B33L.DRV), calls `fmradio.ap` with
   parameter 1 (init), shows LOGO.RES, then loads UI30.RES and runs the menu loop.

Everything after step 3 is unmodified firmware: the kernel, its bank switching,
driver loading and the AP loader.

## Memory map (as established)
| range | content | evidence / status |
|---|---|---|
| 0x0000-0x007F | RST/IRQ vector page (from SYSCFG file offset 0x3700). RST08 → jp 0x2E17 (fast API, A = fn; A<0x10 → 0x3FDB). RST10 / RST18 → jp 0x32BE (banked API, HL = fn). RST30 = syserr. 0x0038 = IM1 IRQ dispatcher on MINT_STATUS (0x26) bits. 0x0066 = retn. Byte 0x0037 = IRQ nesting counter. | disassembly of SYSCFG |
| 0x0080-0x03FF | kernel variables, message queues (0x010E keys, 0x0115 system), AP stack (SP 0x0400 at start) | SYSCFG |
| **0x0400-0x05FF** | **bank-A window**: one 512-B page per API group, loaded on demand (SYSBNKA.SYS, or the driver's bank-A pages for slots ≥ 0x80) | SYSCFG 0x2FE4/0x30A7 |
| 0x0600-… | SYSCFG init code at boot; later driver resident code (KY_HARD at 0x0600) and AP data/text (MAIN: data 0x0800, text 0x0B2F, banks 0x0E00 / 0x2400) | DRV/AP headers |
| 0x2E00-0x32FF | resident kernel (RST handlers, bank switch, IRQ, timers, message queue) | SYSCFG |
| 0x3300-0x3FFF | BREC tail (resident NAND read 0x3CAF behind the entry 0x3FDB). 0x3FE0-0x3FFA = VM_NANF resident stubs. | BREC, VM_NANF.DRV header |
| 0x4000-0x47FF | UI driver resident code (rcode 0x4000 len 0x800); buffers such as 0x41A2 | UI_6B33*.DRV header |
| **0x4800-0x4FFF** | **bank-B window** (SYSBNKB pages and driver bank-B pages, 0x800 B each) | SYSCFG 0x30F1/0x31B4 |
| 0x4000-0x7FFF | ZRAM2 / DSP IPM / DSP IDM window selected by port 0x05 (see "Port 0x05 window") | device probe + PG p.12-13 |
| 0x8800-0x8E77 | **mask ROM: IAR C runtime** (?L_ADDASG_L03 = 8889, ?L_LSH_L03 = 8920, ?UC_DIV_L01 = 8D0C, memmove = 8E5B, …). It is loaded from `Sdk_i_43/lib/comlib.r01` as linked in `lisong_rebuild/ref/sdklib/comlib.s28`. The firmware calls exactly these addresses. | **ASSUMPTION:** chip ROM equals this library (no ROM dump; TROM_L_M.rom is encrypted) |
| 0x8000-0xFFFF | external bus. When port 0x02 = 0x18 (CE3) every access goes to the LCD. Otherwise it is RAM or ROM. | UI driver 0x45DC; s1sys.inc (LCM CE3 at 0x8000) |

### FW-read entry 0x3FDB (HLE'd)
`DE → {u32 byte_offset; u8 ?; u16 dest; u8 page}`, `BC` = byte count, returns `A = 1`.
The offset is a byte offset into FWIMAGE.FW (sector 1 = directory of 32-byte entries
with a u32 sector field at +0x10). Offsets ≥ 0x1000000 address the **VM area**
(settings NAND). The emulator keeps this area in RAM as 512-B sectors and can persist
it with `--vm`.

### RST 18 while no UD driver is installed
VM_NANF writes settings with `ld a,1 ; jp 0x18` (0x3FEC) and `ld a,4 ; jp 0x18` (0x3FF1).
Without `--disk` no UD driver gets installed (the flash-detect table at 0x0080 is empty), so
the emulator implements A=1 as "write one 512-B VM sector {u32 sector, buf at +5}" and A=4 as
a no-op while 0x0018 still holds the kernel's `jp 0x32BE`. With a UD driver installed see
"Storage model" (the same VM sectors are then UD sectors 0..511).

### Interrupt counter (HLE, unverified)
SYSCFG's vector page initialises RAM 0x0037 = 1. Every kernel entry does di, then
`inc (0x37)`, then `dec (0x37)`, then `ei` only if the counter is 0. Nothing in the
loaded code ever brings the counter to 0, so interrupts would stay off forever. The
emulator sets it to 0 when SYSCFG starts. On hardware the skipped BREC init probably
accounts for this. **This needs checking.**

## Port map (what the emulator models; ✓ = confirmed from firmware code)
| port | use | model |
|---|---|---|
| 0x00 | MCU clock/wait states; LCD access ORs 0xC0 ✓ | latch |
| 0x02 | EM_PAGE_HI: 0x18 selects CE3 = LCD at 0x8000 ✓ | switches the 0x8000 window to the LCD |
| 0x05 | 0x4000-0x7FFF window: ZRAM2 / IPM / IDM lanes ✓ | full model, see "Port 0x05 window" |
| 0x06-0x13 | DMA1 (SA, ISA, DA, IDA, BC, mode, command) | memory-to-memory incl. DSP lanes and "DSP mode"; instantaneous; external (NAND) source not modelled |
| 0x2E / 0x2F | DSP control (bit7 run/reset, bit1 clock) / boot mode | forwarded to the DSP model |
| 0x30-0x37 | DSP host mailbox (HDR0-5, HSR6/7) | separate MCU->DSP / DSP->MCU latches in the DSP model |
| 0x25 / 0x26 / 0x27 | IRQ_STATUS / MINT_STATUS / MINT_ENABLE. The IRQ dispatcher tests 0x26 bits 7..0 (ADC, KEY, RTC, DMA/CTC, I2C/UART, USB, …, DSP) ✓ | 0x26 bit 5 = (0x44 & 3), bit 0 = DSP request; writing 0x26 clears bits; 0x25 bit 3 set at DMA1 end |
| 0x43 | RTC_CTRL; bit 3 = busy, polled at bank-A 0x042D ✓ | bit 3 clears on read |
| 0x44 | RTC_IRQSTAT, write-1-to-clear. bit 1 = fast tick → kernel timers (0x00A0.., which run the key scan) and key queue → message queue; bit 0 = slow tick → message 0x80 ✓ (SYSCFG 0x2ED8) | bit 1 every 10 ms and bit 0 every 0.5 s of emulated time at an assumed 12 MHz (device-confirmed: 10 ms OS tick, message 0x80 at 2 Hz) |
| 0x45-0x47 | RTC time | read 0 |
| 0x4E | watchdog: kicked with bit 3 everywhere; also used in delay loops ✓ | latch |
| 0x50 | USB status; bit 6 = cable/power ✓ (kernel 0x2F38) | bit 6 = `set_usb()` (headless `USB+`/`USB-`, GUI "USB cable"); the kernel then posts 0x91 / 0x92 (device-confirmed) and MAIN runs USBDISK.AP, which exits again (no USB host emulated) |
| 0xD0 | battery monitor, low nibble compared with 4/5/6 ✓ | 0x07 |
| 0xEE | GPOA: bit 0 = LCD reset, bit 1 = backlight, bit 2 set by the key driver | latch |
| 0xF0 / 0xF1 / 0xF2 | GPIO-B output enable / input enable / data: key matrix ✓ (see keys.py) | key model |
| 0xF3 / 0xF4 | GPIO-C; 0xF4 bit 2 = LCD A0 (0 = command, 1 = data) ✓ | latch, used by the LCD |
| others | DAC/ADC (0x80, 0xD4, 0xD5, 0xDD), DCDC 0x4F, LOSC 0x4B-0x4D, KEYSCAN 0xC1, … | plain latch, **logged with PC** |

Every port access goes into a ring buffer (`port_log`: tick, PC after the
instruction, direction, port, value) with per-port counters. The first 3 accesses
to each port that has no model are printed to the log with PC. The GUI's "Ports"
tab and the end of `headless.log` show both. RST 08/10/18/20/28/30 calls are traced
with A/HL/DE/BC and the caller in the "Syscalls" tab. Names for the API numbers are
in `lisong_rebuild/analysis/api_names.json`.

## LCD (milestone 3)
The controller is SSD1789A-compatible (init opcodes match the SSD1789A datasheet per the ATJ2085X guide cross-check; earlier notes said S6B33B). Its RAM is
132×132 at 16 bpp. Commands used by the driver: 0x15 column range, 0x75 row range,
0x5C RAM write, 0x5D RAM read, 0xBC entry mode (P1), 0x81 contrast, 0xAF/0xAE display
on/off. The init table also uses D1 94 20 F3 FB F2 F7 A7 CA BB F1 44 82. A pixel is
2 bytes, high byte first, RGB565. The RES bitmaps use the same format.

**Orientation, derived empirically:**
- P1 bit 1 = column inverse, P1 bit 0 = row inverse, P1 bit 2 = vertical-first scan.
- The visible glass is GRAM columns 4..131 and rows 0..127, rotated 180°. The driver
  adds +4 to rows in the row-inverse modes.
- With this model, both the bottom-up LOGO/UI30 bitmaps and the text come out upright.

## Keys
KY_HARD's timer routine (0x0600) scans GPIO-B; pressed reads as 1. Phase 1 reads
F2 & 0x66; phase 2 drives B6 and reads bit 1. Codes come from the table at 0x0709.
Names and codes are **confirmed on the device** (release = 0x30).

| button | GPIO line | key code | effect in the main menu |
|---|---|---|---|
| MENU | B2 | 0x01 | select / enter |
| LEFT (<<) | B1 | 0x04 | MUSICA -> JUEGOS |
| RIGHT (>>) | B6 | 0x05 | MUSICA -> VIDEO |
| PLAY (play/pause) | phase-2 B1 | 0x06 | (player: play/pause) |
| K0D | B5 | 0x0D | in the scan table, but no physical key produces it |

GUI keys: Left/Up = LEFT, Right/Down = RIGHT, Enter/m = MENU, Space/p = PLAY, k = K0D, plus
clickable buttons that you hold with the mouse, and a "USB cable" checkbox.

## Storage model (user disk, `--disk`) — driver-level HLE
**Why "Disk error!" happened** (traced): MUSIC.AP calls `DRV_ActiveUD(E=0 flash, C=1)`
(RST 10 HL=0x1300) and `DRV_ActiveDrive('C', 1)` (0x1101). DRV_ActiveUD (SYSBNKB 0x4AB6) reads
the flash-detect table at RAM **0x0080** that BREC's init (0x1B12 → 0x150D) fills; the emulator
skipped that init, so 0x0080 = 0 → "no flash" → 0xFF → no UD driver, no FS → "Disk error!".

**Option 1 (implemented, works):** reproduce only the detect results, let the real kernel load the
real UD driver, and HLE that driver's sector entry:

| item | value | evidence |
|---|---|---|
| RAM 0x0080 | bitmask of CEs with NAND: 0x02 (CE1) | BREC 0x1526-0x155C |
| RAM 0x0081 | flash family of CE1: 1 (→ `*641`) | BREC 0x1526; SYSBNKB 0x4B85 switch: 0→SF321, 1→SF641, 2→SF644, 0x20→SDMMC |
| RAM 0x0088+2n | size of CEn in physical blocks (UD_GetCap sums them) | BREC 0x154C; UD_*.DRV bank A02 |
| driver | C=1 → **UD_SF641.DRV**, C=0 → UD_FF641.DRV; rcode at 0x3300, UD_Init (API 0x0080) patches `jp 0x35E3` into 0x0018 | SYSBNKB 0x4B9B-0x4BE5, UD_SF641 bank A00 |

RST 18h contract of UD_SF641 (entry 0x35E3, `DE` → 8-byte struct copied to 0x361A):
`{u32 sector; u8 rsv; u16 buf_or_offset; u8 page}`, sector = 512-byte logical sector
(driver: `sector>>9` = logical block of 256 KB = 2 physical blocks, +0x40 blocks of firmware area).

| A | function | HLE |
|---|---|---|
| 0 | SectorRead → buf (page = port-05 lane, 8 = ZRAM) | ✓ |
| 1 | SectorWrite from buf | ✓ (in RAM; `--disk-save img` writes the image back) |
| 2 | WordRead: HL = u16 at byte offset (driver caches the sector at 0x3A80) | ✓ |
| 3 | DwordRead: HL = low, BC = high word | ✓ |
| 4 | Update (flush) | no-op |
| ≥5 | real driver: `ret` with A unchanged | same |
Return A = 0 on success (FS_FAT16 bank A15: `and a; jr nz error`).

**Logical sector space** (the UD driver's view, used by everything): sectors **0..511 = VM
(settings) area** (VM_NANF writes sectors 0/1 through RST 18; BREC's FW-read serves byte offsets
≥ 16 MB from the same sectors), sectors **512.. = the user FAT volume**. Evidence: DRV_ActiveDrive
(SYSBNKB slot 1, 0x4872 `ld de,0x0200`) adds 0x200 to every sector, reads base+0, accepts a boot
sector (0xEB) or an MBR (partition entry at +0x1BE), and decides FAT12/16/32 from BPB+0x11 /
"12"/"16" at +0x39 (→ 0xC0/0xC1/0xC2). `--disk folder` builds a 64 MB (`--disk-mb`) FAT16
superfloppy (4 KB clusters, VFAT long names) in RAM; `--disk img` uses a raw FAT image (without MBR
or with one).

Verified with the real drivers: MUSIC.AP lists, opens and streams files (FS_FOpen/FSeek/FRead/
FTell), shows long file names; the homebrew launcher (`homebrew-launcher-v04.fw`) lists the
`*.AP` files of `--disk work/hbdisk` (screenshot 10).

**Finding for the homebrew launcher:** starting SNAKE.AP from the list fails in the emulator
because the launcher's stub parameter block at **0x2C00** is overwritten by FS_FAT16's FS_FRead:
it keeps a cluster cache at `0x2C00 + 2*i` (FS_FAT16 bank B12 0x4921 `ld hl,0x2c00; add hl,de;
ld (hl),e ...`). After the first sector, `dest` (0x2C02) becomes 0x0205 and the next sector lands
on the kernel area. `fakefs.py` does not model this; on the device it should fail the same way.
Moving the parameter block/stub away from 0x2C00-0x2C7F (or reading through a different area)
should fix it.

**Option 2 (hardware level): now implemented as `--nand`, see "NAND model" below.** Original notes:
emulating the NAND controller so the real UD/SD drivers
and BREC's FTL run against `readback-my-ximpro.s1clone`. What is known: the dump header is
`s1clone\0`, flash ID **EC D3 51 95** (Samsung, 8 Gbit), 0x80000 pages, 64 pages/block, page
size 0x840 = 2048 + 64 spare, pages stored as (u16 checksum, u16 size, encoded page) records
(`CloneDevice.cpp` encodePage). The drivers drive the NAND through port 0x2A (hardware CMD/ADDR
sequencer: 0x70 status, 0x30/0x10/0xD0, 0x37 = stop/release CE), data via the 0x8000 window with
ports 0x01/0x02 selecting CE and (inferred) CLE/ALE regions, DMA1 from the external window, ECC in
0xCC-0xCF, and BREC's resident page-read/translation code at 0x3A39-0x3FFF (0x3D3A select CE,
0x3E30 page read via DMA, 0x3F1B/0x3F69 address/FTL lookups, tables at 0x3C80/0x3CA0). The FTL
(logical block → physical block, using the spare area) lives in BREC + UD driver code, so a
hardware model would also need the BREC init (0x1B12) to run instead of being skipped. Option 1
got MUSIC.AP working immediately, so option 2 was not started.

## NAND model (`--nand DUMP`, `emu/nand.py`) — hardware level
Spec and evidence: `lisong_rebuild/analysis/nand_ftl.md`; reference FTL reader:
`lisong_rebuild/tools/ftlread.py`; container reader: `lisong_rebuild/tools/s1clone.py`.

* **Backing store:** the s1clone dump, opened read-only (page index cached in `work/nandcache/`).
  Programs/erases go to an in-memory overlay (`--nand-overlay FILE`: loaded if present, saved at
  exit). Programming ANDs into the page (NAND semantics); erase = all FF.
* **Bus decode (soc.py `_ext_select`):** while port 0x02 bits 5-3 select CE1/2/4-7 the 0x8000-0xFFFF
  window goes to the NAND controller: port 0x01 = 1 -> CLE write, 2 -> ALE write, 0 -> data read/write.
  Only CE1 has a chip; other CEs read 0xFF (the BREC scan then reports "no flash"). CE3 (0x18) stays the
  LCD, CE0 the ROM/RAM view.
* **Ports:** 0x2A sequencer (one CLE cycle; 0x90 + one ALE; 0x37 = release, no cycle), 0x28 bit 0 never
  busy, 0xCC-0xCF hardware ECC (started by 0xCC bit 4, hashes the next 512 data bytes, result in the
  stored-spare byte format). DMA1 with SA3/DA3 bit 7 reads/writes NAND data cycles (CE = SA3 bits 4-2).
* **Chip:** reset FF, status 70 (0xC0), read ID 90 (EC D3 51 95 00), read 00-5addr-30, random data
  out 05-2addr-E0, copy-back read 00-..-35, program 80/81/85-..-10/11, two-plane erase 60-60-D0.
  One page register **per plane** (block bit 0) - required, because UD_SF641 merges units with two-plane
  copy-back (85 ... 11, 81 ... 10); a single register silently wiped the VM unit in a first version.
* **What runs for real:** BREC 0x1B12 (LCD init, flash detect -> RAM 0x0080 = `02 01 .. 0x008A=0x1EC0`,
  spare scan of both zones, temp-unit erase, table copy rewrite), the FW reader 0x3FDB (only traced:
  log lines `FWREAD ... [NAND]`), UD_SF641 RST 18h (`UDLOG=1` traces it), VM reads/writes, FS_FAT16.
  BREC is loaded by a model of the mask-ROM loader (`soc.py brom_load_brec`, BROM 0x4213/0x436E): blocks
  0-3, data of pages 0-7 through the chip model (overlays included), the first block with `0x55AA` + correct
  sum; no valid block -> the run stops with "ROM recovery". `NANDLOG=1` logs every CLE/sequencer command and program.
* **Still HLE'd in this mode:** only the RAM 0x0037 irq-nesting fix (set to 0 when SYSCFG starts; BREC
  leaves 0x76 there and SYSCFG's vector page sets 1 - the real-init run confirms it is needed) and the DSP.
* **Timing:** BREC init takes 105 M T-states (the spare scan), menu at ~300 M. Run speed ~40-55 M T/s.
* **Keys with the dump's settings:** in the main menu MENU/LEFT are ignored until PLAY has been held
  (~3 s, at 330 M); probably a saved key-lock state (inferred). Then MENU enters MUSIC.AP, which spends
  ~400 M T-states initialising (scan of the 1 GB disk) and shows the file paused at its resume point.

Run (`logs/nand_play.log`, `logs/nand_compare.txt`):
```
python headless.py --nand ..\readback-my-ximpro.s1clone --ticks 1150000000 ^
   --keys 330000000:PLAY:3000,420000000:MENU:300,850000000:PLAY:300,950000000:PLAY:300 ^
   --wav work/nand_play.wav --shots 800000000:screenshots/13_nand_music_resume.png
```
Result: 518 400 PCM frames; the WAV is found in an independent libsndfile decode of the same file
(extracted with `ftlread.py`) at 28:58.8 with SNR 66.0 dB. 273 page programs + 8 erases in the
overlay (table copies, VM settings writes by KCFG/MAIN, temp-unit cycling); the VM unit keeps its data.

## Port 0x05 window (0x4000-0x7FFF)
Device-measured with a probe app (coordinator) + ATJ2085X PG p.12-13. Implemented by swapping
8 KB halves into the flat Z80 memory on every port-05 write (code can execute from any page).

| port 05 | MCU sees |
|---|---|
| `xxxxx111` (stock **0xF7**) | ZRAM2 (UI driver 0x4000-0x47FF, OS bank-B window 0x4800-0x4FFF); 0x5000-0x7FFF kept as RAM (not measured) |
| lane 000/001/010 + bit7 / bit6 | IPM low/mid/high byte lane, low / high 8 KB half (0xC0-0xC2 = whole 16 KB page) |
| lane 100/101/110 + bit5 / bit4 | IDM low/mid/high lane, low / high half (0x30/0x34-0x36 = whole page; codec uses 0x24/0x25/0x26 = low half only, 0x34 = both) |
| anything else (0x00, lane 011, unowned half) | reads 0, writes lost |

Each lane is a separate 16 KB backing store; the same stores are the DSP model's IPM/IDM
(`soc.dsp_read/dsp_write(lane, word)`), so `InitDSPCode` really writes MP3DEC.DSP's DM/PM byte
planes there. FW-read/UD-read requests with page byte 0-6 and `dest < 0x4000` go to DSP lane
`page` at word `dest` (InitDSPCode: header to IDM-L 0x3000, read back at 0x7010 with 05 = 0xF4;
inferred from SYSBNKA bank 0x23 + BREC 0x3E30); `dest ≥ 0x4000` goes to the ZRAM2 view.
Self-test: `tools/test_paging.py` (independence of the 7 pages, 0x00 = zeros, code execution).

## DSP model (`emu/dsp.py`) — host-protocol HLE, MP3 only
The DSP core is not emulated. When the OS loads `mp3dec.dsp` (`InitDSPCode`, RST 10 HL=0x0023,
hooked) and the codec sets port 0x2E bit 7, the model plays the DSP side of the v04 MP3 codec
driver's protocol (MUSIC.AP bank 0x01 data/helpers, bank 0x02 `c_mp3decINIT`, bank 0x03 IRQ
handler 0x253A / dispatcher 0x2561, basal banks 0x21/0x23) and decodes on the PC with pymp3.
Other DSP programs (WMA, OGG, ADPCM, JPEG...) are logged and left silent.

**Which program runs** is decided when port 0x2E bit 7 rises, by fingerprinting program memory
(CRC32 of IPM lanes L|M|H over the program's PM length) against every ADEF `.DSP` file in the
firmware image plus a built-in v04 MP3DEC fingerprint. A program written into the lanes by
non-stock code (port 05 pages 0xC0-0xC2 / 0x34-0x36, as zos does) is therefore recognised; the
InitDSPCode hook is only a fallback name. The model also behaves like the hardware where a
from-scratch driver could go wrong, and logs a `DSP WARNING` once: DSP frozen while 0x2E bit 1
(clock) is 0; IPM/IDM halves owned by the MCU at start; silence while the DAC is off/muted
(0x80 bit5/bit1, 0x8B bit4); 0x81 rate or 0x42 FSSEL not matching the stream; PA off (0xDD);
bitstream DMA differing from the stock descriptor or without a pending 0xEE. The 0xCC rate code
carries bit 7 for the 44.1 kHz family (0x88/0x8A/0x8B), as the codec's PLL switch expects.
Full Z80-side protocol: `lisong_rebuild/analysis/mp3_driver_spec.md`.

Mechanics: request = DSP writes the code to port 0x35 (DSP→MCU latch) and sets MINT_STATUS bit 0
(enabled by port 0x27 bit 0). The handler reads 0x35 twice, acks with `out (0x26),1`, dispatches,
and replies by writing 0x35. Data: ports 0x30-0x34 both ways (separate latches), IDM low lane
through port 05 = 0x24/0x34, DMA1 for the bitstream.

| request | DSP provides | MCU does | reply |
|---|---|---|---|
| 0x11 | — | port 0x34 ← c_mp3deSecondRestFlag (0x2400): **0 play, 1 resume, 0xFF duration scan** (MC_LOAD); with 1 also 3 bytes → IDM 0x3E08 | 0x22 |
| 0xEE | 0x31 = frame-sync status (≠0; 100× 0 = "not an MP3"), **0x32/0x33 = signed sector step for the next read (1 = sequential)**, 0x34 = 0x11 forward / 0x22 backward | first time: file length → IDM word 0x1C03 (L,M,H lanes) + 0x1C04, volume (port 0x9E) → 0x1C07, EQ/AB params; DMA 512 B from the 0x1DCD buffer to IDM 0x1CB3 (desc. at 0x2B47: `00 00 00 B3 1C 00 40 06 FF 01 30`); port 0x30 = 0 data / 1,2 FF-REW markers / **3 no more data**, 0x33 = (0x2448); then refills the buffer with hook_read (bank 0x21 0x1C00: FS_FTell, optional FS_FSeek(step), FS_FRead) | 0xDD |
| 0xCC | 0x30 mode, 0x31-0x33 bitrate as 3 ASCII digits, 0x34 DAC rate code (written to port 0x81: 8 = 44.1k, 0xA = 22.05k, … PG p.51); IDM-L 0x1C01 = total sec, 0x1C02 = min, 0x1D52 = hour | c_mp3deSongTimeLength, bitrate, mode; sets the DAC rate. **Scan mode: sets DecodeInfor = 0xFE, no reply, resets the DSP** | 0x99 |
| 0xBB | **same 0x30-0x34 frame info again** (the handler re-reads them via 0x283B→0x285A); IDM-L 0x1D57 hour, 0x1D58 min, 0x1D59 sec, 0x1D5A/B ms = current play time → c_mp3deNowTime* (MC_GETTIME) | writes EQ (0x5D3E, 11 B), AB, 0x5C07, 0x5D60, … | 0x00 |
| 0xAA | — | DecodeInfor = 0xFE (end of song); basal then stops the DSP and the player moves on | none |
| 0x55 / 0xA8 / 0xA9 / 0x77 / 0x2B / 0xE0 | (not used by the model) | sentence point / chip check (port 0x2F) / verdict / MIPS up / overlay load / exit ack | 0x66 / 0x95 / 0x96 / 0x88 / 0x3B / none |
The codec also runs a watchdog (0x2A3B, every 20 ms): if the DSP is silent for 20-40 ticks it sets
port 0x37 bit 6 and eventually DecodeInfor = 0xFA ("reset DSP"); the model keeps requests flowing.
v04 RAM map (bank 0x01 data at 0x2400) matches the 913f SDK `mp3decC.h` order: 0x2420-22
SongTimeLength [s,m,h], 0x2423-27 NowTime h,m,s,ms, 0x2428 FileType, 0x242B-2D BitRate ASCII,
0x242E SampleRateIndex, 0x242F Mode, 0x2430 BitStreamTimes, 0x2431 DecodeInfor (F1/FE/FA/EE),
0x2432 HipIntType, 0x2465/67/69 hook_read/hook_Buffer/volume, 0x2407-0A FileLength.

Model behaviour: 0x11 → EE until the first frame is confirmed → CC (total time from Xing frames,
else (file length − ID3v2) × 8 / bitrate) → in play mode EE whenever < 8 KB of undecoded bitstream
is buffered and the file is not complete, BB every 0.25 s of audio, AA when the decoder has
drained. The DSP stops asking once it has received the file length (the driver would keep
DMAing padded sectors). Pacing: WAV/headless = emulated clock (12 MHz assumption → real time);
`--audio` = sound-card clock (decodes while < 0.35 s is queued). MPEG-1/2/2.5 layer III only.

**DMA1** (ports 0x06-0x13): SA/DA bit 7 external (NAND data cycles with `--nand`, else 0xFF), bit 6 internal DSP lane
(ISA/IDA), else ZRAM (Z80 address); mode bit 5 "DSP mode" cycles L/M/H lanes per word (H→L
when the down-count bit is set — order inferred), bits 3/4 down-count. Transfer is instantaneous,
port 0x13 bit 0 clears, 0x25 bit 3 set.

## Tests (all headless, reproducible)
| test | result |
|---|---|
| MUSIC with `--disk testdisk` (2 files) | scan pass gives 0:00:08 and 0:00:12; PLAY plays track 1, auto-advances to track 2, stops after it; clock, progress bar, "128KBPS", ID3 title update (screenshots 05-09) |
| WAV vs independent decode (`tools/compare_wav.py`, libsndfile/mpg123) | sweep (22.05 kHz mono → 44.1 k): found at 0.000 s, SNR **85.4 dB**; tone (44.1 kHz stereo): found at 8.046 s, SNR **82.0 dB** (`logs/compare_wav.txt`). The emulator WAV is also bit-identical to an offline pymp3 decode through the same resampler. |
| playback timing | track 1 decoded 8.05 s of audio in 96.5 M T-states = 8.05 s at 12 MHz |
| live audio (`--audio`, GUI path) | sounddevice consumed 3.58 s of 22.05 kHz audio in 4 s wall time (audible on the PC) |
| homebrew launcher on the real FS | lists KEYTEST.AP / SNAKE.AP; launching fails (stub at 0x2C00, see above) |
| USB plug (`USB+`) | MAIN starts USBDISK.AP (message 0x91), which returns |
| `tools/test_paging.py`, `tools/gui_smoke.py` | pass (also after the NAND model was added) |
| `--nand readback-my-ximpro.s1clone` (real BREC init + UD driver) | menu with the device's settings, MUSIC.AP lists the dump's root MP3, playback matches an independent decode of the extracted file at 66.0 dB (screenshots 12-14, `logs/nand_compare.txt`) |
| `tools/test_dsp_loader.py` (non-stock loader + port-level host driver in Python, no InitDSPCode) | MP3DEC recognised by fingerprint, corrupted PM rejected; scan gives 0:00:08 / 0:00:12; play PCM identical to a direct pymp3 decode; no model warnings |

## Stubbed / not emulated
- **DSP:** only MP3DEC.DSP has a model (HLE, no DSP ISA). WMA/OGG/ADPCM/recording/JPEG/video
  programs load but the DSP stays silent. FF/REW (port 0x30 = 1/2, step ≠ 1), A-B repeat, EQ,
  volume (0x5C07 / port 0x9E), resume time (IDM 0x3E08) and the 0xA8/A9 chip check are not
  acted on by the model. The DAC ports are latches; the PCM goes straight from the model to the
  PC.
- **NAND hardware / FTL:** emulated with `--nand` (see "NAND model"); without it the driver-level HLE
  (`--disk`) and the BREC-init skip are used as before. Not modelled: NAND timing/busy, the 0x2B-0x2D/0xEC
  sequencer address registers (unused by this firmware), program/erase failures, bad-block growth.
- **USB (host side), I2C (FM tuner), UART, ADC (battery):** latches only.
- **DMA2, CTC1:** not modelled.
- **RTC time:** stays 0.
- **CPU clock:** assumed 12 MHz; scales RTC ticks, key-press duration and (WAV mode) audio pacing.
- 0x5000-0x7FFF under port-05 lane 7 is plain RAM here (only 0x4000-0x4FFF is ZRAM2 on the chip).

## Next steps
1. (done: `--nand`.) Open: DSP resume time, key-lock behaviour with the dump's settings, USB mode
   (USBDISK.AP) against the NAND model.
2. Model the other codec drivers (WMA bank 0x04-0x06, OGG 0x1A-0x1C, ADPCM 0x09-0x0B) on the same
   mailbox/DMA framework; FF/REW and resume for MP3.
3. 0x0037 interrupt counter: with the real BREC init it is 0x76 (BREC filler) before SYSCFG and SYSCFG's
   vector page sets 1, so the fix is still needed; how the device gets it to 0 remains open.
4. Mask ROM: the s1clone header's 16 KB BROM image matches `rom/comlib.s28` at 0x8800-0x8E77 in 1644 of
   1652 bytes (8 bytes differ, not analysed) - the C-runtime assumption is essentially confirmed.
