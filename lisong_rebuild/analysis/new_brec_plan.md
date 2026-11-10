# Boot selector / new BREC: plan (written 2026-11-10, to be picked up later)

Goal (user): a boot stage that can start **zos**, a **future minimal OS** (Z80 + DAC + keys + LCD + USB,
no MP3), the **stock firmware**, or a built-in **USB mass storage (read/write)** mode, chosen at power-on
with the keys and the LCD, easily. Stock must stay reachable at all times. The user accepts the brick risk
(has ROM recovery + s1clone; see memory "RISK STANCE UPDATED").

## 1. What we know (facts, with sources)

* **The vendor source folder** (not for publication, never commit it):
  `s1mp3\Chinese Files\BREC_lowlevel_source\lowlevel\` (CVS 2005-10-26): `brecf641.asm` (the BREC main, 93 lines),
  `641Read / 641SectorRead / 641WordRead / 641FTblWordRead / 641SectorProgram / 641WordProgram / 641BlockErase.asm`
  (F641 NAND primitives), `ScanNAF.asm`, `Sdf641.asm`, `sdrw.asm`, `usbdisk51kernel.asm` (3900 lines: the stock
  USB disk kernel), `usbsuspend.asm`. It needs `actos.inc` / `actos.h` (not present) and an IAR-style Z80
  assembler. `sBREC_SystemTstRpr` (the firmware check/repair) and `syscfglauncher` are `extern`: **not in this folder**.
* **`brecf641.asm` matches our BREC's entry exactly** (user's dump, md5 of BREC `9e3174977a640d24fa0f2683c3645233`):
  header `jp 0x0200`, "BREC" "F641" "00", release 2004 + date bytes `03 15`; code at 0x0200:
  `ld sp,0x0200 ; in a,(0xDF) ; or 1 ; out (0xDF),a ; call sBREC_SystemTstRpr (=0x1B12) ; call syscfglauncher (=0x0216) ;
  ld sp,0x0400 ; jp 0x0600`. So the source is the same generation as ours; the diff task (section 6, task A) only has
  to map the lowlevel files onto address ranges of the binary and list real differences.
* **Boot chain**: BROM (mask ROM) loads 16 KB BREC from NAND blocks 0..3 (first one with `0x55AA` at 0x3FFC and the
  16-bit word sum at 0x3FFE; it reads no spare/ECC), runs it at 0x0000. BREC (our dump, boot.md 1.1): LCD init +
  hourglass (0x3000), flash detect (0x150D), FTL mount (0x2075, rewrites the tables every boot; 0x0A36 is the boot-record self check/repair, see
  brec_source_vs_binary.md), firmware check
  (0x0600) incl. A/B copy logic, `SYSCFG.SYS` lookup/load (0x0216), `sp=0x400`, `jp 0x0600`. BROM recovery ADFU only
  if no BREC block loads.
* **Contract BREC -> kernel**: kernel image at RAM 0x0600.., vector page at 0x0000, SP 0x0400, interrupts off, FTL
  state in RAM left by BREC's mount (the stock kernel and drivers rely on it; boot.md 7.3), screen initialised with
  the backlight on. zos core takes over everything else (zos_native_boot.md).
* **Free space in the 16 KB BREC**: only ~637 bytes of zeros (0x0080-0x01FF = 384 B, 0x3701-0x37FF = 253 B), plus the
  1440-byte hourglass bitmap (0x3161-0x3700) that brecpatch already replaces. ~2 KB in total: too small for a
  menu + font + FAT + USB inside the existing image by patching alone.
* **Stage 0 already exists** and has the space: `tools/fwpatch/stub/zosboot.s` is our selector that lives in
  `SYSCFG.SYS` (BREC loads up to 0x3800 bytes of it to RAM 0x0600.., the kernel image is kept as `SYSCFG.STK`). It
  already reads the user disk's FAT16 root through BREC's own flash reader, loads `ZOS.AP` to 0x0800, and re-enters
  BREC's loader with the name patched to `SYSCFG  STK` to start stock. Its own code area is 0x2C00-0x30FF
  (7 bytes free in the current build, but the zos core region 0x0800-0x2BFF is free while it decides).
* **Key/LCD facts** (device-verified): key GPIOs must be set up first (`P_GPB_CFG` / `P_KEYSCAN` = 0; the first native
  boot was bricked by reading MENU before that); keys MENU, VOL, `<<`, `>>`, PLAY (PLAY is on a different scan phase;
  see zos/targets/atj2085/platform.c). LCD init table + RAM-write sequence: boot.md 2.1 (CE3 via port 02 = 0x18, A0 on
  GPIO_C2). zos has a tiny 5x7 font and the LCD routines already.

## 1.1 DECISION (user, 2026-11-10): build a NEW BREC; nothing goes into SYSCFG.SYS

The boot menu and the USB mass-storage mode live **in the BREC itself**. Layer 1 below (menu as stage 0 in
SYSCFG.SYS) is NOT to be implemented. Correction to the premise: **our BREC has no USB stack** (scan of the 16 KB
binary: the only USB-register access is one `in` of port 0x50 (VBUS) at 0x097E; the 3900-line `usbdisk51kernel.asm`
in the vendor folder belongs to the stock USB disk AP, and the BROM's own USB code is the vendor-protocol recovery
ADFU, not mass storage). So the new BREC carries **our own** USB MSC code (zos `common/usb` MSC path, rewritten
for the BREC environment) and must fit in 16 KB together with: NAND/FTL read+write for the user disk, LCD/keys/font/
menu, the firmware check/A-B repair logic (can be slimmed: per-file sums already dropped by `brecpatch --fast`), the
SYSCFG/payload loader, and the BROM contract (`0x55AA` + sum). Size is the main risk: measure first.
Repo layout: own code in `newbrec/` (tracked, open source); the stock-derived reconstruction in `brec_src/`
(recovered from the user's dump, **UNTRACKED**). If the stock routines (mount, firmware check, SYSCFG launcher,
LCD init) are reused from `brec_src/`, the public tree cannot build the full image: keep the build script able to
use `brec_src/` when present, and document a clean-room replacement as a later step.

Milestones: M1 `newbrec` prototype = a 16 KB BREC image that boots in the emulator (BROM model), draws the boot
screen and a key menu, and runs USB MSC RW on the user disk (no stock boot yet); M2 add payload start (ZOS from the
disk / FWIMAGE) and the stock path with the RAM state the stock kernel expects (from `brec_src/`); M3 A/B repair
logic + BOOT.CFG; M4 flash through FIRMWARE BREC UPDATE (exists), recovery via ROM mode + s1clone.

## 1.2 REQUIREMENTS (user, 2026-11-10, refines 1.1: these win over 1.1 where they differ)

* **The vendor folder does contain a USB stack** (`usbdisk51kernel.asm`, `usbsuspend.asm`: the stock USB disk kernel),
  but it is not part of the BREC binary, it only exposes the user disk, and it is vendor code (reference only, never
  committed). The new BREC gets **our own** USB MSC code that exposes the **entire NAND, not just the user data**.
* **Screen/behaviour**: the new BREC just shows the logo and the options: **USB, STOCK, ZOS and one more item**
  (a placeholder for the future minimal OS; label it OS2 or similar). If there is space to spare it reads
  `BOOT.CFG`, otherwise the table is hardcoded.
* **Hardware-agnostic structure**: core code independent of the board; a `target` layer for this ximpro
  (LCD init table and routines, GPIO/key set-up, key map, clocks, NAND chip selects/geometry). The key and LCD code
  must NOT depend on zos code (own small drivers; zos stays a reference). First and only target: ximpro (LSD_501).
* **STOCK = load the stock BREC and jump into it.** Idea (to be verified in the dump/emulator): the stock BREC is kept
  as a copy in NAND unit 1 (blocks 2/3: the BROM tries blocks 0,1,2,3 in turn and stock's FTL mount scans units
  from 2, so unit 1 should be free; CHECK what is in blocks 2/3 of the dump). The new BREC copies a small loader
  to ZRAM2 (0x4000, where the BROM's own loader runs), reads the 8 pages of that block into 0x0000-0x3FFF, checks
  `0x55AA` + sum, restores the machine state the BROM leaves (usb_msc.md 4.2: watchdog 4E=0xA2, 04=01, di/im 1,
  27=0, 42=0x80 (HOSC only), 70|=0x30, 05=0xF7, 00=0x10, sp) and `jp 0x0000`: the stock BREC then boots exactly as
  before. Side benefit: if the new BREC in blocks 0/1 is ever bad, the BROM falls through to the stock copy itself.
* **USB mode = raw NAND device** plus **PC-side tools** (`newbrec/tools/`, Python, Windows SCSI pass-through via ctypes
  + Linux sg): at least `info`, `dump`, `restore`, `erase`, `read/write pages with spare`, `flash-brec`, `put-slot`.
  Proposed device model: LUN0 = raw data sectors in physical order (block, page, sector), reads always, writes with
  erase-on-first-page-of-a-block semantics and automatic ECC/spare (the BREC-style spare); vendor SCSI commands
  (CDB 0xC0-0xCF) for full-fidelity page read/program including the spare bytes (FTL unit numbers/markers must
  survive a restore), block erase, chip ID; optional LUN1 = the FTL-mapped user disk (FAT16, RW) only if space allows;
  optional spare-area LUN read-only. Bad-block policy: report, don't hide.
* **Payloads for ZOS / the extra item**: preferred = raw NAND slots written with the PC tool (no FAT code in the BREC);
  alternative = FAT16 files from the user disk (zosboot has the code) if space allows. Decide after the size budget.

## 1.3 USB item = the BROM's own ADFU (user, 2026-11-10: "or maybe expose it in DFU mode, whatever is smaller and easier")

This SUPERSEDES the raw-NAND MSC of 1.2 for the first milestones. The BROM contains a complete vendor ADFU server
(ROM 0x9000-0x9BFF, usb_msc.md 4.2: waits for VBUS, 48 MHz, enumerates VID 10D6 PID FF51 class FF/FF/FF, bulk 0x01 OUT /
0x82 IN, commands 0x05 memory read/write, 0x10 jump, 0x16 reset). The BROM runs it when no BREC can be loaded; the USB
menu item simply **jumps into it**: map the BROM at 0x8000 (ports 01=02=0, i.e. not CE3), copy 0xC00 bytes ROM 0x9000 ->
RAM 0x0000 from a tiny copier that runs from ZRAM2 (0x4000; it must not run from the area it overwrites), reproduce the
BROM's entry state (the 0x8030 reset code: 4E watchdog, 04, di, im 1, 27=0, 42=0x80, 70|=0x30, 05=0xF7, sp=0x0100, 00=0x10)
and `jp 0x0200` (the ADFU entry). ~40-60 bytes of BREC code, no USB stack in the BREC at all.
* PC side: the existing tools already speak it (s1clone: full NAND dump/restore incl. spare; s1res/giveio; our
  `lisong_rebuild/tools/s1clone.py`, `s1flash.py`). The host uploads the ADFU server (`ADFUS.BIN`, 9216 B, an AFI part of
  the user's own firmware: vendor code, never committed; tools extract it from the user's .fw like build.py does for DSP
  files). `newbrec/tools/` only needs thin wrappers (extract ADFUS.BIN from a .fw, dump/restore/flash-brec helpers) on top
  of the existing protocol code.
* The device looks like a ROM-recovery unit on the PC (10D6:FF51): same drivers the user already has (libusb/WinUSB).
* Leaving: reset command (0x16) or unplug + power cycle; there is no way back to the menu without a reset (acceptable).
* The own raw-NAND MSC of 1.2 becomes optional/later (only worth it if a driver-less file-level or raw LUN is wanted).
  Normal user-disk access stays with zos's USB module and the stock firmware.
* Risks to verify in the emulator: does the emulator model the BROM ROM at 0x8000 for BREC (ports 01/02) and its ADFU
  code with a host model? (the recovery path is reachable in the emulator when no BREC loads: `brom_load_brec` stops with
  "ROM recovery"; the same ROM code can be entered from a running BREC); does the ADFU entry need anything else set
  by the 0x8030 reset code (clock, pull-up 0x3E, watchdog)?

## 1.4 FINAL for the USB item (user, 2026-11-10: "then use MSC, which should be the quickest. ADFU doesn't make that much sense")

Own MSC in the BREC, NOT the ADFU jump of 1.3 (kept above for reasoning only). Simplest useful form first:
* One LUN: the **whole NAND as raw data sectors** in physical order (block, page, 512-byte sector). READ(10) always;
  WRITE(10): the first page of a block erases that block, pages are programmed in ascending order, hardware ECC and the
  BREC-style spare (FF x8, ECC x4, FF FF 55 FF) are generated automatically. Plain BOT + the minimum SCSI set (INQUIRY,
  TEST UNIT READY, REQUEST SENSE, READ CAPACITY, READ/WRITE(10), MODE SENSE(6), PREVENT/ALLOW, START STOP).
* NO vendor SCSI commands, no spare LUN, no FTL LUN in M1 (later, only if wanted). The PC sees a raw disk: the PC tool
  reads/writes the physical drive (`\\.\PhysicalDriveN` on Windows, needs admin; `/dev/sdX` on Linux) with
  block-aligned (erase-block-sized) writes. Limitation (documented): a raw write cannot restore FTL-managed areas
  exactly (the spare bytes get the BREC format); use it for BREC blocks, payload slots, dumps of the data; full NAND
  restores stay with s1clone in ROM-recovery mode.
* PC tools (`newbrec/tools/nandtool.py`): `info`, `dump` (data), `read-block`, `erase`, `write-block`, `flash-brec`
  (blocks 1 then 0, verify), `put-slot`; works on a physical-drive path or on an image file/emulator transport for
  tests.
* Everything else of 1.2 stands (menu USB/STOCK/ZOS/OS2, hardcoded table or BOOT.CFG if space, target layer for the
  ximpro, STOCK = reload the stock BREC from unit 1 / blocks 2-3 and jump).

## 2. Two layers (OBSOLETE: superseded by 1.1; kept for the reasoning)

**Layer 1: boot menu as stage 0 in SYSCFG.SYS** (extends zosboot; no BREC write needed, flashable with s1res):
* power-on: no key held -> default entry, immediately (no delay; boot speed matters to the user);
  hold a key (suggest MENU) -> the menu; hold VOL -> stock directly (as today).
* menu on the LCD: title, 4-6 entries, highlight with `<<` `>>`, PLAY starts, MENU = default; entries from
  built-ins (STOCK, USB DISK) plus the entries listed in a text file on the user disk (`BOOT.CFG` in the root:
  `label=FILE.AP` per line, file loaded to 0x0800 like `ZOS.AP`; editable from a PC through the stock USB disk, no
  tool needed) and the choice of default (`DEFAULT=n`).
* payload types: **stock** (re-enter BREC's loader with the `SYSCFG  STK` name, exists), **disk AP** (zos, the future
  minimal OS, anything built like `ZOS.AP`; exists), **USB MSC RW**: a standalone payload `USBMSC.AP` on the disk
  (zos's USB mass storage code on a minimal HAL: LCD status line, MENU = leave/reset). It is also the first brick of
  the future minimal OS, so build it as the first "mini-OS" target (see section 5).
* room: SYSCFG.SYS may be 0x3800 bytes; the selector gets 0x2C00-0x30FF now, and we can move the sector buffer /
  loaded payload area to give it 3-4 KB (menu + font + BOOT.CFG parser). Budget to verify.

**Layer 2: our own BREC** (later; only if layer 1 is not enough). Reasons it could be worth it:
* boot time: no hourglass/blue init waits, no per-file sums (brecpatch --fast already gets SYSCFG start at 1.84 s,
  a from-scratch BREC could skip the FTL mount rewrite too, but then must provide the FTL RAM state itself);
* a boot screen/menu that appears at ~0.3 s instead of after the mount;
* no dependency on the stock kernel at all (needed only if the stock fallback is ever dropped).
Costs/risks: stock kernel and drivers expect BREC's RAM state (FTL descriptors at 0x3C80 etc., clock/watchdog
setup), the A/B firmware copy and repair logic must be kept for s1res, and a bad BREC needs ROM recovery. The vendor
`lowlevel` NAND sources give the confirmed F641 read/program/erase protocol to write our own (our code, not copies).
If we do it: keep stock's BREC code as the starting point by *hooking* (brecpatch) before writing from scratch.

## 3. Boot policy (proposal, to confirm with the user)

| situation at power-on | action |
|---|---|
| nothing held | default entry from `BOOT.CFG` (factory default: ZOS if present and valid, else stock) |
| MENU held | boot menu |
| VOL held | stock (escape, as today) |
| USB cable present | default entry (option: USB DISK straight away; the old VBUS->stock escape is optional) |
| payload missing/corrupt | stock, never a hang (zosboot already does this) |

## 4. Work breakdown (each step independently testable in the emulator; nothing flashed by agents)

1. **zosboot menu**: add LCD text (init is already done by BREC: only fill/text), keys (after GPIO init), menu loop,
   BOOT.CFG reader (FAT file read exists), entries; keep the "any failure -> stock" invariant; emulator test matrix
   extended (`tools/fwpatch/test/emu_matrix.py`): key presses at power-on (the emulator's key model + early boot),
   each entry, missing file, garbage file, bad BOOT.CFG.
2. **USBMSC.AP**: standalone MSC RW payload from zos's `common/usb` code (MSC only) on a minimal startup (no zos
   core): clocks (48 MHz), NAND/FTL read + write (`nand.s`, `ftlw.inc`), FAT not needed (raw LBA to the host), LCD
   status line, unplug/eject handling; tested with the emulator's USB host model (`zos/tools/emu_usb_test.py`).
3. **mini-OS skeleton** (the future OS for Z80 + DAC + keys + LCD + USB): HAL subset of zos (hal.h targets),
   build target with its own core, "hello" + USB + DAC beep; loaded as an AP from BOOT.CFG. Decide here how much of
   zos's core is reused (git subtree / shared `common`), since the user says "no mp3".
4. **zospatch v2**: patcher support for the new stage 0 (selector size, BOOT.CFG default file creation), docs.
5. **(optional) BREC layer 2**: hook-based first (brecpatch `--menu`: use the freed ~2 KB for an early key check that
   skips straight into a chosen payload before the mount), then evaluate a from-scratch BREC.

## 5. Open questions for the user

* Which key opens the menu (MENU?) and should the menu also be reachable by holding PLAY-length etc.?
* Default when nothing is held: ZOS, or stock? Should USB cable at power-on go straight to MSC?
* Where should payloads live: user disk root (easy, no reflash, recommended) or inside FWIMAGE (survives disk
  formatting)? The disk is the plan.
* Name/label set for the menu (EN/ES?).
* Is layer 2 (own BREC) wanted now, or after the menu exists?

## 6. Tasks to hand to agents when work resumes

* **Task A: vendor source vs our BREC (diff).** Map each `lowlevel\*.asm` function to address ranges in our 16 KB BREC
  (user's dump: `readback-my-ximpro.s1clone`, BREC extracted by `tools/brecpatch/brecpatch.py` `load()`), by
  disassembling the binary (`lisong_rebuild/tools/z80dis.py`) and matching instruction sequences by hand/script
  (the source needs `actos.inc`: symbols can be reconstructed from the binary). Output: a table (source routine ->
  binary address, identical / different / absent), the real differences (constants, extra code such as the hourglass
  and LCD, the F641-specific paths), and what the source adds to our knowledge (names, comments, NAND timing,
  `ScanNAF` bad-block logic, `sdrw` for SD, `usbdisk51kernel` = stock USB disk). Write findings into
  `lisong_rebuild/analysis/brec_source_vs_binary.md` **in our own words, no verbatim vendor code**; raw diffs stay in the
  scratchpad. The vendor folder is never copied into the repo.
* **Task B: steps 1-4 above** (one agent per step, in separate git worktrees, merged by the coordinator).

## 7. How to resume

* Repo: `C:\Users\cocus\Downloads\s1mp3` (git; last commits: README screenshots, brecpatch fast boot default).
* Tools that exist: `tools/fwpatch/zospatch.py` (+ `stub/zosboot.s`, `build_stub.py`, `test/emu_matrix.py`),
  `tools/brecpatch/brecpatch.py`, `zos/modules/firmware` (BREC BACKUP/UPDATE/RESTORE), `atj2085emu` (real BROM + BREC
  model in `emu/soc.py brom_load_brec`), `zos/tools/emu_usb_test.py`.
* Read first: this file, `boot.md`, `zos_native_boot.md`, `tools/fwpatch/README.md`, `zos/NAND_AND_BOOT.md`
  (sections 4-5), memory notes (zos-project, atj2085-device-facts, zos-build-deploy, emulator-usage).
