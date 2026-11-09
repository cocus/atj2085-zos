# I2C / FM tuner on the ATJ2085 Lisong board

## Mechanism: software (bit-banged) I2C on GPIO C, not the 7Ah-7Ch hardware block

The stock `FMRADIO.AP` (v04, `disasm_v04/FMRADIO_AP/seg_bank12.s`) never touches ports 71h/7Ah/7Bh/7Ch.
It drives the tuner by bit-banging on GPIO C. The v04 code is the same as the Actions SDK source
`ATJ2085/912018811MP4_src_core_2091N/.../case_i_d913f/ap/ap_radio/FM_i2cdrv1.msa` and `FM_i2cdrv2.msa`
("philips TEA5767 FM module driver (I2C DRIVER)"), which has the port defines and comments.

| signal | pin | data (port F4h) | output enable (F3h) | input enable (F3h) |
|---|---|---|---|---|
| SCL | 63 GPIO_C0 (alt: I2C_SCL) | bit 0 | bit 0 | bit 4 |
| SDA | 62 GPIO_C1 (alt: I2C_SDA / SIRQ-) | bit 1 | bit 1 | bit 5 |

Evidence: SDK defines `SDA_outport 0F3H/1`, `SDA_inport 0F3H/5`, `SCL_outport 0F3H/0`, `SCL_inport 0F3H/4`,
`SDA_doport/dinport 0F4H/1`, `SCL_doport/dinport 0F4H/0`. In the v04 AP: `I2C_init_b12` at 0x28CA (sets
F4.1, F4.0, F3.1, F3.0), `I2C_exit_b12` 0x28E5 (clears F3.1, F3.0), `I2C_start_b12` 0x28F4,
`I2C_stop_b12` 0x2906. The register map is in `atj2085_hw.md` (F3h = GPIO_C OE/IE, F4h = GPIO_C data;
pin table section 2.2).

### Electrical behaviour
* Outputs are push-pull: the bus is driven high by data=1 with OE=1. There is no open-drain emulation
  and no tri-state on SCL (SCL is always driven by the master).
* SDA is turned around for ACK/data reads: OE off (F3.1=0), IE on (F3.5=1), then the pin level is read
  from F4.1. After the ACK the stock code turns IE off, loads data=0 and re-enables OE.
* Idle: SCL=SDA=1, both OE on (`I2C_init`), written data first, then OE.
* START: SDA low, wait, SCL low. STOP: SCL high, wait, SDA high. Bit: set SDA, wait, SCL high, wait, SCL low.
* Delays are `I2C_wait5us/3us`: call + push/pop/nop padding, about 80 T-states. At the 24 MHz core that is
  3-4 us per half bit, so about 100 kHz or faster. TEA5767 max is 400 kHz.
* The hardware I2C master (71h/7Ah/7Bh/7Ch, `atj2085_hw.md`) exists and the pins list I2C_SCL/SDA as their
  alternate function, but nothing in the stock firmware uses it, and the pin-mux that routes it to C0/C1 is
  not documented anywhere I could find. Treat it as unverified/unused.

## The chip: TEA5767 at I2C 0x60 (8-bit 0xC0 write / 0xC1 read)

* Probe in the Lisong v04 AP: `testFMi2c_b12` (0x2AFA): 4 x `FMwait50ms` (200 ms), START, send 0xC0,
  ACK = tuner present, STOP. It runs right after `I2C_init` in `FM_INIT_Drv_b12` (0x2500).
  The board may therefore be built without the tuner (no ACK).
* Write frame (`WRITE_ONEFRAME`): START, 0xC0, 5 data bytes, STOP. Buffer layout (SDK `WRITE_BUFFER`):
  * byte1: MUTE(7) SM(6) PLL[13:8]
  * byte2: PLL[7:0]
  * byte3: SUD SSL1 SSL0 HLSI MS ML MR SWP1. Stock default HLSI=1, so byte3 = 0x10.
  * byte4: SWP2 STBY BL XTAL SMUTE HCC SNC SI. Stock defaults XTAL=1 (32.768 kHz crystal; the AP stores 1
    at 0x0AB1), HCC=1, SNC=1, so byte4 = 0x16. STBY (bit 6) = standby.
  * byte5: PLLREF DTC = 0
* Initial buffer 80 00 00 00 00 (muted). PLL = 125*(f_kHz + 225)/1024 for high-side injection
  (`CALCULATE_PLL`), -225 for low-side. The stock `FM_setfrequency` tries both injections, compares the
  level, and keeps the better one; the final write is unmuted.
* Read frame (`READ_ONEFRAME`): START, 0xC1, 5 bytes (ACK x4, NACK on the last), STOP.
  * byte1: RF ready(7), band-limit(6), PLL[13:8]
  * byte2: PLL[7:0]
  * byte3: stereo(7), IF counter(6:0)
  * byte4: level ADC(7:4)
  * `READ_STATUS`: wait 50 ms, then read up to 4 times until the ready flag (bit 7 of byte1) is set.
* IF counter window used by the stock search: 0x31..0x3E (`IFlowlimit/IFhighlimit`).
* Other bus devices: none. The FM driver talks only to 0xC0/0xC1. The rest of the player (RTC, codec) is
  internal to the ATJ2085. The schematic note ("FM (acfmx01, TEA5767-like) on FMDATA/FMCLK (I2C)") agrees.
* No GPIO power or enable line for the tuner. The only power control is the TEA5767 STBY bit. The stock
  `FM_Init` also calls the ACTOS APIs AI_Enable (0x0029), AI_SetGain and PA_Enable, which route FM audio
  into the analog input / power amp. That is audio routing, not needed for I2C, and zos does not do it
  (so no sound from the tuner yet).

## Conflicts with zos
* Keys: zos scans GPIO B (F0h-F2h). The I2C pins are GPIO C (F3h/F4h), so there is no conflict.
* LCD A0 = F4 bit 2 and C2 shares F3 bit 2/6 with the pins. The I2C code does read-modify-write with masks
  and only touches F4 bits 0-1 and F3 bits 0,1,4,5. Reserved F3/F4 bits are never written with 1
  (atj2085_hw.md item 13). The zos timer IRQ (key scan) does not touch F3/F4, so a bit being delayed
  by an IRQ is harmless. Do not call gfx_* inside a bus transaction (LCD writes toggle F4.2 with RMW,
  harmless to the data bits but it lengthens the SCL phase).
* Reset state is F3=07h, F4=03h (both lines driven high). The module saves/restores these bits.

## What was wrong in `modules/i2ctest/main.c` (original)
* Used a non-existent I2C driver model: ports 71h/7Ah/7Bh/7Ch with 8051/PIC-style bit layouts
  ("F1 mode" switching, ACK in the control register). The stock firmware does not use this block, and the
  pin-mux needed to connect it is unknown. All register comments were copied from other code (71h is
  an I2C address register per PG, not "wait states"; 7Ah is not "external page high", etc.).
* The scan drew one line per address with `x += 16` into a `uint8_t` y, which wrapped past 255.
* No bus recovery and no state restore.

## What the user should see on the device
Run I2CTEST from MODULES. First screen: "IDLE SCL=1 SDA=1" (lines released and read back), then the scan of
0x03-0x77 and a compact list of ACKing addresses in hex. On a working board there should be exactly one:
`60 TEA`. Press `>>` to open the tuner screen. It shows the frequency (starts at 98.7), the 5 bytes
written and the 5 bytes read, READY, band flag, stereo, PLL read-back, IF counter and level 0-15 with a
bar. `<<`/`>>` move 100 kHz (wraps 87.5-108.0), PLAY toggles mute, MENU returns to the scan (tuner
goes to standby), MENU again exits and restores the pins. READY should become 1 and the IF counter
should be in the 31h..3Eh range on a station. If it says "NO ACK FROM 60", the tuner is missing or not
powered; if the scan finds nothing and idle shows SDA=0, the bus is stuck or has no pull-ups.

## Untested / open
* Not run on the device. The atj2085emu does not model F3/F4 as I2C pins (pin levels are just stored),
  so a scan in the emulator finds nothing and a read of F4.1 returns whatever was written.
* Whether the input read of F4 returns the pin level with IE=1 and OE=0 is taken from the stock driver
  (it relies on this); the programming-guide text for F3/F4 is only in the digest.
