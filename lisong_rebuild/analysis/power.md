# Power-related hardware: backlight, contrast, battery, USB cable (what zos needs)

Evidence tags: **[fw]** = read in stock code (file + address), **[doc]** = ATJ2085X Programming Guide v3.1 (PG) / SSD1789A
datasheet, **[inf]** = inferred, **[emu]** = measured in the emulator on the real NAND dump.
Stock file addresses: `drivers/disasm/UI_6B33L_DRV/seg_bank*.s` = driver CPU addresses (bank B loads at 0x4800).
`SYSBNKA.SYS` / `SYSCFG.SYS` have no disassembly in the tree: addresses below are **file offsets** in
`orig/fwimage/` (decoded with `tools/z80dis.py`; SYSCFG CPU address = file offset + 0x600, which is why usb_msc.md says 0x2F38
for what is file 0x2938). AP code is in `disasm_v04/<AP>_AP/`.

## 1. LCD backlight

| Item | Value | Evidence |
|---|---|---|
| Port/bit | **port 0xEE bit 1** = GPO_A1 data (plain GPIO output, EE[2:0] = GPO_A[2:0]). 1 = on, 0 = off | **[fw]** `UI_6B33L.DRV` bank B04 0x4948: `in a,(EE); set 1,a / res 1,a; out (EE),a` (API 0x14A4, `os_backlight`). Init turns it on at bank B00 0x481f. **[doc]** atj2085_hw.md 2.1/6 (EEh) |
| Brightness levels | **None**: it is one GPO bit, no PWM/duty register exists in the PG (GPO_A has no PWM function). Only on/off. | **[doc]** + **[fw]** no other EE/PWM access in the driver |
| Preserve other bits | EE[7:5] = MFP pin-mode select, EE.4 = CE0S, EE.3 = GPO_A2 enable, EE.0 = LCD reset (must stay 1 after init). Always read-modify-write. | **[fw]** driver does RMW; **[doc]** PG p.40 |
| Panel sleep sequence | `UI 0x15A4` (bank B04 0x495d): OFF = backlight off (EE.1=0), cmd `AE` (display off), 250 ms; ON = cmd `AF`, 20 ms, cmd `94` (sleep out), 250 ms, backlight on. | **[fw]** bank B04 0x495d-0x4994 |
| Mode flag | MAIN keeps a "backlight type" in comval[20] (RAM 0x0809). If it is 1 the OS switches the real backlight via `os_backlight`; if not 1 it fakes "backlight off" with `SetContrast(0)`. The real device NAND has comval[20] = 1. | **[fw]** MAIN bank40 0x10ce (`f_b40_10ce`), set from comval[20] in 0x10e9; **[emu]** comval dump after boot (see 2) |

### Stock timeout logic (MAIN.AP bank40)

* Settings come from comval (512-byte block, buffer 0x08bf in MAIN): `0x10e9` stores `comval[9]*2` into 0x0802 (backlight timeout in
  ticks), `comval[11]` -> 0x0803 (auto power-off, derived from minutes), `comval[10]*2` -> 0x0805 (sleep timer), `comval[8]` -> 0x080b
  (contrast), `comval[20]` -> 0x0809 (backlight mode). **[fw]** bank40 0x10e9-0x1124. The tick unit is one per MAIN "timer" message
  (0x0f path at 0x1152); I did not measure it, 0.5 s is the likely value **[inf]** (timeout = comval[9] x 2 ticks; value on the device = 10).
* Tick handler `f_b40_1266`: `0x0807++` (idle ticks); when `0x0807 == 0x0802` it calls `f_b40_114a(E=0)` -> `f_b40_10ce(E=0)` ->
  `os_backlight(0)` and clears flag 0x080a (backlight is off). **[fw]** 0x1266-0x1282.
* Wake: any key message (dispatch at 0x1152; msgs 0x0a, 0x15, 0x16, 0x30, 0x31, 0x80-0x92 ...) resets 0x0807 and 0x080e; if the
  flag 0x080a is 0 (backlight currently off) it calls `f_b40_1148` = `f_b40_10ce(E=1)` = `os_backlight(1)` (0x1253-0x125b). The key
  that wakes it is not otherwise consumed in that path (it jumps to 0x131e) **[fw]**. Keys are polled in `KY_HARD.DRV` (ports F2/F0/F1).
* zos: implement as a counter in the main loop; on any key event set `EE.1`, reset counter; at timeout clear `EE.1`.

### regs.h block (proposed)

```c
/* ---- backlight: GPO_A1 on port EE (pin shared with ICECK; no PWM, on/off only) -------------- */
/* P_MFP (0xEE) already defined: bits 7-5 MFP, 4 CE0S, 3 GPO_A2 enable, 2 GPO_A2, 1 GPO_A1, 0 GPO_A0 */
#define MFP_GPOA0_LCDRST 0x01   /* LCD reset (active low), keep 1 */
#define MFP_GPOA1_BL     0x02   /* LCD backlight: 1 = on */
```

```c
static void backlight(uint8_t on)
{
    uint8_t v = inp(P_MFP);                 /* read-modify-write, bits 7-5 = pin mode */
    outp(P_MFP, on ? (v | MFP_GPOA1_BL) : (v & ~MFP_GPOA1_BL));
}
```

Confidence: port/bit **high [fw]**; no brightness **high [doc]** (cannot be excluded that a board-level PWM exists, but the driver
never touches one); timeout tick period medium [inf].

## 2. LCD contrast

Call chain: `os_set_contrast(v)` (actos.s) = `RST 10h HL=0x11A4` = UI driver bank B04 entry 1 (`f_b84_492a`, bank B04 0x492a).
The OS-side wrapper in MAIN is `f_c_0bf6` (text 0x0bf6).

| Step | Bus action | Evidence |
|---|---|---|
| 1 | LCD command `0x81` (A0 = 0, store to 0x8000) = SSD1789A **"Set contrast / regulator resistor ratio"** | **[fw]** B04 0x492f `ld c,#0x81; call f_c_4500` ; **[doc]** SSD1789A cmd 0x81 (2 parameters) |
| 2 | data byte 1 = **`E + 0x19`** (E = low byte of the argument), i.e. contrast level = argument + 25 | **[fw]** B04 0x4934-0x4938 (`ld a,e; add a,#0x19; ld c,a; call f_c_4508`) |
| 3 | data byte 2 = **`0x07`** (internal regulator resistor ratio) | **[fw]** B04 0x493b |
| Init value | `81 38 06` (level 0x38 = 56, ratio 6) in the init table | **[fw]** bank B00 0x48a8; README (drivers) |

* Range: the SSD1789A contrast parameter is 6 bits (0-63) **[doc]**, so the useful argument range is E = 0 .. 38 (E + 0x19 <= 0x3F). The
  stock driver does **not** clamp E. The AP slider limits were not located (SETTING.AP bank42 `f_b42_1ea8` calls SetContrast from a
  generic slider widget, 0x1ea8) **[inf]**.
* Default: first word of comval (0xDEAD magic) then `comval[8]` (u16 contrast). The device's saved comval has **comval[8] = 6**
  (so command byte 0x1F = 31, regulator 7) **[emu]** (RAM 0x08bf after a stock boot on `readback-my-ximpro.s1clone`:
  `ad de 00 00 00 00 b6 03 06 0a 00 00 0a 00 00 02 1c 00 00 00 01 ...` -> [8]=06 contrast, [9]=0x0a backlight timeout, [20]=01
  backlight mode). zmain already restores it with `os_set_contrast(comval[8])`.
* zos direct form (zos keeps the bus selected: `lcd_cmd` sends the command, parameters are plain stores to LCD_DATA):

```c
#define LCD_CONTRAST 0x81
#define CONTRAST_BASE 0x19      /* stock adds 0x19 to the user value */
#define CONTRAST_REG  0x07      /* regulator ratio used by SetContrast (init uses 0x06) */
static void lcd_contrast(uint8_t v)             /* v = 0..38, stock default 6 */
{
    lcd_cmd(LCD_CONTRAST);
    *(volatile uint8_t *)LCD_DATA = v + CONTRAST_BASE;
    *(volatile uint8_t *)LCD_DATA = CONTRAST_REG;
}
```

Confidence: command/parameters **high [fw]**; default 6 **[emu]** (device dump); max value medium [doc].
Note: other SSD1789A init values (`F3 90 05 60 84`, `FB 02`, ...) are in drivers/README.md and zos lcd init.

## 3. Battery voltage

| Item | Value | Evidence |
|---|---|---|
| Register | **port 0xD0 bits 3-0 = 4-bit battery monitor ADC result (RO)**. Bits 7-4 are USB TX bias/force (RW, default 0101): **mask before use, never write D0 without preserving them** | **[doc]** PG p.49-50 (item 3); **[fw]** `SYSBNKA` file 0x441c `in a,(D0); and 0x0F` |
| Starting a conversion | **None needed**. The ADC free-runs (samples every 0.125 s, 8x smoothed, latched into D0[3:0] once per second). The stock code only reads it; it never writes anything to enable it. | **[doc]** PG p.47 ("refreshed once per second"); **[fw]** all `in a,(D0)` sites are pure reads |
| Scaling | 16 levels = **0.90 V .. 1.50 V at the ADC input, 0.04 V/step** (n=0 -> 0.90 V, n=15 -> 1.50 V). VBAT pad range 0-3 V, halved or not depending on BATSEL/DCDIS pins. DF[7:6] LBD levels 0.90/0.98/1.06/1.14 V correspond to n = 0/2/4/6; DF[5:4] LBNMI 0.98/1.06/1.14/1.22 V = n 2/4/6/8 | **[doc]** PG p.47, p.50 |
| Battery type | thresholds below (n=6 => 1.14 V, full 13-15 => 1.42-1.5 V) fit a **single 1.5 V cell (AA/AAA/NiMH), not halved** | **[inf]** |
| OS API | `GetBattery` = `RST 10h HL=0x0022`: **returns 0x0F if VBUS (0x50.6) is present, else `D0 & 0x0F`** | **[fw]** SYSBNKA file 0x4413-0x4420 |

### Stock thresholds

| Where | Rule | Evidence |
|---|---|---|
| Battery icon | AP code (e.g. MUSIC bank6b `0x17df`, draw call `0x16a8`): VBUS present -> `D0&0F`; if > 12 show "full", else charging animation (counter 0x0811 cycles icons 0..8). On battery: n < 6 -> level 0 (empty); 6..12 -> level n-5 (1..7); n >= 13 -> level 8 (full). Image id = 0x015D + level (9 images). | **[fw]** MUSIC_AP bank6b 0x17df-0x1850 |
| Low-battery message | Kernel tick (SYSCFG file 0x2938-0x2977, CPU 0x2F38): only when VBUS absent. Reads `D0&0F` and compares with a setting at RAM 0x009d: 0 -> n < **4**, 1 -> n < **5**, 2 -> n < **6** (set from the SYSCFG init struct, file 0x125; meaning of the option is not known). If below: posts system message **0x82** (low battery) into the queue 0x0115 and **`out (04h),01h`**. MAIN then handles 0x82 (shutdown). | **[fw]** SYSCFG 0x294d-0x2975 |
| Boot check | MAIN bank40 0x1a87: `GetBattery` < **7** -> shows a low-battery page (calls bank 0x40:0x139e / 0x102d) before continuing | **[fw]** MAIN_AP bank40 0x1a84-0x1abc |
| Hardware | LBNMI (04h.5 flag, enabled with 04h.1, level DF[5:4], default 10 = 1.14 V = n 6) and LBD system reset (DF[7:6], default 00 = 0.90 V); stock standby resets DF[5:4] to 00 and sets DF.0 (DCOP clock select) | **[doc]** PG p.12, p.50; **[fw]** STANDBY.AP text 0x09fb |
| Standby low-battery wait | STANDBY.AP text 0x09dc: LRADC off (D1.7=0), clocks to 32 kHz (00=0), WD off (4E=22h), PLL off, then loops until VBUS present or `D0&0F` >= 3 | **[fw]** STANDBY_AP seg_text 0x09dc-0x0a39 |

### regs.h block (proposed)

```c
/* ---- battery monitor / power control ---------------------------------------------------------- */
#define P_BATADC        0xD0    /* 3-0 RO: 4-bit battery ADC (0.90 V + 0.04 V * n at the ADC input); 7-4 USB TX bias/force (RW, keep) */
#define BATADC_MASK     0x0F
#define BAT_EMPTY_ICON  6       /* stock icon: n < 6 = empty; 6..12 -> 1..7; >= 13 full */
#define BAT_FULL_ICON   13
#define BAT_LOW_MSG_MIN 4       /* stock kernel low-battery message below 4/5/6 (setting dependent) */
#define BAT_BOOT_MIN    7       /* stock MAIN boot warning below 7 */
#define P_PWRCTL        0xDF    /* 7-6 LBD reset level, 5-4 LBNMI level, 3-2 IBIAS, 1 VCCOUT, 0 rd DCDIS pin / wr DCOP clk */
#define P_VDETECT       0x3F    /* VDD/VCC detect enable + level */
#define P_DCDC          0x4F
#define P_NMICTL        0x04    /* bit 5 LBNMI flag W1C, bit 3 DCDIS NMI W1C, bit 1 NMI enable, bit 0 must be 1 */
```

```c
static uint8_t battery_level(void)              /* 0..15 raw; 0..8 icon via bat_icon() */
{
    return inp(P_BATADC) & BATADC_MASK;         /* free-running, updates 1 Hz: no start needed */
}
static uint8_t bat_icon(uint8_t n)
{
    return n < 6 ? 0 : (n > 12 ? 8 : n - 5);
}
static uint8_t battery_low(uint8_t n) { return !usb_cable() && n < 5; }   /* stock: 4/5/6 */
```

Confidence: register/format/scale **high [doc]+[fw]**; thresholds **high [fw]**; battery chemistry **inferred**. The value is only
meaningful against load: stock reads it with VBUS absent, and the PLL/DSP/NAND load pulls it down; expect jitter of +/-1, so average
or hysteresis (the stock icon code has none, it re-reads every redraw).

## 4. USB cable detection

| Item | Value | Evidence |
|---|---|---|
| Port/bit | **port 0x50 bit 6 (USBPWR/VBUS), read-only**; 1 = cable (5 V) present | **[fw]** `os_usb_cable` = `RST 10h HL=0x0021`, handler SYSBNKA file 0x421b: `in a,(50); and 0x40; ret`; kernel tick SYSCFG file 0x297a (usb_msc.md); **[doc]** PG USB SIE port 50h |
| Kernel behaviour | tick compares `0x50&0x40` with saved bit (RAM 0x0134, initialised at SYSCFG file 0x38a) and posts message **0x91 plug / 0x92 unplug** | **[fw]** SYSCFG file 0x297a-0x2991 |
| IRQ | USB IRQ 0x52 bit 6 = connect/disconnect (only if USB IRQ enabled) | **[doc]** usb_msc.md |
| Note | VBUS is also a charger/power source indication: `GetBattery` returns 15 while VBUS present, so the battery ADC is meaningless then in the stock UI. Reading D0 while VBUS present may be disturbed by charging; zos should do the same. Chip pins DCDIS/BATSEL select the power mode; DF.0 reads the DCDIS pin (DC adapter) **[doc]** | |

`USBST_VBUS` already exists in zos regs.h (`P_USB_STAT` 0x50, 0x40).

```c
static uint8_t usb_cable(void) { return (inp(P_USB_STAT) & USBST_VBUS) != 0; }
```

Confidence: **high [fw]+[doc]**.

## 5. Other power-related findings

* **Charging status**: no dedicated charge-status input was found in the stock code. Only VBUS (0x50.6) is used; the stock UI shows a
  charging animation whenever VBUS is present and `D0&0F` <= 12 (MUSIC bank6b 0x17e5-0x17fb) and "full" when > 12. Charger
  control (if any) is not via these ports. **[fw]** The DCDIS pin level is readable at **DF bit 0** **[doc]**.
* **Power-off / standby**: there is no software power latch pin in the stock code. Standby = STANDBY.AP routine (text 0x09dc/0x0a42):
  LRADC off (`D1 &= 0x7F`), MCU clock to 32 kHz LOSC (`00 = 0`), WD disable (`4E = 0x22`), PLL off (`42 &= 0x3D`), DF[5:4] = 0, then
  waits for VBUS or good battery; wake-up reverses (`D1 |= 0x80`, `42 |= 0x80` HOSC, `00 = 0x10`). The `out (04),01` in the kernel
  low-battery path just sets 04h.0 (normal A15 mapping; NMI enable is 04h.1 so it does not itself power off). Real hard power-off of the
  chip is by the DC-DC / external switch and was not found **[inf]**. Wake sources from clock stop: key IRQ, alarm, SIRQ, DCDIS NMI **[doc]**.
* **GPIO_B7 / F2 bit 7**: the kernel tick (SYSCFG 0x290c-0x2935) reads `in a,(F2)` bit 7 when flag 0x009a is set and posts message
  **0xB0 / 0xB1** on change. It is undocumented in the PG (bit 7 "must not be written"); `KY_HARD` has a stub `HoldCheck`, so this is
  most likely a hold/lock switch or earphone detect **[inf]**. Not verified on the device; zos can log the bit while plugging the
  headphones/switching the hold slider to find out.
* **Headphone detect**: no code found that reads a port for it (only the F2.7 candidate above). Beep/key click path copies FF.0 onto EE.2 (KY_HARD entry 6),
  GPO_A2 enable = EE.3: probably an amplifier/LED, unrelated.
* **GPO_A pins are ICE pins**: EE.1 (backlight) and EE.0 (LCD reset) do not work with an ICE attached (PG p.40).
* DC-DC (4Fh, VCC 2.6-3.3 V / VDD 1.6-2.3 V) and voltage detect (3Fh) are left at defaults by the APs (only STANDBY touches DF); zos should too.

## Summary table

| Item | Port/bit | Confidence |
|---|---|---|
| Backlight | 0xEE bit 1 (RMW), on/off only, no PWM | high [fw] |
| Contrast | LCD cmd 0x81, data = value+0x19, then 0x07; stock default value 6 | high [fw], default [emu] |
| Battery | 0xD0 bits 3-0, free-running, 0.9 V + 0.04 V*n; icon <6 empty, 6..12 -> 1..7, >=13 full; low msg n<4/5/6; boot warn n<7 | high [doc]+[fw] |
| USB cable | 0x50 bit 6 (RO) | high [fw]+[doc] |
| Other | charge status = VBUS only; F2.7 = hold/headphone candidate (msg 0xB0/0xB1); DF.0 = DCDIS pin | medium/guess |

## 6. Power-off / standby (stock behaviour, and what zos does)

**Summary: the stock firmware never cuts the power.** There is no power-hold pin and no DC-DC "off" bit in use (4Fh is
only saved/restored). "Off" = STANDBY.AP: display off, CPU on the 32 kHz LOSC with HOSC/PLLs/watchdog off, polling PLAY
and the USB cable; "on" = restore everything and **continue in MAIN** (no reset, RAM kept). The DCDIS/BATSEL pins only
strap the DC-DC mode **[doc]** (DS 4.8, PG 12.3); the only chip-level off is the LBD reset below DF[7:6] (0.90 V default).

### Trigger (all [fw])
* KY_HARD posts the key code on press, the same code again every 8 scans while held, 0x30 on release (drivers/disasm
  KY_HARD 0x0669-0x06e5). Each AP's message filter (MAIN bank40 `f_b40_1152`, 0x11dc-0x121a) counts repeats: the 4th
  becomes `key|0x80` (long), later ones `key|0x40` (hold), release `key|0x20`. PLAY = 0x06 -> **0x86 = long PLAY**
  (about 1.5-2 s on the device, depends on the kernel timer; emulator: standby.ap started 4.5 s after the press).
* The common hot-key handler (MAIN bank40 0x1407, IAR `?C_V_SWITCH` at 0x1409; the **same table
  `15 16 1a 1d 1f 2e 81 86 88 8d` is in every AP**: FMRADIO, MUSIC, RECORD, ... file offsets e.g. MUSIC.AP 0x56f7)
  maps **0x86 -> result 2 (RESULT_STANDBY)**, also **0x1f** (auto power-off: MAIN tick 0x1266, idle counter 0x0807 ==
  0x0803 = comval[11]) and **0x1d** (sleep timer 0x0805). Default branch: `GetBattery() < 5` -> result 2 (low battery;
  the kernel tick posts 0x82 below n = 4/5/6, power.md 3). Note: the switch table is in reverse order (0x8dea: entry
  index = n - k), 0x81 (long MENU) -> 0x0a.
* The AP returns 2 to MAIN; MAIN's dispatch (bank40 0x194b, case 2 = 0x1a78) shows the low-battery page if
  `GetBattery() < 7`, then `ExecAP("standby.ap", 0)` (0x1acc). (Emulator: a 3 s hold in FMRADIO was not enough, 4 s in MAIN was; the emulator timer is a guess.)

### STANDBY.AP (bank41 0x1300, text 0x09dc / 0x0a42)
1. comval read, `ClearScreen`, `UI_LCD_Sleep(0)` (light off, LCD cmd AE, 250 ms).
2. Save ports 42, 4F, 00, 4E, DF, 04; DI-count; save 43, 27; `43 = 04` (RTC timer + 2 Hz IRQ off), `27 = 20`.
3. Wait until PLAY has been released for 0x2000 consecutive polls (watchdog kicked).
4. Loop `f_b41_15eb` with interrupts off: `09dc`: `D1 &= 7F` (SAR ADC off), `00 = 00` (LOSC /1), delay,
   `4E = 22` (watchdog off), `42 &= 3D` (HOSC, PLL1, PLL2 off), `DF = DF & CF | 01`; **wait while no VBUS and
   `D0 & 0F < 3`** (battery too low: the key is not even polled); `4E = saved | 08`. Then `1607`: 80 polls (watchdog
   kick each): **VBUS -> 4** (checked first), **PLAY -> 0x33** (key read = phase 2 of the scan: drive B6, read B1).
   No HALT, no clock stop: a busy loop at 32 kHz.
5. PLAY: `0a42` (`D1 |= 80`, `42 |= 80` HOSC, delay, `00 = 10`), then PLAY must stay down for 8 x `f_b41_1681`
   (200 x 200 watchdog kicks, ~0.95 s at 24.576 MHz measured in T-states) -> wake; released earlier -> back to 4.
   VBUS: if `(50 & 30) == 30` (D+ and D- both high = charger) **stay in standby** (PLAY is then never looked at,
   because VBUS is checked first); else clocks back, return 4 -> MAIN case 4 = `ExecAP("usbdisk.ap")`.
6. Exit: restore 43, 27, `UI_LCD_Sleep(1)` (AF, 20 ms, 94, 250 ms, light on), restore 42, 4F, 00, 4E, DF, 04,
   contrast; return 0x33 -> MAIN (bank40 0x1913) redraws its menu and goes on.
* Emulator ([emu], real NAND dump, MAIN menu, PLAY held 4 s): `ExecAP standby.ap`, then `43=04, D1=04, 4E=22,
  42=00, DF=09` (from 29); PLAY 3 s later: `D1=84, 42=80, 00=10`, 23 M T-states later `43=64`, LCD `94`, `EE=0b`
  (light), `42/4F/DF/04` restored, MAIN menu back on screen. (D1 is OR-ed with 80 on wake even if it was 04 before.)
* Charging: no software charge control anywhere; VBUS (50.6) is the only input. Standby current per DS: VDD 110 uA +
  VCC 35 uA typ in "standby mode" (clock stop); the stock 32 kHz polling loop is above that.
* Not used by stock: clock stop (00 low nibble = DC) with wake by key-scan IRQ / RTC alarm / SIRQ / DCDIS NMI (PG
  p.10) - the keys here are a GPIO matrix, not the key-scan controller, so PLAY cannot raise an IRQ.

### zos (2026-11)
* Core (`targets/atj2085/platform.c`): PLAY held 100 scans (2 s) posts `EV_POWER` once and swallows the rest of
  that press (no `EV_KEY_UP`); API `hal_backlight(on)` (light off works like the idle timeout: the next press only
  wakes the light and is swallowed). +42 bytes code, +1 byte RAM.
* Modules: SHELL acts on PLAY release; on `EV_POWER` it saves ZOS.CFG and runs `modules/shell/standby.s` (the stock
  loop above, copied to ZRAM1 0x3800, position independent), then display on and redraw; woken by a PC on USB it
  chains to USB. MP3PLAY / MUSIC / FM / SETTINGS leave on `EV_POWER` through their MENU path (MP3PLAY: resume point
  + `mp3_off` fade) with `MOD_POWER_REQ` set; SHELL powers off when it starts. Other modules ignore it.
* Differences to stock: hold time 2 s (zos timer) and 1 s to wake; PLAY also wakes while a charger is plugged in;
  D1/DF/43 restored to their saved values; no automatic low-battery / idle power-off yet (core has 6 bytes left).
