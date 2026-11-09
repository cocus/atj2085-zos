# Where the firmware lives, the NAND map, and how the player boots

Board: Ximpro / LISONG LSD_501, ATJ2085X (Z80 MCU + 24-bit DSP), 1 GB NAND on CE1.
Sources: `lisong_rebuild/analysis/` (nand_ftl.md, boot.md, usb_msc.md, zos_native_boot.md), all
measured on this player's dump `readback-my-ximpro.s1clone` and firmware `my-ximpro.fw`.

## 1. Where the firmware is

| Place | What | Can it change? |
|---|---|---|
| Chip mask ROM, **BROM** (8 KB, bank 0 at 0x8000) | reset code, NAND boot loader, USB recovery mode (ADFU, VID 10D6 PID FF51) | no |
| Chip mask ROM, **TROM** (21 KB, bank 2) | DSP coefficient tables (FFT/DCT twiddles, decoder tables) - not code | no |
| NAND **blocks 0 and 1** | **BREC** (`BRECF641.BIN`, 16 KB, one copy per block): the boot record | only by GiveIO / ROM recovery tools |
| NAND **firmware area** (FTL logical units 0x00-0x3F) | **FWIMAGE.FW** (6.4 MB file system image), copy A + copy B | s1res (normal ADFU) writes copy A |
| NAND **user disk** (FTL logical units 0x40-) | VM settings (stock), FAT16 volume (your files, `ZOS.AP`, `*.ZM`, `ZOS.CFG`) | USB mass storage, zos |

The `.fw` file you flash (`my-ximpro.fw`) is an Actions "AFI" container: `BRECF641.BIN` (boot record),
`ADFUS.BIN` (ADFU server, used by recovery tools), and `FWIMAGE.FW`. FWIMAGE.FW is a small file
system with a header (`55 AA F0 0F`), a directory and per-file checksums; its files are the stock OS:
`SYSCFG.SYS` (kernel), `MAIN.AP`, `MUSIC.AP`, `GAME.AP`, ... (applications), `*.DRV` (drivers),
`*.DSP` (DSP programs: `MP3DEC.DSP`, `PCMDEC.DSP`, ...), `*.RES` (images/strings), `*.$$$` (fonts).

zos today:
* **hosted** (current device setup): `ZOS.AP` + modules on the FAT disk; started by `GAME.AP` (ZBOOT, flashed)
  or the homebrew launcher. The stock firmware boots first.
* **native** (`s1mp3/build_v04/zos-native-v04.fw` - the only valid copy; the old one under
  `lisong_rebuild/build_v04` was broken and is renamed OBSOLETE-*): the zos core is inside FWIMAGE as
  `SYSCFG.SYS` (stub + core), the stock kernel kept as `SYSCFG.STK`. Modules still come from the FAT disk.

## 2. NAND map

Geometry: 2048-byte pages + 64-byte spare, 64 pages per block, 8192 blocks = 1 GB.
FTL ("641" stock format): a **unit** = 2 blocks (both planes) = 256 KB = 512 sectors of 512 bytes;
2 **zones** of 2048 units (4096 blocks) each; each zone maps 0x7B0 logical units.

### Physical

| Blocks | Content |
|---|---|
| 0, 1 | BREC (pages 0-7 of each block = 16 KB; block 1 is the spare copy). Outside the FTL. |
| zone 0: 2..4095 | units of zone 0: data units (logical 0x000-0x7AF), free/spare units, the **temp unit** (this player: unit 0x070 = blocks 0xE0/0xE1), the **mapping-table unit** (spare byte 1 = 0xBB; unit 0x050 = block 0xA0) |
| zone 1: 4096..8191 | units of zone 1: logical 0x7B0-0xF5F, temp unit 0x435, table unit 0x049 (block 0x1092) |
| 9 bad blocks | 0x451 0x6A1 0xC82 0x12FC 0x131F 0x1350 0x137C 0x19AA 0x1AF7 (spare byte 0 = 0x00) |

Which physical unit holds a logical unit is in the zone's mapping table (2048 u16 entries, copies
appended page-pair by page-pair in the table block; entry 0x7FD = temp unit, 0x7FF = 0x55AA).
Every 512-byte sector carries 16 spare bytes: `FF FF L_lo L_hi FF FF FF FF ECC(4) FF FF 55 FF`
(L = logical unit, 0xBB in byte 1 marks the table unit).

### Logical (what the OS sees)

| Logical units (zone 0 first) | Bytes | Content |
|---|---|---|
| 0x00-0x1F | 0 - 8 MB | **FWIMAGE.FW copy A** (s1res writes this) |
| 0x20-0x3F | 8 - 16 MB | **FWIMAGE.FW copy B** (BREC copies A -> B when they differ) |
| 0x40 | 256 KB | "VM": stock settings, user-disk sectors 0-511 (only 0-127 used) |
| 0x41 - 0xF5F | ~968 MB | **user disk**: FAT16 superfloppy, boot sector at user-disk sector 0x200 (= unit 0x41) |

User-disk sector S -> logical unit 0x40 + S/512, sector S%512 (zos `hal_blk_read(lba)` = sector 0x200 + lba).
zos never writes units 0x00-0x40 or blocks 0/1: its write path (`modules/common/ftlw.inc`) refuses them.

## 3. Boot process

```
power on
  |  BROM (mask ROM): clock to 24.576 MHz, reads BREC from NAND blocks 0/1, checks
  |  0x55AA + word sum at the end of the 16 KB. Unreadable BREC -> USB recovery (ADFU PID FF51).
  v
BREC (16 KB, RAM 0x0000-0x3FFF)                                         ~4.3 s in total
  1. LCD init, blue screen + HOURGLASS, backlight on                      (~1.3 s)
  2. NAND detect, FTL mount: scans the spare of every unit, rebuilds and  (~0.8 s, writes NAND:
     re-writes the mapping tables                                          16 programs + 4 erases)
  3. firmware check: FWIMAGE header (copy A, compared with copy B; copies (~2.2 s, CPU bound)
     A -> B if needed), word sums of every file except *.$$$ / *.RES
  4. loads SYSCFG.SYS: file 0x0000-0x2CFF -> RAM 0x0600, vector page -> RAM 0x0000,
     SP = 0x0400, jp 0x0600
  v
stock:   SYSCFG.SYS = ACTOS kernel -> loads MAIN.AP -> the UI driver re-inits the LCD (~1 s)
         -> boot animation -> main menu (MUSICA, ..., JUEGOS = GAME.AP)
zos hosted:  main menu -> JUEGOS -> GAME.AP (= ZBOOT) loads ZOS.AP from the FAT disk -> zos
         takes over the machine (own interrupts, drivers; the stock OS is never called again)
zos native (zos-native-v04.fw):
         SYSCFG.SYS = stub + zos core. The stub checks: MENU or VOL
         held (or zos damaged) -> makes BREC load
         SYSCFG.STK = the untouched stock kernel -> normal stock boot. Otherwise it sets up clocks,
         watchdog, DC-DC, keys like the stock OS and jumps to zos (no second LCD init): zos is up
         ~0.1 s after BREC, the stock menu needs another 3.5-6 s.
```

Leaving zos (REBOOT, or a crash -> watchdog reset) restarts from BROM.

### Recovery ladder
1. zos misbehaves -> it is only files on the FAT disk: fix them over USB (zos USB mode, or the stock one).
2. Native firmware: hold **MENU or VOL** from power-on (device-verified) -> stock firmware; from there
   s1res can flash `my-ximpro.fw` back (normal ADFU, writes FWIMAGE copy A only).
3. FWIMAGE broken but BREC fine: BREC shows the hourglass and stops (fallback to copy B not analysed).
4. BREC unreadable: the BROM enters USB recovery by itself -> s1clone / vendor tool with the dump
   (`readback-my-ximpro.s1clone` holds the whole NAND).
