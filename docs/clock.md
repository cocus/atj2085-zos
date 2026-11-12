# zos wall clock and the DS3231

What the user sees: `HH:MM` (24 h, no seconds) in the title bar of every app that has room for it (next to the
battery icon, else at the left edge; short titles only: the time is skipped when the title would overlap),
date + time + source in SYSINFO, SET CLOCK in SETTINGS (year, month, day, hour, minute: `<<` `>>` change the
field, VOL next field, PLAY sets, MENU leaves; setting makes seconds 0 and validates the day of the month).

## Where the time comes from (once per boot, SHELL: `modules/shell/main.c clock_boot`)
1. **DS3231** on the I2C bus (address 0x68), if it answers and its oscillator-stop flag (status 0Fh bit 7) is clear.
2. else the **anchor** in `ZOS.CFG` (`CLOCKDAY=` days since 2000-01-01, `CLOCKMIN=` minute of the day): the OS
   time at the last save. It is written when the clock is set, at power-off / reboot, and on any other settings
   save when it is 30+ minutes old (a save that changes nothing else is skipped: no NAND wear). Time that
   passed while the player was off is lost (accepted); an anchor older than the build is ignored.
3. else the **build date/time** (`out/atj2085/buildtime.h`, made by `build.py`).

SYSINFO shows which one is in use (`SRC DS3231 / SAVED / BUILD`), the DS3231 probe (`DS OK / NONE / OSF`),
the MCU RTC counter and its health (`OK`, or `En` = the counter disagreed n times).

## How it runs
* The core (`targets/atj2085/clock.s`, from the 2 Hz RTC interrupt) keeps `hh:mm:ss` + a day count in the
  17-byte block `HAL_CLK` (`hal/hal.h`, HAL_SHARED+444) and posts `EV_TIME` when the minute changes. No API
  entries: modules read the block directly (`modules/common/skintime.h`). An older core never sets the marker
  byte, so new modules just show no time.
* The MCU RTC has no date/time registers, only a 24-bit 2 Hz counter (ports 45h-47h, port 43h bit 3 latches it).
  zos never trusts it across a cold boot; it is the running time base: `now = base + (counter - cnt) / 2`.
  SHELL compares it with the software clock on every start, each minute and after standby
  (`modules/common/rtc.c clk_sync`), so time that passed while the tick was stopped (standby, interrupt-off
  windows) is added. A counter that went backwards / was cleared: the clock re-bases on the software time
  (never garbage) and `err` counts it. It does not matter if the counter is off at power-on.
* The players (MP3PLAY, DECPLAY) have no room: MUSIC draws the title bar (with the time when "NOW PLAYING"
  leaves room, which it does not) and the time is not refreshed while they run.
* Only 2000-2099 are handled.

## DS3231 wiring
The same two pins as the FM tuner (stock `FMRADIO.AP`, `lisong_rebuild/analysis/i2c.md`): **SCL = MCU pin 63
(GPIO_C0), SDA = MCU pin 62 (GPIO_C1)**, plus GND and VCC = 3.3 V (use the supply the tuner module uses; the DS3231
module's own battery keeps the time while the player is off). SQW/INT, 32K are not used. The bus is shared:
TEA5767 0x60 / RDA5807 0x10, 0x11, 0x60 and the DS3231 0x68 do not collide; the driver
(`modules/common/ds3231.c`) leaves the pins as it found them after each transfer and uses the tuner's timing.
The MCU drives SCL and SDA push-pull and only releases SDA to read, so the bus needs pull-ups on SDA (the
DS3231 is open drain): the tuner boards normally have them (the stock driver relies on them to read the
tuner's ACK), but this is not verified on the board: run I2CTEST (TOOLS) with the DS3231 connected; if idle shows
SDA = 0, add 4.7 k pull-ups to 3.3 V on SDA and SCL. Many cheap DS3231 modules (ZS-042) have a charging circuit
for a CR2032: use a non-rechargeable cell only with that circuit disabled.

## Firmware consequence
The tick is in the core. A native firmware (core inside the flash) shows nothing until it is reflashed with the
new core; with an old core the new modules run unchanged but without a clock (SYSINFO says `NO CLOCK (OLD CORE)`).
The `zospatch` firmware loads `ZOS.AP` from the disk, so copying the new `ZOS.AP` is enough there.

## Emulator
`atj2085emu/headless.py` and `zos/tools/emu_nand_zos.py` take `--rtc [YYYY-MM-DDTHH:MM:SS|osf]` (a DS3231 model
on the I2C bus, also together with `--tuner`) and `--rtc-counter N`; `emu_nand_zos.py --events` adds
`T:rtc+N` (counter jumps: time passing while the CPU is stopped), `T:rtcclear`, `T:nocore` (old core).
