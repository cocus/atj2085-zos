# newbrec - our own boot record for the ATJ2085 players (milestone M1, emulator-tested only)

A 16 KB boot record (BREC) written from scratch (SDCC + sdasz80, no vendor code, nothing derived from the
untracked `brec_src/`). The mask ROM loads it from NAND blocks 0/1 and runs it at 0x0000. M1 draws the logo and a
menu **USB / STOCK / ZOS / OS2**; **USB** turns the player into a raw mass-storage disk of the whole NAND,
**STOCK** reloads the stock boot record kept in NAND block 2/3 and jumps into it. ZOS / OS2 print "NOT IN M1"
(payload start is M2).

**Status: emulator only. Nothing was flashed.** The image is `build_v04/NEWBREC-m1.BIN`
(see "Flashing" and "Open risks" before putting it on a device).

```
python newbrec/build.py                  -> newbrec/out/NEWBREC.BIN  (+ map, size budget)
python newbrec/tools/emu_test.py         -> menu, keys, USB raw disk, nandtool end to end (30 checks)
python newbrec/tools/emu_stock_test.py   -> STOCK item: stock OS boots from the block 2/3 copy (+ failure cases)
```
(venv python of the repository; the emulator tests need the untracked `my-ximpro.fw`, the NAND dump and
`atj2085emu/rom/`: set `NEWBREC_PREREQ` to the folder that holds them.)

## Layout

```
newbrec/
  build.py                 build script (regs.h -> regs.inc, emblem -> logo.h, SDCC compile, link, image + sum)
  core/                    board independent: main.c (menu, USB screens), usb.c (SIE device layer), msc.c (BOT + SCSI,
                           raw NAND disk), desc.c, target.h (the interface to the board), font5x7.h
  target/ximpro/           this board (LSD_501): regs.h (ports), head.s (header/entry/start-up), hw.c (clocks, RTC tick,
                           key pins + key map, VBUS, NAND geometry), util.s, lcd.c + lcdraw.s (LCD init table + drivers),
                           nand.s (NAND primitives), stockldr.s (STOCK loader, assembled for 0x4000)
  tools/                   nandtool.py (PC side), emuboot.py / emutransport.py / emu_test.py / emu_stock_test.py
```
Layering: `core/` only calls what `target.h` declares (tgt_*, lcd_*, nand_*). The USB engine is chip level
(ATJ2085) and sits in `core/` using the register names of the target's `regs.h`; a different chip would need its
own `usb.c`. The key and LCD code is our own small driver and does **not** use zos code (zos was only a reference; the
USB sequences are the ones we verified on the device in the zos USB module).

## Image format and memory map (16384 bytes, loaded at 0x0000)

| address | content |
|---|---|
| 0x0000-0x0007 | `jp 0x0200`, 0x00, `"BREC"`: identical to the stock record (family check of the zos FIRMWARE module) |
| 0x0008-0x0019 | `"F641" "00" ' '` + our tag `"NEWBREC M1"` (stock: release date bytes); 0x76 filler elsewhere |
| 0x0038 / 0x0066 | any interrupt -> watchdog reset / NMI `retn` (interrupts are never enabled) |
| 0x0070-0x01FF | stack (SP = 0x0200 as the stock record; measured use: 110 B, low-water mark 0x0191) |
| 0x0200-0x0214 | entry: `ld sp,0x200 ; in a,(DF) ; or 1 ; out (DF),a ; call 0x1B12 ; call 0x0216 ; ld sp,0x400 ; jp 0x600` = the stock bytes |
| 0x0216 | `jp reset` (stub where the stock record has its SYSCFG launcher; M2: the payload launcher) |
| 0x0400-0x06C2 | variables (707 B: sector buffer 512, CBW, reply buffer, NAND request, USB state) |
| 0x1B12 | `jp brec_main` (the stock `call 0x1B12` lands here: our main, never returns) |
| 0x1B15-0x3D03 | code + constants (8687 B) |
| 0x3D04-0x3FFB | free (760 B) |
| 0x3FFC / 0x3FFE | `0x55AA` / 16-bit sum of the words 0x0000-0x3FFC (the BROM check, set by build.py) |

Runtime windows: 0x4000-0x4FFF = ZRAM2 (port 05 = 0x07), the USB buffer RAM, CPU-visible with both banks MCU-owned;
0x8000 = LCD (port 02 = 0x18) or NAND CE1 (selected only inside nand.s). ZRAM2 is also where the STOCK loader runs.

## Size budget (measured, `build.py` prints it)

| part | code B | data B |
|---|---|---|
| NAND primitives (read, erase, program with ECC + BREC spare, ID) `nand.s` | 337 | 4 |
| USB device layer + descriptors `usb.c`, `desc.c` | 1955 | 83 |
| MSC: BOT, SCSI set, raw read ping-pong, raw write `msc.c` | 2432 | 604 |
| LCD driver + font (295) + logo (180) + init table `lcd.c`, `lcdraw.s` | 1387 | 4 |
| keys, clocks, RTC tick, delay, NAND geometry `hw.c`, `util.s` | 265 | 11 |
| menu, USB screens, STOCK glue + loader blob (317) `main.c` | 2173 | 1 |
| SDCC library helpers (32-bit shifts, memcpy ...) | 108 | |
| header/entry (absolute) | 32 | |
| **total** | **8687 of 9447 available (0x1B15-0x3FFB)** | **707** |

Everything fits with 760 B spare (plus ~6 KB of unused low RAM 0x06C3-0x1B11 that could take buffers). Not in the
image and its estimated cost: FTL map + user-disk LUN (read ~600 B, write/GC ~1.5 KB asm), BOOT.CFG parser (~400 B,
needs a FAT reader: not planned), payload start (M2, small), firmware A/B repair (M3, large: does not fit next to
everything else without slimming; the stock record's repair logic is ~6 KB).

## Boot flow

1. BROM -> `0x0000` -> `0x0200` entry -> `call 0x1B12` = `brec_main` (clears variables, `main`).
2. `tgt_init`: watchdog period, RTC 10 ms timer (polled), DC-DC, **key pins to plain GPIO (`P_GPB_CFG` and `P_KEYSCAN` = 0
   before the first read)**.
3. `lcd_init`: reset pulse, init table, whole GRAM black, then the menu is drawn **before** the backlight goes on.
   **Waits kept at the stock values**: 10 + 10 + 100 ms around the reset pulse and 4 x 200 ms inside the controller
   power-up table: the notes give no evidence that they can be shortened. Dropped: the stock record's extra 200 + 100 ms
   before the backlight (as `brecpatch --fast-lcd`), the FTL mount (~0.8 s) and the firmware sums (~2.2 s). Build option
   `--lcd-wait-div N` divides all LCD waits (untested on the glass, default 1).
   Emulator: menu complete at about 1.0 s (24.576 MHz T-states) after power-on, against 4.3 s to the stock OS.
4. Menu: `<<` `>>` move (wraps), PLAY selects, nothing starts by itself. **M1 policy: the menu is always shown**
   (there is nothing to boot yet). M2 will start the default entry at once unless MENU is held.
5. USB: NAND ID read (`EC D3 51 95` = 8192 blocks; unknown chips are refused with the ID shown), 48 MHz clock, attach when
   VBUS is present, polled BOT loop. MENU leaves; pulling the cable after a session also returns to the menu.
6. STOCK: see below.

## USB: the raw NAND disk

One LUN, 512-byte sectors, LBA = block * 256 + page * 4 + sector (64 pages of 4 sectors per block, 8192 blocks = 1 GB, the
chip's physical order). SCSI: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), READ FORMAT CAPACITIES,
MODE SENSE(6/10) (not protected), PREVENT/ALLOW, START STOP (eject), SYNC CACHE/VERIFY (no-ops), READ(10), WRITE(10); no vendor
commands, no second LUN. VID:PID 1209:0001 (pid.codes **test** PID, private use only), bcdDevice 1.00.

* READ(10): data columns of the page, no ECC check, bad blocks readable. The stock "ping-pong" of the two USB bank
  buffers (device-verified in zos); sectors of one page come from one NAND read.
* WRITE(10): the first sector of a block (page 0, sector 0) erases that block, every sector is then programmed with
  the hardware ECC and the BREC spare `FF x8, ECC x4, FF FF 55 FF` (verified against the chip model, identical to what the
  stock record and the zos FIRMWARE module write for blocks 0/1). A block must be written in ascending sector order; a
  random write into a programmed page fails with MEDIUM ERROR. **No protection**: blocks 0/1 are writable (that is how
  `flash-brec` works).

### Limits (M1)
* The spare area is not visible: the stock FTL's spare bytes (bad-block flag byte 0, 0xBB table marker, logical unit
  number) are neither read nor written through this disk. A dump is **data only**; restoring a dump of the FTL area
  loses the FTL metadata, so the stock FTL would not mount the restored unit tables as they were. For the boot record
  blocks (spare = BREC format) the round trip is exact. A full-fidelity spare path (vendor SCSI commands, ~1 KB) fits
  in the 760 B spare only after trimming; it was left out on purpose (user decision 1.4).
* `erase` writes an FF sector 0: the block reads all FF but sector 0 of its page 0 carries a programmed spare.
* A Windows host sees an unpartitioned raw disk and may offer to format it: never do that.

## nandtool (PC side)

```
nandtool.py DEVICE COMMAND ...      DEVICE = \\.\PhysicalDriveN (admin) | /dev/sdX (root) | image file | emu:NEWBREC.BIN
  info | dump OUT [FIRST [COUNT]] | read-block N OUT | write-block N FILE | erase N [COUNT]
  restore FILE [FIRST]  (blocks 0 and 1 last, 1 before 0) | flash-brec FILE (block 1, verify, block 0, verify)
  put-slot BLOCK FILE [--brec]   (--brec: a 16 KB boot record into BLOCK and BLOCK+1, e.g. the STOCK slot 2/3)
```
It uses plain aligned reads/writes on the drive (no SCSI pass-through needed: the disk is an ordinary raw disk),
verifies every write by reading back, and checks `0x55AA` + sum before any boot record is written. Run
`emu_test.py` for the end-to-end test on the emulated disk (flash-brec, dump, erase, slot write all pass).

## STOCK

Design (implemented, emulator-verified): the stock record lives as a copy in **block 2** (and 3). In the dump
blocks 2..7 are completely erased (data and spare); the stock FTL mount scans units from 2 (unit 1 = blocks 2/3 is
outside it) and the BROM itself tries blocks 0,1,2,3 in turn, so a copy there is a bonus fallback: if the new record
in blocks 0/1 is ever bad the BROM boots the stock copy by itself. Put it there with
`nandtool.py DEVICE put-slot 2 BREC.BIN --brec` (BREC.BIN = the stock record, e.g. saved by the FIRMWARE module).

`stockldr.s` (317 B, assembled for 0x4000, copied to the ZRAM2 window and called): pass 1 reads block 2 (else 3)
page by page into ZRAM2 0x4800 and checks it like the BROM (`0x55AA` at 0x3FFC, word sum) plus the text `BREC`
at byte 4, **without touching 0x0000-0x3FFF**; on failure it returns and the menu shows `NO STOCK IN BLOCK 2/3`.
Pass 2 reads the 8 pages to 0x0000.., sets sp = 0x4F00 and restores what the BROM leaves (usb_msc.md 4.2: 4E = A2,
04 = 01, 27 = 0, 42 = 80, 70 |= 30, 05 = F7, 00 = 10) and the registers this record changed (USB pull-up, RTC,
DC-DC, 0xDF, LCD pins to their reset values), `im 1`, `jp 0x0000`.
Emulator: with the stock record in blocks 2/3 the stock OS boots to its MUSICA main menu (`emu_stock_test.py`); with no
copy, or a copy with one flipped bit, the menu stays up with the message and the new record keeps running.

## Flashing (not done; emulator only)

1. Backup first: zos FIRMWARE `BREC BACKUP` (saves `BREC.BIN`), keep `readback-*.s1clone`.
2. Put the stock record in blocks 2/3 is only possible from a running new record (USB item + `nandtool put-slot`), so the
   order is: flash the new record, enter USB, put the stock copy, then use STOCK. To keep stock reachable the whole
   time, first write the stock copy to blocks 2/3 with s1clone (ROM recovery) or accept the ROM-recovery route.
3. `FIRMWARE > BREC UPDATE` with `NEWBREC.BIN` (copy `NEWBREC-m1.BIN` to the disk root under that name). The module's
   family check compares bytes 0x0000-0x0007 and 0x0200-0x0214 with the installed record: they are identical
   (checked by `emu_test.py`; the module itself was not run against this image). Its summary will say the code differs.
4. Recovery if the record does not run: ROM recovery (hold the NAND off until the BROM gave up, s1clone, the dump).
   `brecpatch.py` cannot check this image (it looks for the stock paint routine); its `sum_ok()` accepts the image.

## Open risks (device-untested, all emulator-only so far)
* The machine state at entry (clock 24.576 MHz / 0 wait states, port 05, DSP clock for ZRAM2) is assumed to be what the
  BROM leaves; the USB path is the zos sequence, but zos ran after the stock OS had initialised more. First test on
  a device should be with the stock copy in blocks 2/3 already in place and ROM recovery ready.
* The NAND is used without the stock mount: no timing or busy-state surprises are modelled by the emulator.
* WRITE(10) on a raw device is a power tool: a wrong host action (format, defrag) destroys the FTL area.
* Interrupted block 0/1 flash: the BROM falls back to block 1 / 2 / 3 (that is why 1 is written first).
* Capacity / chip table has only the Samsung 8 Gbit and 4 Gbit IDs; other chips are refused.
