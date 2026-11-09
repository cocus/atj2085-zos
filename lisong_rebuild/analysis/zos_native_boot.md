# zos native boot: BREC starts zos directly (stock firmware kept, MENU = stock)

Build: `python lisong_rebuild\homebrew\zkern\build.py [--build-zos]` (PowerShell) ->
`s1mp3\build_v04\zos-native-v04.fw` (flash this) and `s1mp3\build_v04\zos-native-control-v04.fw`
(control: the base `my-ximpro.fw` through the same packer, byte-identical to it, md5 3f7a8d29...).
Sources: `lisong_rebuild/homebrew/zkern/{build.py, stub.s, zkregs.inc, emu_native.py}`,
`lisong_rebuild/tools/fwinject.py`. **No zos source file was changed**: the zos core image
(`ZOS.AP` text, linked at 0x0800, built with `zos\build.py --no-deploy`) is used as it is.
BREC, BROM and all other AFI parts are untouched; nothing here writes the device.

## 1. Layout

FWIMAGE = the stock one with **one file replaced in place and one file appended**. Every other
stock file is byte-identical at its original sector (build.py cross-checks this).

| entry | what | where |
|---|---|---|
| `SYSCFG  SYS` (0x3800 B, same sectors as stock) | file 0x0000-0x01FF: stage-0 stub (`stub.s`) -> RAM 0x0600; file 0x0200..: zos core image -> RAM 0x0800 (8845 B now, max 0x2400 B: ends < 0x2C00); file 0x3700-0x377F: vector page -> RAM 0x0000 | BREC 0x0216 loads file 0x0000-0x2CFF -> 0x0600-0x32FF and file 0x3700-0x377F -> 0x0000-0x007F, then `ld sp,0x0400 ; jp 0x0600` |
| `SYSCFG  STK` (appended, sector 12460) | the stock kernel, byte-identical to the stock `SYSCFG.SYS` | chain-loaded by BREC's own loader when stock is wanted |

Checksums: per-file u32 sums (dir +0x1C), directory sum @0x10, header sum16 @0x1FE, AFI entry
sum and AFI header sum @0xFFC are recomputed and re-verified (`build.py --check FW`). `.STK` is a
BREC-verified extension, so BREC also checks the stock kernel copy on every boot (+28 sectors,
+0.47 M T-states). FWIMAGE grows 6 379 520 -> 6 393 856 B (copy B starts at 8 MB: fits).
Nothing else in the firmware references `SYSCFG.SYS` by name (all files grepped; the emulator
shows the stock kernel never reads its own file after BREC has loaded it).

## 2. Stage-0 stub (0x0600-0x07FF, 292 B used)

Entered by BREC with DI, SP = 0x0400, port 05 = 0xF7, LCD initialised by BREC (hourglass,
backlight on), RAM 0x008A / 0x3C80 (flash size, FTL table descriptors) and BREC's resident
flash routines 0x3CAF-0x3FFF valid.

1. Saves BC/DE (BREC leftovers, no push: SP sits on BREC code), SP = 0x0200 (BREC's own stack).
2. **MENU held?** 16 samples ~1.6 ms apart of GPIO B2 (phase-1 key scan as zos `keys_read`:
   OE &= ~0x66, IE |= 0x66, read F2 & 0x04), >= 12 pressed = held. OE/IE restored afterwards.
3. **zos valid?** length 1..0x2400, `jp` at 0x0800, 16-bit byte sum = value stored by build.py.
4. MENU held or zos invalid -> **chain-load stock**: check that BREC 0x0274.. still holds the 11
   `cp 'S','Y','S','C','F','G',' ',' ','S','Y','S'` instructions (else start zos: unknown BREC or
   a previous chain attempt already failed - no loop), patch the 3 extension bytes' compares to
   `STK` (all 11 written), restore BC/DE, SP = 0x0200, `jp 0x020C` = BREC's own
   `call 0x0216 (load) ; ld sp,0x0400 ; jp 0x0600`. The stock kernel is then loaded and entered
   by the identical code path as on a stock boot; its first instructions (0x0600-0x061D) set SP,
   fill 0x0200-0x03FF with 0x76 and clear 0x0100-0x01FF, i.e. they erase the patched loader and
   the stub's stack area. BREC's loader ignores its result: if `SYSCFG STK` were missing it would
   re-enter the stub, which then sees "STK" in BREC and starts zos.
5. Otherwise **start zos** (takeover start, no stock code ever ran), doing what the stock code did
   before zos ran in hosted mode (values from the stock code, port trace kernel->main menu):
   `im 1`; RTC divider 0x4B/0x4D/0x4C = A4/00/01 and RTC_CTRL |= 0x60 (10 ms + 2 Hz timers,
   SYSCFG 0x096A-0x097B); watchdog period (0x4E & 0x8F) | 0x30 + clear (MAIN.AP; watchdog
   enable bit stays as BREC left it, off); DC-DC 0x4F = 0xA5 (MAIN.AP); GPIO B/G as KY_HARD.DRV
   (EF=0, C1=0, F2=0, F0=0, F1=0x77, FE=0x10); I2C pins C0/C1 back to inputs (F3 07 -> 04, the
   stock main-menu state); SP = 0x0800 (zos stack 0x0100-0x07FF, below it BREC data 0x0080-0x00FF
   incl. 0x008A); `jp 0x0800`. No LCD reset, no clock/PLL change (stock does none before its
   main menu either), port 05 stays 0xF7.
6. Vector page (RAM 0x0000-0x007F): 0x0000 and RST 08..30 = watchdog reboot, 0x0038 = `ei ; reti`
   until zos hooks it, 0x0066 = `retn`. RAM 0x0037 stays 0x76: BREC's flash wrapper
   (0x3D3A/0x3D74) counts nesting there and does EI when it reaches 0; with 0x76 the chain-load
   keeps interrupts off exactly as during the original load.

Port state at native zos entry vs the stock main menu (where hosted zos used to start), emulator:
equal except DMA1 address leftovers (06/0B/0C, zos programs all DMA1 registers), ECC result
registers (CC-CF), 0x27 (zos sets its own IRQ enables) and pending RTC flags (0x44, acked by the
zos ISR). The only LCD-init difference BREC vs stock UI driver is the contrast byte (BREC `81 21 07`,
UI driver `81 38 06`); zos SHELL sets its contrast from ZOS.CFG at start-up anyway.

Leaving zos (SHELL REBOOT, crash -> watchdog) = chip reset -> BROM -> BREC -> stub -> zos again.
**The way to stock is holding MENU at power-on / reset** (until the stock boot animation shows).
Modules, ZOS.CFG, SKIN, MP3DEC.DSP stay on the FAT user disk; zos's own FAT loads SHELL.ZM etc.,
so module updates keep working by USB copy (from stock: boot with MENU, plug USB as usual).
ZOS.AP on the disk is not used by the native boot (only by the JUEGOS launcher path, unchanged).

## 3. Emulator proof

Runner: `homebrew/zkern/emu_native.py FW OUTDIR --overlay OVL [--keys] [--shots] [--wav]`
(milestones at BREC `jp 0x0600`, stub entry, `go_zos`/`go_stock`, zos/kernel entry, first LCD
pixel write; 100 k T-state granularity). Overlays: `tools/nandinject.py` (the zos out files
*.ZM, ZOS.CFG, MP3DEC.DSP, SKIN\, plus atj2085emu/testdisk/TONE440.MP3 into the dump's FAT disk)
+ `tools/fwinject.py DUMP OUT.ovl FW --copies A|AB --merge disk.ovl` (FWIMAGE into the firmware
area through the FTL map; only changed sectors, spare ECC recomputed). Real BREC from the
`readback-my-ximpro.s1clone` dump. Outputs: `s1mp3\build_v04\zkern_emu\{a_native,b_menu,c_control,
d_firstboot,e_badzos}\` (run.log, milestones.json, PNGs, music.wav).

T-states from BREC entry (0x0200); seconds at 24.576 MHz (boot.md column A):

| run | image / keys | result | key milestones (M T) |
|---|---|---|---|
| (a) | native, copies A=B | **zos SHELL** (carousel "MUSIC") complete by 112 M; PLAY -> MUSIC list, RIGHT x5 -> TONE440.MP3, PLAY -> MP3PLAY "NOW PLAYING 00:02 / 00:12 128KBPS"; DSP: 1 start, 129 KB bitstream, 330 624 samples, WAV peak at 441 Hz (the tone) | stub 106.08, go_zos 107.22, zos entry 107.22 (4.36 s), **first zos pixel 109.2 (4.44 s)** |
| (b) | native, MENU held 0-9.5 s | stub -> go_stock -> BREC loads `SYSCFG.STK` -> stock kernel -> boot animation -> **stock main menu "MUSICA"** (same as control) | go_stock 106.71, kernel entry 107.06 (control 105.61), first stock pixel 136.0 |
| (c) | control (byte-identical to my-ximpro.fw) | stock main menu | kernel entry 105.61 (4.30 s), **first stock pixel 134.6 (5.48 s)** (UI-driver LCD re-init), main menu between 200 and 250 M (8.1-10.2 s) |
| (d) | native, only copy A written (= state right after an s1res flash) | BREC finds copy B different and copies A -> B (16.6 k page programs, ~194 M T = ~7.9 s, one time), then zos SHELL | zos entry 301.4, first pixel 303.4 |
| (e) | TEST-ONLY image with wrong zos sum (BREC checksums valid) | stub -> go_stock -> stock main menu | go_stock 107.22, kernel entry 107.57 |

In (a) the hourglass stays on screen until zos draws: no dark gap and no 26 M T-state UI-driver
LCD re-init; zos is fully up ~0.1 s after BREC finishes, the stock firmware reaches its menu
3.5-6 s later. BREC itself (LCD init + mount + checksum, ~106 M T = 4.3 s) is unchanged;
making it shorter needs a leaner FWIMAGE (fewer verified files), which this design deliberately
does not do (all stock files must stay for the s1res/ADFU path).

Emulator limits (not proven here, check on the device): no watchdog reset model (reboot path),
DSP is HLE (no clock/state checks), RTC ticks regardless of the divider set-up, no NAND timing.

## 4. Flashing and recovery

What s1res writes (`s1mp3_github/s1res/DeviceIO.cpp`, `common/FirmwareIO.cpp`): in the normal
(non-GiveIO) mode it switches the running stock firmware to firmware-ADFU over MSC (SCSI 0xCC/0xCB,
usb_msc.md 4.1) and writes **only the FWIMAGE** with ADFU command 0x08 from firmware sector 0
(copy A), after checking magic, header sum16 and directory sum32. **No BREC write**
(`write(NULL, 0, ...)`). Copy B (byte 0x800000) is left alone; BREC copies A -> B on the next
boot (run (d)). Only the GiveIO path (`fio`) also writes the BREC (byte-identical here anyway) -
do not use GiveIO.

Flashing:
1. Keep `my-ximpro.fw` and `readback-my-ximpro.s1clone` (they are the way back).
2. Player in stock firmware, USB connected -> s1res -> write firmware -> `zos-native-v04.fw`
   (normal ADFU mode, no GiveIO).
3. Copy the zos files to the disk root before (or after, via MENU boot): `*.ZM`, `ZOS.CFG`,
   `MP3DEC.DSP`, `SKIN\` (snapshot in `build_v04\zkern\disk\` + `zos\out\atj2085\ZOS.CFG`).
   Without SHELL.ZM zos shows its built-in module list.
4. The first boot after flashing takes ~8 s longer (BREC copies A -> B, hourglass shown).

Recovery ladder:
* **MENU held at power-on** (until the stock animation appears; the stub samples ~25 ms right
  after the hourglass phase, ~4.3 s after power-on) -> unmodified stock firmware -> USB disk,
  JUEGOS launcher, and s1res work exactly as before -> flash `my-ximpro.fw` (or the control image,
  which is the same bytes) to go back to stock permanently.
* zos image damaged -> the stub boots stock by itself.
* If neither zos nor the stock kernel runs but BREC does (hourglass shows): whether BREC falls
  back to copy B for a bad copy A is not analysed here; the last resort is ROM recovery (only when
  BREC is unloadable) + s1clone with the dump - unchanged by this firmware, since BREC/NAND
  blocks 0/1 are never written by s1res in normal mode.
* Risk to know: if zos hangs early without the watchdog (watchdog enable stays as BREC left it,
  off - same as hosted mode), only a power/reset cycle helps; then hold MENU.
