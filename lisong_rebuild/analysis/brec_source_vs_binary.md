# Vendor BREC low-level sources versus the BREC binary of the device

Scope: research only. Compared the Actions "lowlevel" source folder (13 `.asm` files, IAR-style Z80
assembly, 2002-2004 copyright headers, folder imported into CVS on 2005-10-26) with the 16 384-byte BREC
of `readback-my-ximpro.s1clone` (md5 `9e3174977a640d24fa0f2683c3645233`, release bytes 2004-03-15).
The vendor files are **not** part of this repository and are described here in our own words only
(routine names are the vendor's identifiers, everything else is paraphrase and measured facts).
Method: disassembly with `lisong_rebuild/tools/z80dis.py`, routine-by-routine matching by instruction
sequence, port numbers and constants. The `actos.inc`/`actos.h` symbols that are missing from the folder
were reconstructed from the binary where needed (port names follow `zos/targets/atj2085/regs.h`).

## 1. Headline

* **Same family, same generation of ideas, not the same build.** `brecf641.asm` matches the binary's header
  and entry exactly, and a handful of routines (flash-type scan, block erase, word program, ECC
  compare/correct, table-word cache) have the same logic instruction for instruction. But the sector read /
  program, the device table and the whole linear (FTL) layer in the binary are a **later, large-page-only,
  zone-aware** rework of what the source shows (the source is still an SLC/small-block-oriented revision with
  half-finished large-page support). Roughly, of the 7 341 code bytes of the BREC: ~5 % are the same logic as
  a source routine, ~18 % have the same role with changed protocol/constants, ~25 % are the reworked linear
  layer (`sdrw`-like), and **~51 % have no counterpart in the folder** (firmware check, boot-record self repair,
  FTL mount, syscfg launcher, LCD/hourglass). The 1 440-byte hourglass bitmap and the init script are data.
* The build switches visible in the source and chosen by the binary: ECC support on, DMA1 variant of the
  controller (`ICVer3951`) on, wait-state constant 3 (DMA mode 0xC0), the `TestEcc` test code (erase-on-page-0)
  and `debug` off.
* **Three of the 13 files do not belong to the BREC at all:** `usbdisk51kernel.asm`, `usbsuspend.asm`
  (stock USB mass-storage engine, part of USBDISK.AP) and `Sdf641.asm` (an entry switch for the system-disk
  driver). They are absent from the binary (it never touches USB ports except reading VBUS, nor CTC timers).
* **Correction to our notes:** `sSD_*` means **system disk** (the firmware space of the NAND), not an SD card.
  `sdrw.asm` is the firmware-space linear layer; there is no SD-card code in the folder.
* **Correction to our notes (boot.md / nand_ftl.md / new_brec_plan.md):** BREC `0x0A36` is **not** the FTL
  mount. It is the **boot-record self check/repair** (verify the copies of BREC itself in NAND blocks 0..3 and
  rewrite a damaged one). The real mount is `0x2075` -> `0x20B8` -> `0x2146` -> scan `0x22E1`.
* `ScanNAF.asm` contains **no bad-block logic**; it only identifies chips by ID. Bad-block handling in the
  vendor code lives in `sdrw.asm` (erase failure -> mark the unit 0xFFFE) and, in the binary, also in the mount scan
  (spare byte 0 != 0xFF -> skip).

## 2. Routine mapping (source -> binary)

Addresses are BREC RAM/file addresses (BREC runs from 0x0000-0x3FFF). "Same logic" = same sequence of
operations and constants up to register allocation / addressing.

| vendor file / routine | binary address range | verdict |
|---|---|---|
| `brecf641` header + `Start` (stack, clock-mode bit, call SystemTstRpr, call syscfglauncher, SP=0x400, jp 0x600) | 0x0000-0x0011 header, 0x0200-0x0215 | **identical** (header "BREC" "F641" "00", 0x2004, month/day BCD built from the assembler date = 03/15; StorageInfor 24 bytes at 0x0080) |
| `brecf641` externs: `sBREC_SystemTstRpr` | 0x1B12-0x1B31 | not in folder; reconstructed (section 3) |
| `syscfglauncher` | 0x0216-0x0239 wrapper, 0x023A-0x0374 loader | not in folder; reconstructed |
| `sSD_SimpleInit` / `sSD_SimpleExit` / `sSD_SimpleEntry` | 0x03F5 (returns 1) / 0x03F4 (ret) / 0x3FDB (jump to 0x3CAF reader) | not in folder; the "simple" read-only firmware reader |
| `641Read`: `sNAF641_Read` | 0x3DC9-0x3E2F | **different**: the sub-sector branch (one-sector cache in a 512-byte buffer at 0x3A80, tag compare, copy) is active in the binary but commented out in the source |
| `641Read`/`641SectorRead`: `sNAF641_SectorRead` | 0x3E30-0x3F02 | **same role, different protocol**: binary = large page (command 00, 5 address cycles, 30, random read 05 ... E0 for the stored ECC); source = small page (4 address cycles, sequential skip of spare bytes). The ECC check/compare constants and the bit-correct step are **identical** (0xE1 mask, 0x1E or, inc/jr, xor), DMA1 programming covers the same 13 registers but the binary loads them with an `outi` table (0x3DBC) and fixes the tR wait at 0x40 loops (source: 60 loops, or 128 when port 0x70 bit 3 says 48 MHz) |
| `641Read`: `sNAFL_EccCorrect` | 0x3F03-0x3F1A | **identical** logic (flip bit selected by the top 3 bits of B/C inside the sector) |
| `641SectorRead` wrapper (BC=0x200) | folded into 0x3DC9 | callers pass BC=0x200 and `sNAF641_Read` tests B==2 |
| `641WordRead`: `sNAF641_WordRead` / `DwordRead` | 0x17E1-0x1807 (generalised "read N bytes") | **different**: 5 address cycles, 30, wait 0x40 loops (the same 25 us delay), N bytes by PIO from the data window; no small-page 0x50 pointer command |
| `641FTblWordRead` | 0x3F69-0x3FCD, window 0x3F48 | **same logic, different constants**: 32-byte window (mask 0xE0 / 0x1F, length 0x20) instead of 64 bytes (0xC0 / 0x3F, 64), plus one more tag (the CE port value) |
| `641SectorProgram` | 0x1808-0x18D2 | **same role, different details**: ICVer3951 branch (DMA1 source regs 06/07/09/0A, dest regs 0C/0D/0E), mode 0xC0 from `WaitState`=3; large page (80 + 5 address cycles, then 85 + column and 16 spare bytes incl. ECC bytes) vs. small page; the binary has no erase-on-page-0 test code |
| `641WordProgram`: `sNAF641_DwordProgram` | 0x18D3-0x18ED | **same logic**: program command + address, two 16-bit words written to the data window, program confirm, status; (the source first writes a pointer command 0x00/0x01/0x50 for small-page areas; the binary does not) |
| `641WordProgram`: `sNAF641_WordProgram` | 0x18EE-0x18F6 | **identical** (BC = 0xFFFF, call the dword version) |
| `641WordProgram`: `sNAF_WriteCMD` | 0x3D9A and its entry points 0x3D7E/83/88/8D | **different shape**: binary issues one command byte (the caller passes 0x80/0x60/0x00) followed by 2..5 address bytes (`IssueCMD2..5`); the source writes pointer + 0x80 and 4 address bytes |
| `641BlockErase` | 0x1954-0x1977 | **identical** logic (60 + 3 row bytes, D0, status); the binary adds a watchdog kick and a two-plane variant at 0x1910 (not in source) |
| `ScanNAF`: `sSTG_ScanObNAF` | 0x1514-0x156A | **identical** (boot flash first, then CE6..CE1 skipping 4 = card socket and 3 = LCD, StorageInfor+0 mask, +1 type, capacity link table at +20 downwards) |
| `ScanNAF`: `sSTG_ScanSMC` | 0x156B-0x15A2 | **identical** (CE4, type value 0x10, capacity at +16, mask at +2) |
| `ScanNAF`: `sSTG_CheckNAF` | 0x15A3-0x17AD | **same structure, different table**: enable, reset (3 = no chip), read ID, dispatch on maker (98 Toshiba, EC/AD/20 Samsung-style) then device ID; the binary adds makers 0x2C and 0xC1, more devices (D3, D5, ...), a second geometry word and a multiply helper (0x17AE) that computes capacity and units per zone; for EC D3 it returns type 1 (MLC), 0x7B0 logical units per zone, 0x800 units per zone, capacity 0x1EC0 (the source's table says 0x1080 and large block) |
| `Sdf641`: `sSD_Entry` (A = 0 read, 1 write, 2 init, 3 exit) | none | **not present** (driver-side entry switch of the system-disk driver, no BREC role) |
| `sdrw`: `sSD_SectorRead` | 0x1F73-0x1F91 | **same role** (convert LBA, translate, read one sector); zone-aware, large page |
| `sdrw`: `sSD_SectorWrite` | 0x03A5-0x03C2 | **same role** (convert, map for write, program one sector with ECC); the program step is 0x1F32 |
| `sdrw`: `sSD_LBAtoChipLBA` | 0x1DD1-0x1DEF | **different**: no 4 M-sector range check; splits the sector into unit + sector-in-unit |
| `sdrw`: `sSDF321_ReadLA2PA` | 0x1DF7-0x1E55 with 0x1BAA (find chip + zone) and 0x1BF8 (load zone table) | **reworked**: one-entry read cache, zones, table loaded on demand into a window (port 05 = 0xF1) |
| `sdrw`: `sSDF321_WriteLA2PA` + `FillForSkipPage` | 0x1E8D-0x1F31 + 0x1978-0x1A9F | **same idea, reworked**: continue the open temp unit for ascending writes, close otherwise; skipped sectors filled by copy-back; binary uses two-plane copy-back groups (`00 a 35`, `85 ... 11/10`) instead of sector read + program |
| `sdrw`: `sSDF641_BlkClose` / `sSD_Update` | 0x1F92-0x2037 (close), 0x1B32 (update = close + save table) | **same idea, reworked**: swap temp and data unit in the map, copy back the tail, erase, plus table save (0x1C8A) |
| `sdrw`: `BadBlockManage_` | 0x203B-0x2062 | **same logic**: reset chip, program the spare of the failed unit with 0xFFFE, take the next unused entry from the end of the free list (the binary marks a taken entry with 0x1FFF in the high byte) |
| `sdrw`: `sSD_WordRead`/`DwordRead` | none | not needed (BREC reads table words through `FtblWordRead`) |
| `usbdisk51kernel` (UdiskInit + UdiskKernel + state machine) | none | **not in BREC** (stock USBDISK.AP engine) |
| `usbsuspend` (CTC idle timer, suspend/resume) | none | **not in BREC** |

Binary routines without a vendor counterpart (about half of the code): firmware check and its A/B logic
0x0600-0x09C7, boot-record self repair 0x09C8-0x0C57, syscfg loader 0x0216-0x0374, FTL mount 0x2075-0x2660,
LCD/hourglass 0x3000-0x30FF with the init script 0x3100 and bitmap 0x3161, two-plane erase 0x1910.

## 3. The parts that are not in the folder, as read from the binary

These are the routines the vendor header declares `extern`. (A local annotated, re-assemblable
reconstruction of the whole binary exists outside the repository; it is derived from the vendor binary and is
deliberately not published.)

* **`sBREC_SystemTstRpr` (0x1B12)**: (1) `lcd_hourglass` 0x3000; (2) `flash_detect` 0x150D; (3) boot-record
  self check 0x0A36 (nonzero -> ADFU path); (4) FTL mount 0x2075 with DE=0, must return 1; (5) firmware check 0x0600.
  Any failure jumps to **0x2063**, which copies 0xC00 bytes of the mask ROM from 0x9000 (the ADFU recovery code)
  to RAM 0x0000 and runs it with SP=0x100.
* **Boot-record self check (0x0A36, helpers 0x09C8 read 16 KB image to the 0x4000 window, 0x09FF program,
  0x0B13 verify, 0x0B65/0x0B9D rewrite, 0x0BEB move mirror)**: reads the BREC at block 0 and verifies it exactly
  like the mask ROM (0x55AA at +0x3FFC, 16-bit word sum at +0x3FFE); reads the mirror copy at the block named by
  the image tail word at 0x3FFA, compares the first 0x40 bytes, and when something is wrong erases and
  reprograms the damaged copy (up to 4 tries; block 0 bad -> tries blocks 1..3, i.e. rows 0x40/0x80/0xC0).
  The tail holds the block number of the mirror three times (this is the vendor symbol `MirBRECBlkAddr`,
  currently 1). So BREC maintains its own redundancy on every boot, and it can write NAND at boot.
* **Firmware check (0x0600 and helpers)**: header at logical sector 0 and a second copy at +0x4000 sectors
  (sector 0x4000 = 8 MB into the firmware space; the unit number is the sector shifted right by 9); each header: magic `55 AA F0 0F`, 16-bit word sum of the first
  510 bytes vs the word at +0x1FE, then the 15 directory sectors summed as 32-bit words and compared with the
  header (+0x10), then every directory entry except `$$$` and `R?S` (.RES) summed over its sectors and compared with
  the entry's stored sum (entry +0x1C). Header A bad -> check B and copy B over A; headers differ -> **wipe the VM
  (settings) area, 128 sectors from logical sector 0x8000 filled with 0xFF**, then copy A over B. The copies are
  refused when there is neither USB power (port 0x50 bit 6) nor battery code >= 3 (port 0xD0 low nibble), so a
  repair never starts on a nearly empty cell. Watchdog (port 0x4E |= 0x08) is kicked throughout.
* **Syscfg launcher (0x0216/0x023A)**: reads 32-byte directory entries of the firmware image with the simple
  reader, compares the name with `SYSCFG  SYS`, then loads 0x2C00+0x100 bytes of the file to RAM 0x0600.. and
  the 0x80 bytes at file offset 0x3700 to RAM 0x0000 (the vector page); returns 1 when loaded. The file's start
  is a sector number in entry +0x10 (shifted left by 9 to a byte offset).
* **FTL mount (0x2075...)**: per chip in the CE mask (CE1, CE2) and per zone: set up work windows (0x6000 =
  new table, 0x7000 = free list, both 0xFF-filled), scan the spare bytes of both planes of every unit
  (bad = byte0 not FF; table unit = byte1 0xBB; leftover/duplicate units are erased; otherwise the logical
  number from bytes 2-3), resolve duplicates, allocate the temp unit and the table unit (failures are marked
  0xFFFE/0x0800 and retried), finalise the table (free list at the end, signature 0x55AA at the last word, checksum
  before it) and write it as a new copy (4 pages with marker 0xBB). This confirms `nand_ftl.md` 2.4.
* **FTL runtime** (0x1B39-0x2062): used by the firmware check for copies/wipes; matches the `sdrw` shape (section 2).
* **LCD/hourglass (0x3000)**: as in `boot.md` 2/2.1; the source gives no information about it.

## 4. Concrete differences, grouped

Source features not in our binary: small-block flash paths (4 address cycles, pointer commands 0x00/0x01/0x50,
sequential spare reads for ECC); the WordRead/DwordRead pair and `sSD_Word/DwordRead`; the `TestEcc`
erase-on-page-0 test; a `debug` loop in the erase file; the 48 MHz delay switch (port 0x70 bit 3, 60 vs 128
loops) in the sector read; the sub-sector read path (commented out); a copy-back-by-read-and-program fill; a
flat single-zone linear layer with a 4 M-sector range check; `Sdf641` entry switch; the whole USB kernel and suspend
code; Toshiba-only small-block table entries are present in both, but with different numbers.

Binary features not in the source: large-page protocol everywhere (5 address cycles, 30 read confirm,
random data out/in 05/E0/85), two-plane erase and copy-back, zones with 2048 units each and 0x7B0 logical units,
the mount, table save, the boot-record self repair, the firmware A/B check and VM wipe, per-file sums, the
battery/USB guard on repairs, the LCD splash, the ADFU escape, makers 0x2C and 0xC1 in the ID table.

Constants that differ: DMA mode 0xC0 comes from `WaitState`=3 (`ld a,3; rrca; rrca`); read tR wait 0x40 loops
(the 25 us of the vendor `WordRead` wait, not the 60 of its `SectorRead`); table window 32 bytes (source 64);
EC D3 geometry 0x7B0 x zones (source 0x1080).

Build date: the header's year bytes are the constant 0x2004, month/day come from the assembler date macro; our
binary says 03/15, so it was assembled on a 15 March (2004 by the constant), while the CVS import of the folder is
2005-10-26 and the individual files are older revisions (some still named for the 2002 BREC). The folder is therefore
a neighbouring revision, not the exact tree of this binary.

## 5. What the source teaches us that our notes lacked

**Layering and names** (useful when reading the binary): `sNAF_Enable/Disable/Reset/ReadID/ReadStatus` and
`sNAF_IssueCMD3/4` (command byte + 3 or 4 address bytes) are the lowest NAND layer; `sNAF641_*` the page layer;
`sSD_*` the system-disk (firmware space) layer; `sSTG_*` the storage scan; `sBREC_*` the boot extras. In the binary:
Enable 0x3D3A, Disable 0x3D5F, IssueCMD2..5 0x3D7E/83/88/8D, ReadStatus 0x1ACA, Reset 0x1AFA, ReadID 0x18F7.
`MirBRECBlkAddr` = the mirror block word in the BREC tail; `StorageInfor` = the 24 bytes at 0x0080.

**F641 command sequences** (binary-verified, source-corroborated):
* read page: `00`, 5 address cycles (col lo, col hi, row 0..2), `30`; wait ~25 us (0x40 `djnz` loops, **no busy
  poll for reads**); ECC engine started (port 0xCC = 0x10) just before the data are clocked; the stored ECC is
  fetched with random data out (`05`, 2 column bytes, `E0`) as 4 bytes at spare offset +8 of the sector group.
* program sector: `80`, 5 address cycles, ECC start, 512 data bytes (DMA1), `85` + 2 column bytes to the
  spare group, 16 spare bytes (FF FF lun-lo lun-hi FF FF FF FF ECC x4 FF FF 55 FF), `10`, status.
* program spare word(s) only: `80`, address with the spare column, 4 bytes, `10`, status (used for the 0xFFFE bad mark).
* erase: `60` + 3 row bytes (twice with the plane bit for two planes), `D0`, status.
* status: `70`, poll bit 6 (timeout 0x7000 polls then reset), bit 0 = failed, bit 7 = not write-protected
  (return codes 0 ok / 2 write protected / 3 failed or timeout); reset = `FF`, ready wait, `37`.
* read ID: `90`, ~0x60-loop delay, four reads from the window.
* the sequencer is entered by writing the command to port 0x2A; `37` stops it; CE select = port 0x02 (CE*8),
  port 0x29 = 1, port 0x28 = 1<<CE, bus phase through port 0x01 (0 data, 1 CLE, 2 ALE).
* ECC check: after the wait for port 0xCC bit 4 to clear, xor the four result ports with the stored bytes;
  zero = ok; the all-FF stored value means "no ECC written, accept"; exactly the pattern where the xor of the two
  halves is all ones except the masked bits (the vendor comment: eleven bits set) = single-bit error, corrected
  in place from the column/line fields; anything else is left uncorrected (not an error).
* bad blocks: chip detection does not look at them. A unit that fails an erase gets 0xFFFE in its spare and a
  spare unit from the end of the free list replaces it (taken entries are marked); the mount treats byte 0 != FF as bad.

**Ports/constants confirmed or added**: port 0xDF bit 0 selects the DC-DC clock mode (set = 200 kHz at 75 %
duty; the vendor source keeps the 133 kHz/50 % alternative commented out) and BREC sets it first thing; port 0xCC
write 0x10 = ECC start, read bit 4 = busy; ports 0xCD/0xCE/0xCF ECC results; port 0x70 bit 3 = 48 MHz mode flag
(only read by the source); port 0x4E |= 0x08 clears the watchdog; port 0xD0 low nibble battery code (3 = threshold
for repair/copy); port 0x50 bit 6 VBUS. DMA1 registers 0x06..0x13 as in `regs.h`; the DMA source byte 3 = 0x80 |
(CE port bits 5..3 >> 1) for external NAND, the lane is chosen from the destination (zero = ZRAM, 0x40 = DSP RAM lane).

**The stock USB disk kernel (`usbdisk51kernel` + `usbsuspend`), cross-check for zos's MSC** (our `usb_msc.md` was
derived from the AP binary and agrees on everything checkable; the source adds names and a few facts):
* ISR dispatch order on port 0x52: bit 7 bus reset, bit 5 SETUP, bit 6 connect change, bit 4 wake-up; then port
  0x53 bit 3 (EP1 OUT = CBW). The ISR saves port 0x05 and SP and leaves through a common exit that restores both,
  so deep waits can abandon the handler on reset/unplug by jumping to that exit (a longjmp pattern: every wait
  loop checks bus reset (0x52.7) and VBUS (0x50.6)).
* Reset handling: clear the software state, write 0x80 to port 0x51 (SIE on, address 0), walk the 8 endpoint
  indices (port 0x60 = n, port 0x64 = 0) until port 0x68 reads 0, then acknowledge everything with 0xFF to 0x52,
  0x53, 0x69, 0x6A.
* SETUP: accept only when `0x57 & 0xDF == 0x58`, else stall both directions (0x61 = 1, 0x66 = 0xC7).
* EP1 OUT: set port 0x05 = 0xF7 (ZRAM2) and `0x70 |= 0x30` (both USB RAM banks to the CPU), check "USBC"
  at the buffer start, copy tag/length/LUN, dispatch on the first CDB byte: 00 TEST UNIT READY, 28 READ(10), 2A
  WRITE(10), 1E PREVENT MEDIUM REMOVAL, 25 READ CAPACITY, 23 READ FORMAT CAPACITIES, 12 INQUIRY, 5A MODE SENSE(10),
  55 MODE SELECT(10), 03 REQUEST SENSE, 2F VERIFY, 1B START STOP UNIT, 1A MODE SENSE(6), plus vendor CB (switch to
  ADFU) and CC (are-you-Actions). Anything else: status 1 and the data phase padded.
* READ/WRITE(10): chunks of 4 sectors (2 KB) in a ping-pong: the CPU owns one RAM bank (port 0x70 bits 5/4 swap
  X/Y ownership), the SIE the other; count regs 0x64/0x65 and mode regs 0x61/0x66 (0x3F = arm), completion by port 0x68
  (bit 3 EP1 OUT busy, bit 4 EP2 IN busy). The first write chunk is received before any flash write starts, the last chunk
  is programmed after the final receive; the CSW is sent after the last program; a write-protect flag turns
  the CSW into an error (status 2). TEST UNIT READY does not count as activity for the idle counters.
* Connect: `UDiskRun` refuses to start when VBUS is absent or D+ and D- are both 1 (shorted/charger), then clears
  the USB IRQ flags, enables the USB interrupt (0x27 bit 2) and only then pulls D+ up (port 0x3E bit 7).
* State machine (`UDiskHandle`, 0 idle, 0x10 reading, 0x20 writing, 0x30 PC eject, 0x40 cable pulled, 0x50/0x51
  switch to ADFU, 0x60 partition eject, 0x70 ..., 0x80 suspend, 0x90 leave suspend): idle for 30 x 3 ms
  (threshold `3000/CTCTimer*30` = 180 ticks of the 0.5 ms CTC, the 0xB4 in `usb_msc.md`) -> flush the open write unit,
  save the IRQ mask, disable all interrupts, enter suspend; 2 Hz timers drive the 0x10/0x20 "stuck" timeouts; "switch
  to ADFU" pulls D+ low (port 0x3E bit 7 = 0) and waits 3 s so the PC re-enumerates.
* Suspend (`usbsuspend`): CTC idle timer = ports 0x22/0x23 = 0, 0x24 = 0x30 (0.5 ms), enable bit 4 of 0x27; its ISR
  acks with 0x80 to 0x25 and counts idle ticks when port 0x50 bit 3 (bus active, W1C: write 0x08) stays 0 and D+/D-
  bits are not both 0. Suspend itself: stop the CTC (0x22 &= 0x7F), 0x50 = 0x80 (transceiver suspend), clock to
  the 32 kHz oscillator (0x00 = 0), ~1024-cycle wait, HOSC off (0x42 = 0), then loop feeding the watchdog until VBUS
  falls (resume or detach), then 0x42 = 0xE0, 0x00 = 0x10, CTC on again.
* Write-back: the state machine flushes the open unit (`UD_Update`) at suspend and exit; reading mirrors it.

## 6. What this means for our own BREC / the boot selector (plan sections 2 and 4)

1. **The "free space" in the 16 KB image is larger than the plan says, but it is RAM.** Because BREC runs
   from the same 0x0000-0x3FFF that BROM loads, the 0x76 filler between its routines is where BREC keeps its
   variables. Measured (static references): unreferenced and free for added code or data: **0x2661-0x2FFF
   (2 463 bytes)**, **0x3800-0x3A7F (640)**, 0x12A8-0x14AB (about 516, the area after the fw-copy sector buffer),
   0x03F8-0x05FF (520, under the kernel entry), plus the zero area 0x0098-0x01FF that is also the BREC stack
   (SP starts at 0x0200: keep the top 0x80). The 1 440-byte hourglass bitmap and the init script are
   replaceable (brecpatch already does that). So a patch has about 3.6 KB of safe room, not 2 KB.
2. **Self repair interacts with patching.** The check at 0x0A36 re-verifies block 0 with the BROM rule and compares
   only the first 0x40 bytes with the mirror; a patched BREC must therefore keep the tail intact (0x55AA, the word
   sum, the mirror block number) and be written to **both** copies, otherwise BREC may "repair" one over the other
   on a boot where one checks bad. Conversely, if a patched copy fails its sum, the stock code will rewrite it from the
   mirror, i.e. patches can silently disappear (and the device writes NAND at that boot).
3. **Where to hook for an early key check / selector**: the earliest point is `brec_start` (0x0200) before the call
   to 0x1B12, but the LCD is only initialised in 0x3000 (first thing in 0x1B12); a key poll after the LCD init and
   before flash detect (0x1B15) costs only the 0.9 s hourglass init, a poll before it costs nothing but has no screen.
   `brecpatch --fast` already trims the waits. The vendor layering shows the cheapest hand-over points: the
   `SYSCFG` launcher (0x0216) is the only consumer of the firmware reader besides the check, so a selector that
   replaces `syscfglauncher`'s target file (as `zosboot` does) needs no knowledge of the lower layers.
4. **Contract the kernel expects from BREC** (unchanged from `new_brec_plan.md`, now with the variable names):
   `StorageInfor` at 0x0080 (CE mask, type, capacity link table), `units_per_zone`/`logical units` at 0x3CA8/0x3CAA,
   the FTL descriptors at 0x3C80 (copy index + table unit per zone), the table in the 0x4000 window pages, the
   sector cache at 0x3A80, the table-word cache at 0x3F40.., and the firmware reader entry 0x3FDB. A from-scratch BREC must
   provide the same state or the stock kernel will not mount the disks.
5. **For writing our own NAND code** the confirmed sequences in section 5 are enough for read, program, erase and
   status; the two-plane/copy-back sequences are already in `nand_ftl.md` and `zos/modules/common/ftlw.inc`, and they
   agree with the binary's helpers (0x1910, 0x193A, 0x1978-0x1A9F).
6. **What not to copy**: the vendor `TestEcc` erase-on-page-0 behaviour, the 4 M-sector range check, and anything
   small-page.

## 7. Open points

* The exact rule behind the boot-record mirror compare (first 0x40 bytes only) and the retry counters
  (0x3FF6/0x3FF8/0x3FFA) was read but not exercised in the emulator; the branch for a bad block-0 copy (which
  blocks it falls back to and what it rewrites) is inferred from the data flow.
* The semantic names in the reconstruction for the table-building helpers (0x2146-0x2660) are inferred from
  their data flow; the mount behaviour is unchanged from `nand_ftl.md` 2.4.
* The unit-state markers of the small-page `sdrw` (0xAAFF / 0xAFFF in the spare) have no direct counterpart in the
  binary; the binary uses the spare byte 1 (0xBB table) and 0xFFFE (bad) only.
