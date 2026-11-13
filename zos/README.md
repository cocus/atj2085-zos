# zos: a small, fast OS for the Ximpro / LISONG LSD_501 (ATJ2085) player

Fresh design, not a re-engineering of the stock ACTOS: single address space, own drivers at register
level, event loop instead of an RTOS, 16 KB RAM pages instead of IAR banking. Portable core
(`core/`, `ui/`, `apps/`) behind a HAL (`hal/hal.h`); everything chip/board-specific lives in
`targets/<target>/`.


Firmware location, NAND partition map and the boot process: [NAND_AND_BOOT.md](NAND_AND_BOOT.md).

## Run it (hosted mode, today)
```
python zos/build.py                      # -> zos/out/atj2085/ZOS.AP
```
Copy `ZOS.AP` and the modules (`*.ZM`) to the root of the player's USB disk â†’ JUEGOS (the homebrew launcher) â†’ `ZOS.AP` â†’ PLAY.
`EXIT TO STOCK` returns to the stock main menu with the machine restored. A crash or hang is harmless:
the watchdog (kicked only from the main loop) reboots into the untouched stock firmware.

Emulator: `lisong_rebuild/tools/emurun.py build_v04/homebrew-launcher-v04.fw OUT "fakefs:DIR_WITH_ZOS_AP boot:320 key:NEXT key:PLAY:150 wait:40 key:K06 ..."`
(the emulator's old button labels: K06 = PLAY, PLAY = MENU, NEXT = LEFT, PREV = RIGHT).

## Hardware registers
Every I/O port of the chip is declared **once**, in `targets/<target>/regs.h` (the `P_xxx` port
number and the bit/value defines) and `targets/<target>/sfr.h` (the C register variable, same name
without the `P_`: `P_PAVOL` -> `PAVOL`, `P_USB_STAT` -> `USB_STAT`, `P_DMA2_SA0` -> `DMA2_SA0`).
- A module/core file just does `#include "regs.h"` and writes `PAVOL = 20; if (USB_STAT & USBST_VBUS) ...`.
  **Never** declare `SFR(...)` / `__sfr` in a module: `tools/check_sfr.py` (run first by `build.py`)
  fails the build when it finds one outside `regs.h`/`sfr.h` (the retired `attic/` is skipped).
- A port that is missing is added in `regs.h` (`#define P_xxx 0x..`, plus its bit defines) and as one
  `SFR(xxx, P_xxx);` line in `sfr.h`, with the access hazards (write-only, W1C, never-write bits)
  in the comment next to it. Unused declarations cost no code.
- Naming rule: register = port define minus `P_`; bit/field constants are `XXX_yyy` and never equal a register
  name. Module-local aliases only where they carry meaning (e.g. PERFTEST `D_SA0` = DMA1 or DMA2 bank).
- Variable-port access (a port number in a variable, e.g. the CDC console peek/poke) uses `port_in()/port_out()`
  with the `P_xxx` numbers; assembly files use the `P_xxx` names from the generated `out/<target>/regs.inc`
  (made from the `#define`s of `regs.h` only; `sfr.h` is C-only and ignored).
- `ZOS_BUILD_TIME=YYYY-MM-DDTHH:MM` pins the build time stamped into `buildtime.h`, so two builds of the same
  sources are byte-identical (used to verify pure refactors by comparing the md5 of every output).

## Design
### Memory (ATJ2085, Z80 64 KB)
| range | use |
|---|---|
| 0000-00FF | vectors. Hosted: zos patches `JP zos_isr` at 0x0038 (IM1) and restores it on exit |
| 0100-3FFF | ZRAM1: kernel, drivers, current app, stack. Hosted image: 0x0800-0x2BFF |
| 4000-7FFF | paged window over on-chip RAM (port 0x05): 6 DSP RAM byte lanes Ã— 16 KB + ZRAM2. Planned: modules run in place from pages, switch = one OUT |
| 8000-FFFF | external bus: zos keeps CE3 (LCD) selected for the whole session |

### Kernel
* **Interrupts** (`targets/atj2085/isr.s`): own IM1 handler; only the RTC source is enabled (port
  0x27 = 0x20): 10 ms timer â†’ tick + key scan every 20 ms, 2 Hz â†’ `EV_SECOND_HALF`.
* **Wall clock** (`targets/atj2085/clock.s`, `hal/hal.h` HAL_CLK): the same 2 Hz interrupt keeps
  hh:mm:ss + a day count in a 17-byte block in HAL_SHARED and posts `EV_TIME` when the minute changes;
  SHELL starts it (DS3231 > `ZOS.CFG` anchor > build time), SETTINGS sets it. See [../docs/clock.md](../docs/clock.md).
* **Events** (`core/event.c`): ISR-safe ring buffer; `ev_wait(&e, timeout)` sleeps with `HALT`
  between interrupts and kicks the watchdog.
* **Keys**: GPIO B scanned directly (board wiring from the stock key driver): MENU, LEFT, RIGHT,
  PLAY â†’ `EV_KEY_DOWN/REPEAT/UP` (repeat after 500 ms, every 100 ms).
* **Power button**: PLAY held 2 s â†’ `EV_POWER` (rest of that press swallowed). Like the stock
  firmware, "off" is a standby, not a power cut (no power-hold pin): SHELL saves ZOS.CFG and runs the
  stock STANDBY.AP loop (`modules/shell/standby.s`: display off, CPU on 32 kHz, HOSC/PLLs/watchdog
  off); PLAY held 1 s (or a PC on USB â†’ USB module) wakes it and zos carries on. MP3PLAY, MUSIC,
  FM and SETTINGS leave on `EV_POWER` with `MOD_POWER_REQ` set (resume point kept); other modules
  ignore it. Research: `lisong_rebuild/analysis/power.md` section 6.
* **Display**: the LCD's RAM is the framebuffer. zos sets the controller's scan mode to 0x03 so
  screen (x,y) = window (x, y+4), streamed leftâ†’right, topâ†’bottom; fills are unrolled asm, glyphs are
  streamed by an asm routine (one window per glyph). The stock mode (0x02) is restored on exit.

### Modules
* `modules/<name>/*.c` â†’ `out/<target>/<NAME>.ZM`, linked at 0x4000 with `targets/<t>/mod_crt0.s`
  (header: "ZM", version, API count, size, end, init, main, name[16]) and the generated `api.s`.
* Core API = `core/api.txt` â†’ jump table at 0x0803 (one `jp` per entry; generated `jtab.s` is linked
  right after the entry `jp`). Modules call the absolute table addresses, so they keep working across
  core rebuilds. **Append only.** Calling convention: SDCC `__sdcccall(1)`, IY reserved.
* `mod_load(name, page, &hdr)` reads the file into a page (`hal_pages[]` = C1 C2 34 35 36; C0 stays
  unused until write-tested); `mod_run` calls init (data/bss), then `mod_main()` with the page mapped.
  MODULES on the home screen lists `*.ZM` and runs the chosen one in page 0xC1.
* Modules may call storage: names and sectors are bounced through ZRAM1, because the bridge maps
  the stock page into the window for the duration of the call.
* Hosted constraint: zos must not use 0x2C00-0x2C7F, which is FS_FAT16's cluster cache.

### Sound engine (modules/common/snd.c)
A non-blocking sound engine for page modules that must make sound while they run (QUADFALL: music + effects).
The DAC has no DMA and no interrupt: at 8 kHz it takes one 16-bit word per channel from an 8-word FIFO
(`DACFIFO_FULL`), so 4 stereo frames = **0.5 ms of audio** (12 288 T-states at the real 24.576 MHz) is all
the buffer there is, and the CPU refills it. `snd_pump()` writes words while the FIFO is not full (at most 12
per call), then does the 10 ms bookkeeping (tune sequencer, effects, amplifier volume) and fills again.
* **Synthesis**: three phase-accumulator pulse voices summed to one mono sample (written to L and R as in
  `beep.c`): melody (25 % pulse), bass (50 % square), one effect voice (50 %, mixed over the music, slides
  and arpeggios from a table). The sample loop is hand-written asm (`sn_synth`, ~380 T-states per sample incl.
  the call = 12 % of the CPU while a tune plays; SDCC's version took ~1000). Amplitudes slew every 4 ms
  (16 ms ramps): notes start and stop without clicks; when everything is silent the output is exactly 0 and
  nothing is written (the CPU can HALT).
* **Time**: tempo and effects run on `hal_ticks()` (10 ms), sixteenth = `unit * 16 / (16 + level)`, so the
  speed-up follows the game level and the tempo does not depend on how often the pump runs. Both voices share
  one sixteenth clock (no drift when the tempo changes).
* **Amplifier** (same sequence as `beep.c`): powered up once, silently; volume from `ZCFG->vol` (x 3/4: square
  waves are loud), set when sound starts, ramped to 0 after 1.5 s of silence and by `snd_exit()` (1 step per
  tick, like MP3PLAY's `mp3_off`). The amplifier stays on at volume 0 - the state `beep.c` and MP3PLAY expect.
* **Rules for a module**: include `snd.c` from one source (next to `skin.c`), `snd.h` elsewhere; do not link
  `beep.c` as well (define `beep()` as `snd_key()` if a shared helper such as `game.c` needs it; `snd_key()`
  honours `ZF_BEEP`); `snd_init()` once, `snd_exit()` before `mod_main` returns (also on `EV_POWER`);
  **call `snd_pump()` often**: the main loop polls (`snd_pump()` + `skin_get()`) and only `HALT`s while
  `!snd_busy()`; every drawing piece is at most ~0.3 ms (a 5x5 cell is one window + 25-pixel blit = 2 600
  T-states) with a pump between (QUADFALL: `cell()`, `fillp()`, `txt()`; `snd_wait()` for delays).
  A late pump means a short dropout (the DAC repeats or plays zero), never garbage. Content: a tune is two
  lists of `{note, sixteenths}` byte pairs, an effect `{amp, {note, ticks, slide}...}` (see `snd.h`);
  `tools/gen_quadfall_tunes.py` writes `modules/quadfall/tunes.c` and `modules/common/sndtab.h` from note names.
* **Verification** (emulator, `atj2085emu/emu/dac.py` = optional DAC FIFO model: 8 words, 1 word pair per
  8 kHz period, `FULL` flag, underruns marked; `tools/emu_game.py` runs a scripted session, `tools/snd_analyze.py`
  and `tools/snd_sfx_check.py` compare the captured samples with the score, `tools/quadfall_logic_test.py`
  checks the game rules). The emulator does not count the LCD bus wait states (3 per store at 24.576 MHz):
  the analysis adds them. Remaining dropouts: the once-a-minute `skin_time()` redraw (6 ms) and the full-screen
  clears between screens (music is silent then).

### Takeover mode (now) vs bare metal
Takeover (now): started by the homebrew launcher from the user disk, then zos needs **nothing** from
the stock OS: own IRQ handler, timer, keys, LCD, NAND + FTL (`targets/atj2085/nand.s`, `blk.c`,
read only, spec `lisong_rebuild/analysis/nand_ftl.md`) and FAT. zos therefore owns ZRAM1 above the
stock stack: the loaded image must end below 0x2C00 (launcher limit), its variables may run up to
0x3A00 (over the stock kernel/UD driver), 0x3A00-0x3BFF is scratch for modules (PERFTEST's DMA
buffer). Stock data read at start-up: RAM 0x008A (flash size) and 0x3C80 (FTL table descriptors,
verified by signature, else a spare-area scan). Leaving zos = **watchdog reset** (176 ms) into the
untouched stock firmware: nothing is ever written to flash.
Bare metal (later): same kernel/apps, started without the stock OS.

Emulator on the real NAND dump: `lisong_rebuild/tools/nandinject.py DUMP OUT.ovl FILES...` puts files
(e.g. the .ZM modules) into the dump's user disk as a NAND overlay; `zos/tools/emu_nand_zos.py OUTDIR
KEYS SHOTS --overlay OUT.ovl` boots the stock firmware from the dump and injects ZOS.AP.
Verified: module list + FILES from NAND, /S1FWX/END.RES (266 752 bytes, 17 clusters) read with
the same checksum as `ftlread.py get`, ~42 KB/s in the emulator.

## Milestones
1. âœ… **Core** (this version, emulator-verified): own ISR, 10 ms tick, keys, event loop, LCD driver,
   fast fills/text, home screen, SNAKE, LCD SPEED TEST, SYSTEM, clean exit to stock.
2. âœ… **Modules** (emulator-verified with the real FS driver): jump-table API, `.ZM` modules run in
   place from DSP RAM pages, storage bridge to the stock FAT driver, MODULES browser. SNAKE and HELLO
   are modules now.
   **Own FAT** (`core/fat.c`, emulator-verified on FAT16 and FAT32 images: long names, folders,
   multi-cluster files, checksums match the host): zos no longer uses the stock FS driver. The
   only stock call left is the UD driver's raw sector read (`targets/atj2085/blk.c`, RST 18h).
   FILES (module) browses the disk and reads a file to check it; SYSTEM and LCD SPEED TEST are
   modules now; MEMORY PROBE is retired to `attic/` (results are in the memory table above).
   Next: own NAND + FTL driver (spec: `lisong_rebuild/analysis/nand_ftl.md`). Then zos needs nothing
   from the stock OS while it runs, so it can take over all RAM and exit by reset ("takeover mode"):
   needed for the player, because MP3DEC occupies most of the DSP RAM lanes the modules use now.
3. Audio: own DSP loader + MP3/WMA player on the reversed mailbox protocol
   (`lisong_rebuild/analysis/dsp_interface.md`), voice record/playback.
4. Settings, USB mass storage (own USB stack), NAND/FTL, bare-metal boot.
   USB (modules `modules/usb` and `modules/usbaudio`, emulator-verified with a host model, not yet
   on the device): polled full-speed device with read-only mass storage, CDC-ACM debug console,
   and USB Audio Class 1 capture from the microphone or FM radio (32 kHz mono PCM). MSC/CDC and
   audio descriptors/control requests are emulator-tested; isochronous streaming still needs an
   on-device check. Design and checklist:
   `lisong_rebuild/analysis/zos_usb.md`; regression test: `tools/emu_usb_test.py`.

Measured in the emulator (assumed 12 MHz, not the real clock): full-screen fill â‰ˆ 24 ms,
glyph â‰ˆ 0.5 ms. Real numbers: run LCD SPEED TEST on the device.

## Testing storage in the emulator
* Folder disk (FAT16, built in RAM): `--disk lisong_rebuild/work/zosdisk`
* FAT32 image: `lisong_rebuild/tools/mkfatimg.py SRC_DIR OUT.img --mb 300 --fat 32` (pyfatfs, in the
  venv), then `--disk OUT.img`.
