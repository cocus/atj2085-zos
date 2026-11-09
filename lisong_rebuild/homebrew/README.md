# Homebrew apps for the Ximpro / LISONG LSD_501 player (ATJ2085, ACTOS 3.x)

Write an app in C (SDCC), build it into a `.AP`, drop it into the firmware in place of a stock
app (default `GAME.AP` = "JUEGOS" in the main menu), test it in the emulator, flash it.

```
python homebrew/build.py snake --fw        # -> build_v04/homebrew-snake-v04.fw
python tools/emurun.py build_v04/homebrew-snake-v04.fw work/try "boot:320 key:NEXT key:PLAY:150 wait:40 png:title"
```
(`LISONG_PROFILE` = `v04` (default here, your device's build) or `v05`; `--as SETTING.AP` replaces
another app instead.)

## Launcher: run apps from the user disk (flash once)
Flash `launcher` as GAME.AP once (`python homebrew/build.py launcher --fw`, write with s1res).
Then the loop is: `python homebrew/build.py snake` → copy `homebrew/out/snake/SNAKE.AP` to the
**root** of the player's USB disk → unplug → JUEGOS → `<<`/`>>` → PLAY. MENU on the list = main menu.

How it works: mounts the user disk (`DRV_ActiveUD(0,1)`, `DRV_ActiveDrive('C',1)`, `FS_Init`,
`FS_CD(":")` — the sequence of the stock READER.AP), lists `*.AP` with `FS_Dir`, validates the AP
header (homebrew layout only: one segment at file 0x800 → 0x0800, no banks/data, ≤ 0x2C00), reads
the **whole image into DSP RAM page 0xC1** (window 0x4000, port 0x05) while the launcher is still
intact, and closes the file. A stub in that page (`launcher/stub.s`, at 0x7F00, parameters at
0x7EF0) copies the image to 0x0800 (over the launcher) and restores the stack and BC/DE/IX/IY the
OS gave the launcher (saved by `crt0`). It leaves through a 4-byte trampoline written below the OS
stack (`OUT (5),A` back to the OS page, `EI`, `RET` into the app entry), so the app returns
**directly to the OS**. No OS call runs after the launcher has been overwritten.
Errors are shown on screen (no disk, no apps, not an AP, too large, read error).
v1 kept the stub and its parameters at 0x2C00, which FS_FAT16 also uses as a cluster cache, so it
only worked while the app fit in one cluster. This was found with the real FS driver in atj2085emu.
Emulator test with the real FS driver on a disk built from a folder:
`atj2085emu/headless.py build_v04/homebrew-launcher-v04.fw --disk FOLDER --keys ... --shots ...`.

## Apps
| app | what |
|---|---|
| `launcher/` | the loader above (flash it as GAME.AP) |
| `snake/` | Snake. `<<`/`>>` turn, MENU start/pause, **hold MENU** = back to the main menu |
| `keytest/` | Debug tool: prints every OS message (key codes, system messages) in hex. Hold a key to exit. Use it on the device to name the two unconfirmed keys (`0x06`, `0x0D`). |

## How an AP runs (verified in the emulator)
* The OS loads the AP segments into RAM; a homebrew AP is one segment at **0x0800** (code,
  constants and variables), entry 0x0800, no banks. Limit 0x2B00 (~8.9 KB) — stock apps never place
  code above ~0x2B4C, the OS resident code starts at 0x2E00.
* Entry follows IAR's banked convention: `[bank word][return]` on the stack, `param` in DE, result
  in HL, exit restores the caller's bank (`sdk/crt0.s`, a copy of `?BANK_FAST_LEAVE`).
  Return `RESULT_NULL` (0) to go back to the main menu. The C entry is `uint16_t main(uint16_t param)`.
* `crt0` copies initialised data and clears zeroed data, so normal C globals work.
* SDCC: default calling convention (`__sdcccall(1)`), IY reserved (`--reserve-regs-iy`).

## SDK (`sdk/`)
| file | |
|---|---|
| `crt0.s` | entry, C runtime init, bank-restoring exit |
| `actos.s/.h` | OS calls: `os_get_msg()` (GetSysMsg, RST 08h A=0x11), `os_put_msg()`, `wdt_kick()` (port 0x4E bit 3), `os_backlight()` (UI API 0x14A4); key codes, result codes |
| `lcd_hw.s` | raw LCD bus, identical to the stock driver: memory 0x8000 with port 0x02 = 0x18 (CE3), port 0x00 \|= 0xC0 (wait states), A0 = port 0xF4 bit 2, interrupts off via the OS nesting counter at 0x0037 |
| `gfx.c/.h` | `gfx_fill`, 5×7 font `gfx_text`/`gfx_num`, `RGB()` (RGB565). Screen (x,y) → controller column x, row 127-y (128×128 visible) |

Messages from `os_get_msg()` — all confirmed on the device with KEYTEST:
| code | meaning |
|---|---|
| `0x01` | MENU key |
| `0x04` / `0x05` | LEFT (<<) / RIGHT (>>) |
| `0x06` | PLAY/PAUSE |
| `0x30` | key released (a held key repeats its code) |
| `0x80` | system tick, 2 per second |
| `0x91` / `0x92` | USB cable plugged / unplugged |

Timer: `os_timer_start(1)` = one tick every **10 ms** (KEYTEST shows ~50 ticks per `0x80`).
Call `os_get_msg()` and `wdt_kick()` regularly in every loop, and `os_timer_stop()` before exiting.

Note: in the emulator GUI the buttons are still labelled from early guesses — its "PLAY" sends 0x01
(= MENU), "NEXT" 0x04 (LEFT), "PREV" 0x05 (RIGHT), "K06" 0x06 (PLAY); "MENU" sends 0x0D, which no
physical key produces.

## Flashing — proposal
Your safety net first: the full NAND dump `readback-my-ximpro.s1clone` (restorable with s1clone)
and the chip's ADFU recovery mode (boot ROM, always available). Keep both before every flash.

1. **Now (no new tools):** the `.fw` produced by `build.py --fw` has exactly the layout s1res read
   from your player (AFI with BREC/FWSC/ADFUS/FWIMAGE, all checksums recomputed, every other file
   byte-identical to your dump — verified by unpacking it again). Write it with **s1res → write
   firmware** (the same tool and USB driver you used to read it). ~1 minute per iteration.
2. **Next step — one-command deploy (`tools/s1flash.py`, to be written):** a Python/pyusb port of
   wiRe's `AdfuSession` + `GiveIO` + `FlashIO` + `FirmwareIO` (~1.9 k lines C++ in
   `s1mp3_github/common`). Optimisation: rewrite only the FWIMAGE sectors that changed (one AP is a
   few KB), with read-back verification. GiveIO also gives a PC-side debug channel in ADFU mode:
   peek/poke RAM and ports, upload and run code. Needs testing on the device with you.
3. **Fast iteration without flashing:** the emulator (`tools/emurun.py`, `atj2085emu/`) runs the
   real firmware with the new app — every screenshot in this folder's history came from it. For
   hardware-only experiments, `loadram` (ADFU RAM upload + execute, which you already used) runs
   bare-metal code without touching the flash.
4. **Debug on the device:** `keytest` shows raw messages; a small resident "debug monitor" (dump RAM /
   ports to the screen, or over the s1debug GPIO link in `s1mp3_github/s1debug`) is the next tool.
