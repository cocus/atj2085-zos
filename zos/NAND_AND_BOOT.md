# Where the firmware lives, the NAND map, and how the player boots

Board: Ximpro / LISONG LSD_501, ATJ2085X (Z80 MCU + 24-bit DSP), 1 GB NAND on CE1.
Sources: `lisong_rebuild/analysis/` (nand_ftl.md, boot.md, usb_msc.md, zos_native_boot.md), all
measured on this player's dump `readback-my-ximpro.s1clone` and firmware `my-ximpro.fw`.

## 1. Where the firmware is

| Place | What | Can it change? |
|---|---|---|
| Chip mask ROM, **BROM** (8 KB, bank 0 at 0x8000) | reset code, NAND boot loader, USB recovery mode (ADFU, VID 10D6 PID FF51) | no |
| Chip mask ROM, **TROM** (21 KB, bank 2) | DSP coefficient tables (FFT/DCT twiddles, decoder tables) - not code | no |
| NAND **blocks 0 and 1** | **BREC** (`BRECF641.BIN`, 16 KB, one copy per block): the boot record | ROM recovery tools; zos FIRMWARE `BREC UPDATE/RESTORE` (section 5, emulator-verified only); **not** s1res |
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
zos never writes units 0x00-0x40 or blocks 0/1 through the FTL: its write path (`modules/common/ftlw.inc`)
refuses them. The only exceptions are in the FIRMWARE module: FWIMAGE copy A (`ftlw_fwwrite`) and the raw
boot-record path `ftlw_brec_erase/prog` (blocks 0 and 1 only, section 5).

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
3. FWIMAGE copy A broken but BREC fine: **BREC repairs A from B by itself** (see 4 below).
   Both copies broken: BREC jumps to 0x2063 = the mask ROM's USB recovery (ADFU) -> s1res / s1clone.

## 4. What BREC does with the two FWIMAGE copies (BREC 0x0600, emulator-verified)

```
0x0606  verify(A at fw sector 0)        full check: magic, header u16 sum, dir u32 sum, every
        |                               non-.$$$/.RES file's u32 sum (0x06DB -> 0x0875 -> 0x091F)
        +- A ok:  verify(B at 0x4000) in light mode (magic only), compare header +4..+7 and the
        |         dir sum (+0x10) with A's: equal -> boot; different -> erase VM area (fw sector
        |         0x8000.., 128 sectors = stock settings) and copy A -> B (0x0978), re-verify (0x0606)
        +- A bad: verify(B) FULLY: ok -> copy B -> A (0x0978, 0x4000 sectors), re-verify, boot
                                    bad -> 0x2063: BROM ADFU recovery code copied to RAM, USB recovery
```
Emulator runs (real BREC from the dump, `atj2085emu --nand`, overlays): A with a flipped byte,
A = half new / half old image, A cut by a power loss during a zos FIRMWARE update (inside a unit,
right after a unit's old block erase = mid copy-back, during unit 0 = header/directory) -> each
time B is verified, copied over A (~139 M T-states = ~5.7 s more hourglass) and the stock
firmware boots with A = B = the old image; the FTL mount also resolved the interrupted unit close
(two units with the same logical number; afterwards 0 spare/table mismatches). A valid new A ->
A copied to B on the next boot (~140 M T). A and B both bad -> 0x2063.
Consequence for updates (zos FIRMWARE module, s1res): write copy A only and never B; an
interrupted write is undone by the next boot. Residual risk (also stock): a power loss during
BREC's own A -> B copy leaves B with A's header but partial data; since BREC only checks B's
header while A is valid, B stays silently bad until A also breaks (then: ROM recovery).
4. BREC unreadable (no block 0-3 passes the check, section 5.1): the BROM enters USB recovery by itself -> s1clone / vendor tool with the dump
   (`readback-my-ximpro.s1clone` holds the whole NAND).

## 5. The boot record in NAND, and how zos rewrites it

### 5.1 Physical layout (measured on the dump, BROM loader disassembled)

| | block 0 | block 1 |
|---|---|---|
| pages 0-7 (2048 + 64 bytes each) | BREC bytes 0x0000-0x3FFF, page p = bytes p*0x800.. | identical copy (dump: byte-identical, data and spare) |
| pages 8-63 | erased (all FF, data and spare) | erased |

* Every 512-byte sector g (4 per page) has the 16-byte spare group at column 0x800 + 16g:
  `FF FF FF FF FF FF FF FF  ECC0 ECC1 ECC2 ECC3  FF FF 55 FF` - the user-disk format (nand_ftl.md 2.2)
  with bad-block byte 0 = FF, byte 1 = FF and **logical unit FF FF** (no FTL unit). ECC = the
  controller's 24-bit Hamming code (ports 0xCD, 0xCC, 0xCF, 0xCE; nand_ftl.md 1.5); all 64 sectors of
  both blocks match it. Blocks 0/1 are the two planes of physical unit 0, which the FTL never uses
  (the mount scan does not include unit 0, boot.md 3; ftlw.inc cannot address it).
* **What the mask ROM checks** (BROM loader 0x8100, copied to and run at 0x4000; large-page path
  0x4213, sector read 0x439B, check 0x436E): for block 0, 1, 2, 3 in turn it reads the **data columns
  of pages 0-7** (32 x 512 bytes by DMA2, ports 0x14-0x21, into RAM 0x0000) and accepts the first block
  whose u16 at 0x3FFC is `0x55AA` and whose u16 at 0x3FFE equals the 16-bit sum of the little-endian
  words 0x0000-0x3FFC. **It reads no spare byte, no bad-block marker and does not use the ECC engine**
  (an earlier note in usb_msc.md said so; the loader has no access to ports 0xCC-0xCF). If no block
  passes: the USB recovery code (ROM 0x9000) runs. So a half-written block 0 falls back to block 1.
  BREC itself starts at 0x0000 with `jp 0x0200` and the text "BREC".
* zos writes the same format as the vendor tool did: an **identical** BREC rewritten by zos gives
  pages that are byte-identical to the dump, data and spare (emulator check below).

### 5.2 BREC BACKUP / UPDATE / RESTORE (FIRMWARE module)

* **BREC BACKUP**: reads block 0 (or block 1 if block 0 is invalid) through the core raw page read,
  checks `0x55AA` + sum, reports whether block 1 is a valid identical copy, and saves the 16 KB as
  `BREC.BIN` in the disk root (`BRCnnn.BIN` if `BREC.BIN` exists: the first backup is never
  overwritten), read back through the FAT and compared with the NAND.
* **BREC UPDATE** (`NEWBREC.BIN`) / **BREC RESTORE** (`BREC.BIN`): `BREC.BIN` must exist and be valid
  (UPDATE makes it first if it is missing). The file must be 16384 bytes with `0x55AA` + correct sum and
  belong to the same boot-record family as the installed one (bytes 0x0000-0x0007 and the entry code
  0x0200-0x0214 identical); the summary shows the number of differing bytes and whether they are all
  in the boot-screen area (0x3000-0x37FF + the sum). `>>` held 2 s writes:
  1. block 1: single-block erase (`60 row D0`), 32 sector programs (`80`, 5 address cycles, 512 bytes by
     DMA1 with the ECC engine running, `85` to the spare group, `FF x8, ECC, FF FF 55 FF`, `10`), then
     every data byte and every spare byte read back (ECC = the bytes the engine produced), and the
     block's sum re-checked;
  2. only if block 1 verified: block 0 the same way.
  A failure in block 1 stops before block 0 is touched. The path is separate from the FTL
  (`ftlw_brec_erase/prog`, assembled only with `FTLW_BREC`) and refuses any block other than 0 and 1.
* **Risk**: a boot record that passes the sum but does not run (wrong code, a hang before the LCD or
  NAND init) makes the player unbootable, and the ROM will not enter recovery by itself because the
  block passes its check. Recovery then needs the ROM to **fail** reading blocks 0-3: keep the NAND
  from answering at power-up (a switch in series with the NAND CE# line, or CE# held high, until the
  ROM has tried its four blocks), release it, and write the dump back with s1clone in ROM recovery
  mode. Only install boot records made by `tools/brecpatch` from this player's own `BREC.BIN`.

### 5.3 Emulator verification (real dump, `atj2085emu --nand`)
The emulator now models the ROM loader (`soc.py brom_load_brec`: blocks 0-3, data of pages 0-7 through
the NAND chip model incl. overlays, `0x55AA` + sum, first valid block; none -> stop "ROM recovery").
* BREC BACKUP: `BREC.BIN` = the dump's BREC (md5 9e3174977a640d24fa0f2683c3645233), FAT check clean,
  nothing written to blocks 0-3.
* BREC UPDATE with a brecpatch output: erase block 1, 32 programs, erase block 0, 32 programs (no NAND
  rule violation: no overprogram, NOP <= 4, ascending pages); both blocks = the patched BREC with the
  spare format above and pages 8-63 erased. Cold boot: ROM model takes block 0, black screen + ZOS
  emblem, stock OS to its menu. With the same FTL state, original vs patched boot record: identical
  port I/O statistics, identical T-states to the SYSCFG jump (105.61 M), RAM at SYSCFG start identical
  except the bitmap and the sum.
* BREC RESTORE: blocks 0 and 1 byte-identical to the dump again (all 64 pages, data + spare), blue
  hourglass, normal boot.
* `NEWBREC.BIN` with one flipped bit ("55AA / SUM WRONG") and a random file with a valid sum ("NOT THIS
  BOOT RECORD"): refused, no erase or program in blocks 0-3.
* ROM model: block 0 erased + block 1 valid -> boots from block 1; both erased -> ROM recovery.
