# ATJ2085 NAND controller and the stock "641" FTL (LISONG LSD_501_04 / my-ximpro)

Scope: what a new OS (zos) needs in order to read, and later write, the player's NAND without
ACTOS. Device: Samsung K9K8G08-class NAND on **CE1**, READ ID `EC D3 51 95`, 2048 + 64 byte pages,
64 pages/block, 8192 blocks (1 GB). Firmware: BRECF641.BIN (boot record) + UD_SF641.DRV (user disk driver).

Tags: **[code]** = read in the firmware (address given), **[src]** = Actions' own BREC low-level sources
(`s1mp3/Chinese Files/BREC_lowlevel_source.7z`: 641Read.asm, 641WordProgram.asm, 641BlockErase.asm,
ScanNAF.asm, sdrw.asm; an older SLC variant, same register usage), **[wiRe]** = s1clone/FlashIO.cpp,
**[PG]** = ATJ2085X programming guide (digest in `atj2085_hw.md`), **[dump]** = measured on
`readback-my-ximpro.s1clone`, **[emu]** = observed with the hardware NAND model in `atj2085emu --nand`
(real BREC + UD_SF641 code running), **[inf]** = inference.

Addresses: BREC = BRECF641.BIN loaded at 0x0000 (identical to dump block 0 pages 0-7, copy in block 1
[dump]); UD = UD_SF641.DRV resident code at 0x3300 (disassembly `drivers/disasm/UD_SF641_DRV`, partly
still `.db` - the program routine 0x37C9 and the 0x3536-0x361A dispatcher are hand-decoded below).

Tools: `tools/s1clone.py` (container), `tools/ftlread.py` (reference FTL reader),
`atj2085emu/emu/nand.py` (hardware model).

---------------------------------------------------------------------------------------------------
## 1. Controller, register level

### 1.1 Ports
| port | use (as the firmware uses it) | evidence |
|---|---|---|
| 0x01 EM_PAGE_LO | external-window page = EMA22..15. **NAND bus phase**: 1 = every window write is a CLE (command) cycle, 2 = ALE (address) cycle, 0 = data cycles | [code] BREC 0x3D9A, UD 0x37AF; [src] sNAF_WriteCMD; [wiRe] |
| 0x02 EM_PAGE_HI | bits 5-3 = EMA28..26 = **chip enable**: 0x08 = CE1 (boot NAND), 0x10 CE2, 0x18 CE3 (LCD), 0x00 = CE0 (boot ROM) | [code] BREC 0x3D52 (`out (2),e*8`) |
| 0x28 NAND_ENABLE | write `1<<CE` to enable CLE/ALE on that CE; 0 = off. Read bit 0 = sequencer busy (never polled by this firmware) | [code] BREC 0x3D58-0x3D5C, 0x3D65; [PG] |
| 0x29 CE mode | written 1 (= Samsung NAND, "old" CE mode) on select, 0 on release | [code] BREC 0x3D56, 0x3D67 |
| 0x2A NAND_CMD | hardware sequencer: writing a command emits one CLE cycle with that byte. 0x90 also emits one ALE cycle (address from 0x2B, left 0). **0x37 = stop sequencer, release CE (no bus cycle)** | [code] BREC 0x18FD (0x90), 0x1AD2 (0x70), 0x3E48 (0x30), 0x1AFC (0xFF), 0x3D62 (0x37); [PG] |
| 0x2B-0x2D, 0xEC | sequencer address registers; **not used** by this firmware (it sends addresses by hand) | [code] no writes |
| 0xCC | ECC control: write 0x10 = start; bit 4 reads 1 while busy | [code] BREC 0x3E4C, 0x3EC5; UD 0x37D5 |
| 0xCD, 0xCC, 0xCF, 0xCE | ECC result (see 1.5) | [code] BREC 0x3EC5-0x3ED8 |
| 0x06-0x13 | DMA1 (see 1.4) | [code] BREC 0x3E92-0x3E9E |
| 0x05 | internal page register, only for DMA destinations in DSP RAM / ZRAM2 | [code] BREC 0x3E7A |
| 0x70 bit 3 | read by Actions' 641Read.asm to choose a longer tR delay (48 MHz) | [src] |

The data address inside 0x8000-0xFFFF is irrelevant (no NAND address pins); the firmware uses 0x8000,
0x8002, 0x8123, 0x81xx. While a NAND CE is selected the window is **not** RAM/ROM: do not call the mask-ROM
C runtime (0x8800-0x8E77) or touch 0x8000+ data in that state [code: all NAND routines run `di`].

### 1.2 Select / release (BREC 0x3D3A / 0x3D5F, resident, also used by UD_SF641)
```
select(CE=e):  di ; inc (0x0037)              ; kernel irq-nesting counter
               save in(0x01), in(0x02)
               out (0x01),0 ; out (0x02),e*8 ; out (0x29),1 ; out (0x28),1<<e
release:       out (0x2A),0x37 ; out (0x28),0 ; out (0x29),0
               restore 0x01/0x02 ; dec (0x0037) ; ei if it reached 0
```
### 1.3 Command / address / data cycles, busy wait
```
CLE c:     out (0x01),0 ; out (0x01),1 ; ld (0x80xx),c          (BREC 0x3D9A, UD 0x37AF)
ALE a..:   out (0x01),2 ; ld (0x80xx),a  (one write per address byte)
data:      out (0x01),0 ; ld a,(0x8xxx) / ld (0x8xxx),a  (or DMA, 1.4)
```
Every routine first writes `out (0x2A),0x70` (status command) as a "known state" [code BREC 0x3D9C].
**Ready wait** (BREC 0x1ACA = UD 0x39FB): ~0x60-iteration delay; `out (0x2A),0x70`; poll
`ld a,(0x8002)` until bit 6 (ready), timeout ~0x7000 polls -> `out (0x2A),0xFF`, return 3; then read the
status once more: bit 0 = fail -> reset (0xFF) and return 3; bit 7 = 0 (write protected) -> return 2;
else 0. Reset = `out (0x2A),0xFF` + ready wait + `out (0x2A),0x37` (BREC 0x1AFA). Read ID =
`out (0x2A),0x90`, ~0x60 delay, 4 data reads -> B=maker C=device D E (BREC 0x18F7).

Address cycles (Samsung 2 KB page, 5 cycles): `col_lo, col_hi, row0, row1, row2`,
column 0..0x83F (0x800+ = spare), row = physical page number = block*64 + page.

### 1.4 Page / sector read (BREC 0x3E30, "read one 512-byte sector + check ECC")
Input: 8-byte struct `{col_lo, col_hi, row0, row1, row2, dest_lo, dest_hi, sel}`; sel = 8 -> ZRAM
(dest < 0x4000 or 0x8000+ RAM), 0-2/4-6 -> DSP IPM/IDM lane (DMA DA3 bit 6, port 05 lane), 7 -> ZRAM2.
```
 select(1)                                   ; 0x3D3A
 out (0x2A),0x70
 CLE 0x00 ; ALE col_lo,col_hi,row0,row1,row2 ; out (0x01),0      (0x3D8D)
 out (0x2A),0x30                             ; read confirm (tR starts)
 out (0xCC),0x10                             ; ECC start (counts the next 512 data bytes)
 delay ~0x40 djnz                            ; tR (25 us) - no busy poll for reads!
 DMA1: out 07,00  out 08,00  out 09,0x80|(CE<<2)   ; SA = external, CE1 -> 0x84
       out 0A,00  out 0B,dest_lo out 0C,dest_hi out 0D,00
       out 0E,0x00 (ZRAM) | 0x40 (DSP lane)  out 0F,sel
       out 10,0xFF out 11,0x01               ; count-1 = 0x1FF (512 bytes)
       out 12,0xC0                           ; mode: 3 wait states
       out 13,0x01                           ; start (CPU halts until done)
 CLE 0x05 ; ALE (0x08+16*s) , 0x08 ; CLE 0xE0 ; out (0x01),0   ; random data out -> spare+8 of sector s
 ld bc,(0x8123) ; ld de,(0x8123)             ; 4 stored ECC bytes: C=+8 B=+9 E=+10 D=+11
 wait in(0xCC) bit4 = 0
 B^=in(CC) C^=in(CD) H=D^in(CE) L=E^in(CF)
 ok if (L|C)==0 and ((B|H)&0xE1)==0; spare ECC = FF FF (+10,+11) -> no ECC, accept;
 single-bit error -> flip bit (0x3F03); release
```
DMA register order is literally an `outi` loop over the table at BREC 0x3DBC (ports 0x07..0x13;
SA0 port 0x06 is not written). DMA from external memory: SA3 bit 7 = external, bits 4-0 = SA[28:24]
-> CE = bits 4-2; SA bits 16/15 select data/CLE/ALE as port 0x01 does for the CPU (SA1 = SA2 = 0 -> data).

**Minimal zos read of one 2048-byte page + 64 spare into ZRAM1 (PIO, no ECC check)**, page number P
(row), buffer at HL (0x840 bytes, must be < 0x8000):
```
    di
    xor a ; out (0x01),a
    ld a,0x08 ; out (0x02),a          ; CE1
    ld a,0x01 ; out (0x29),a
    ld a,0x02 ; out (0x28),a          ; enable CE1
    ld a,0x70 ; out (0x2A),a
    ld a,0x01 ; out (0x01),a ; xor a ; ld (0x8000),a        ; CLE 00
    ld a,0x02 ; out (0x01),a
    xor a ; ld (0x8000),a ; ld (0x8000),a                   ; column 0x0000
    ld a,P0 ; ld (0x8000),a ; ld a,P1 ; ld (0x8000),a ; ld a,P2 ; ld (0x8000),a   ; row
    xor a ; out (0x01),a
    ld a,0x30 ; out (0x2A),a                                ; read confirm
    ld b,0 ; djnz $                                         ; >= 25 us (BREC uses 0x40..0x80 loops)
    ; PIO: 0x840 data reads (or DMA 512-byte chunks as in 1.4, 4 times + the spare by PIO)
    ld de,0x840
1:  ld a,(0x8000) ; ld (hl),a ; inc hl ; dec de ; ld a,d ; or e ; jr nz,1b
    ld a,0x37 ; out (0x2A),a ; xor a ; out (0x28),a ; out (0x29),a
    out (0x02),a ; out (0x01),a                              ; (restore what was there before)
    ei
```
A DMA variant: after `out (0x2A),0x30` + delay program DMA1 exactly as in 1.4 with BC=0x7FF (2048
bytes; BC is 15 bit, max 32 KB) and read the 64 spare bytes by PIO afterwards (the column pointer
continues at 0x800). Use the ECC engine per 512 bytes if wanted (start before each 512-byte chunk).

### 1.5 Hardware ECC
24-bit Hamming per 512 bytes [PG CEh 4:3 = 10 = 512 B]. The firmware stores and compares the
registers in this byte order; spare bytes +8..+11 of the sector's 16-byte spare group:
`+8 = CD, +9 = CC, +10 = CF, +11 = CE`, i.e. (verified [dump]: 1000/1000 random sectors match;
same formula as wiRe's `IFlashIO::calcECC`):
```
odd  = XOR of bit addresses (8*byte + bit, bit 0 = LSB) of all 1 bits in the 512 bytes   (12 bits)
even = odd ^ 0xFFF if the number of 1 bits is odd, else odd
+8 = odd>>3 & 0xFF            +9  = (odd&7)<<5  | (odd>>11)&7
+10 = even>>3 & 0xFF          +11 = (even&7)<<5 | (even>>11)&7 | 0x10   (0x10 = block type 512)
```
Program side: the controller computes it while DMA writes the 512 bytes (UD 0x37D5, 0x3869-0x3879).

### 1.6 Init (BREC 0x1B12, called from 0x0209; ~105 M T-states in the emulator)
1. `0x3000` LCD controller reset/init + hourglass (CE3 = 0x18, ports EE/F3) [code].
2. `0x150D` flash detect = Actions `sSTG_ScanObNAF`/`ScanSMC` [src ScanNAF.asm, code 0x1514/0x156B]:
   for CE1 then CEs 6,5,2,1 (skipping 4 = SMC card, 3 = LCD): select, reset (0x1AFA; returns 3 if no
   chip: no ready bit or fail bit), READ ID (0x18F7), table by maker/device (0x15A3..0x1768):
   EC/AD/20 D3 -> family 1 (=`*641`), logical units per zone **0x7B0** (`0x3CAA`), `0x3CA8 = 0x0800`
   (`0x3CA9 = 8`: 2048 units = 4096 blocks per zone), capacity 0x1EC0 (in 128 KB "physical-block" units).
   Results ("StorageInfor", RAM 0x0080, 24 bytes) read by the kernel/drivers:
   `0x0080` = bitmask of CEs with NAND (0x02), `0x0081` = family (1), `0x0088+2*CE` = capacity
   (`0x008A = 0x1EC0`), `0x0082/0x0083/0x0090` = SMC card on CE4 [emu: `02 01 00.. c0 1e`].
3. `0x0A36` boot-record self check/repair (see brec_source_vs_binary.md) ; `0x2075(DE=0)` FTL mount (zone scan,
   table load/save, see 2.4) ; `0x0600` (BREC code).
   Any failure -> 0x2063 (error path; not followed).
4. Then BREC reads SYSCFG.SYS through its FW reader (0x3FDB -> 0x3CAF) and jumps to 0x0600.
Note: RAM 0x0037 is 0x76 (BREC filler) during BREC; SYSCFG's vector page later sets it to 1, so the
emulator's "irq counter" fix (set 0 at SYSCFG start) is still needed with the real init [emu].

---------------------------------------------------------------------------------------------------
## 2. On-flash format

### 2.1 Geometry and "units"
* **unit** = 2 physical blocks (2n, 2n+1) = 128 **superpages** of 2 KB = 256 KB = 512 sectors. The two
  blocks are the two planes (block bit 0 = plane); superpage sp lives in block `2n + (sp & 1)`,
  page `sp >> 1` [code BREC 0x3F1B]. Sector s of a unit (0..511): `sp = s >> 2`, column `(s & 3)*512`.
  BREC 0x3F1B builds the 5 address bytes from (HL = unit, BC = sector):
  `col_lo = 0, col_hi = (s&3)*2, row0 = (unit&1)<<7 | (sp&1)<<6 | sp>>1, row1 = unit>>1 & 0xFF, row2 = unit>>9`
  (= row `(2*unit + (sp&1))*64 + (sp>>1)`).
* **zones**: 2048 units (4096 blocks); this chip has 2 zones. A zone holds **0x7B0 logical units**
  (L = 0..0x7AF); the rest are spare/free units, the temp unit and the table unit [code 0x16FB, dump].
* global logical unit **U**: zone `U / 0x7B0`, L = `U % 0x7B0` (UD 0x3717 divides by `(0x3CAA)`);
  physical unit = `zone*2048 + table_zone[L]` (UD 0x370F: `H += zone << 3`).
* Physical blocks 0/1 = BREC (pages 0-7, 16 KB each). Bad blocks: spare byte 0 = 0x00 on all pages
  (9 such blocks in the dump: 0x451 0x6A1 0xC82 0x12FC 0x131F 0x1350 0x137C 0x19AA 0x1AF7).

### 2.2 Spare area (64 bytes = 4 groups of 16, group g belongs to sector g of the page)
| byte | content | evidence |
|---|---|---|
| 0 | 0xFF good; 0x00 = bad block (factory/Samsung convention) | [dump] |
| 1 | 0xFF data unit; **0xBB = mapping-table unit** | [dump], [code] BREC 0x2355 `cp 0xBB`, 0x25FB |
| 2-3 | zone-relative **logical unit number L** (LE); FFFF = none | [dump: 0 mismatches vs. table] |
| 4-7 | 0xFF | [code] UD 0x3861 writes FF x4 |
| 8-11 | ECC of the 512 data bytes (1.5) | [code], [dump] |
| 12-13 | 0xFF | UD 0x387C |
| 14 | **0x55** | UD 0x3884 |
| 15 | 0xFF | UD 0x3889 |
Every written sector carries the full 16 bytes (logical number repeated in all groups/pages).
BREC's scan reads bytes 0-7 of page 0 of both planes of every unit (0x2304-0x233F, 8-byte reads into
0x2115/0x211D).

### 2.3 Logical address spaces
* **Firmware ("system disk") space**: byte offset X -> U = X >> 18 (BREC 0x3CAF..0x3CDE), zone 0 only.
  U 0..0x3F = 16 MB = FWIMAGE.FW (**identical** to the FWIMAGE.FW in my-ximpro.fw, verified with
  `ftlread.py export-fw --compare`). X >= 16 MB = the VM area (= UD unit 0).
* **UD (user disk) space**: 512-byte sector S -> U = 0x40 + (S >> 9) (UD 0x362C `add 0x40`), sector S & 511.
  UD sectors 0..511 = **VM (settings)** (unit 0x40, physical unit 0x4C6 = blocks 0x98C/0x98D; only the
  first 128 sectors written), UD sector **0x200** = start of the FAT volume (kernel `DRV_ActiveDrive`,
  SYSBNKB 0x4872). UD capacity = (2*0x7B0 - 0x40) units = 1 982 464 sectors (968 MB).
* The FAT volume in the dump is a **superfloppy FAT16** (no MBR): boot sector at UD 0x200, 1 981 937
  sectors, 32 sectors/cluster, 2 FATs x 242 sectors, 512 root entries, 61 919 clusters, label "NO NAME".
  `ftlread.py fatcheck`: 1084 files / 63 dirs / 850 MB, no lost or cross-linked clusters, FAT copies equal.

### 2.4 Mapping table ("FTBL") and mount
* One 0xBB unit per zone (zone 0: unit 0x050 = block 0xA0; zone 1: unit 0x049 = block 0x1092). Only the
  even (plane-0) block is used. A table copy = 2 pages = 2048 u16 (LE):
  `[0..0x7AF]` = physical unit (zone-relative) of logical unit L (0xFFFF = unmapped);
  `[0x7B0..0x7FC]` = free/spare unit list (0xFFFF = none); `[0x7FD]` = **temp unit** (zone 0: 0x070,
  zone 1: 0x435); `[0x7FE]` = unknown (zone 0: 0x39D8 - not a unit, maybe a counter; zone 1: 0x577);
  `[0x7FF]` = 0x55AA signature [dump; BREC 0x1B4E stores 0x4FFA/0x4FFC = entries 0x7FD/0x7FE].
* Copies are appended in page pairs 2k/2k+1 of the table block; the RAM descriptor
  **`0x3C80 + 2*zone` = (k << 12) | table_unit** [emu: after init `0x3C80 = 50 10 49 10` = copy 1 of units
  0x050 / 0x049]; readers (BREC 0x3CF9, UD 0x372F) address entry L at column `2L & 0x7FF` of page
  `2k + (2L >> 11)` of the table block, via the cached word reader BREC 0x3F69 (32-byte chunks at 0x3F48).
* **Mount (BREC 0x2075 -> 0x20B8 -> 0x2146 -> scan 0x22E1, every boot)**: scans the spare of all units of all zones (~8200 spare reads
  [emu]) - code 0x22F3-0x2406: byte0 != FF -> bad; byte1 = 0xBB -> table unit (a second 0xBB unit is
  erased and freed); other non-FF markers -> erase + free; FF -> data unit with logical number in
  bytes 2-3. It then erases the temp unit of each zone and writes the table as a new copy (k+1) into
  the table block [emu: erase 0xE0/0xE1 and 0x186A/0x186B, program pages 2-3 of 0xA0 and 0x1092; the new
  copy is identical to the old]. What happens when the table block is full (k = 16) or when two data
  units claim the same L is **not established** (code 0x23A4.. 0x2600, not read in detail).
* **Read path never consults the spare at run time**: firmware reads go BREC 0x3CAF -> 0x3CF9 (table
  lookup via 0x3F69) -> 0x3F1B -> 0x3DC9/0x3E30; UD reads go UD 0x34A0 (`A=0`) -> 0x33FE -> 0x362C (zone
  split) / 0x3677 (table) -> 0x3F1B -> 0x3E30. A zos reader therefore only needs: geometry, the table
  (or a spare scan, which gives the same map: 0 mismatches in the dump), and the address formula.

---------------------------------------------------------------------------------------------------
## 3. Write path (stock algorithm; zos implementation in 3.1)

UD_SF641 RST 18h entry 0x35E3: `DE -> {u32 sector; u8; u16 buf/offset; u8 sel}` copied to 0x361A;
A = 0 read, 1 write, 2 word read, 3 dword read, 4 update/flush (0x35FE-0x3616) [code]. The kernel's
VM writes (KCFG/MAIN settings) use the same entry (sectors 0/1) [emu].

Observed sequence for one sector write + flush [emu, `NANDLOG=1`], unit L=0x40 (phys 0x4C6), temp 0x070:
1. **Open** (first write to a unit; UD 0x3312-0x33D4 [inf: entry logic not fully decoded]): temp unit =
   table[0x7FD] of the zone (temp in zone 0 = 0x070, blocks 0xE0/0xE1 [emu]). The sector is programmed
   into the **same sector offset of the temp unit** with UD 0x37C9 (hand-decoded): `CLE 80, ALE 5
   (col (s&3)*512, row), out (0xCC),0x10, DMA1 512 bytes ZRAM->NAND (DA3 = 0x80|CE<<2, mode 0xC0,
   BC 0x1FF), CLE 85 (random data input), ALE 0x10*(s&3), 0x08 (= column 0x800+16*(s&3)), data
   {FF FF L_lo L_hi FF FF FF FF, CD CC CF CE (wait CC bit4 = 0), FF FF 55 FF}, out (0x2A),0x10,
   ready wait 0x39FB`. One partial program per 512-byte sector (4 per page, NOP 4).
   The 4 bytes `FF FF L_lo L_hi` come from UD 0x37C9..0x37CC (set by the caller).
   Consecutive writes to the same unit continue in the temp unit (0x3303/0x3308 track the next sector);
   skipped sectors are filled by copy from the old unit (sdrw.asm `FillForSkipPage` [src], UD 0x38C6).
2. **Close** (A=4, a write to another unit, or a backward write; UD 0x34A8):
   a. copy the not-yet-written sectors of the **old** unit into the temp unit with two-plane
      **copy-back** (UD 0x38C6/0x3924: `00 a 35`, `00 a' 35`, `85 a [FF-fill skipped sectors] 11`,
      `81 a' .. 10`; sectors that must not be copied are overwritten in the page register with FF
      via `85 col` + 512 x FF);
   b. erase the old unit (UD 0x3785: `70`, `60 row`, `60 row|plane`, `D0`, ready wait);
   c. copy the **whole temp unit back into the old physical unit** (same copy-back);
   d. erase the temp unit.
   => the logical->physical map **never changes** for data writes; the table is only rewritten at mount
   (and presumably when a bad block is retired: Actions' sdrw.asm `BadBlockManage_` marks the block
   0xFFFE and takes a spare unit from the end of the table [src]).
   [emu: after KCFG's settings write, unit 0x4C6 holds the new sector 0, the other 127 sectors
   unchanged; temp unit erased; 273 programs + 8 erases for the whole boot-to-playback session.]
3. Power loss during a close leaves either the temp unit or the old unit holding the data; the mount
   scan then sees two units with the same L (or a temp unit with data). The resolution rule is in BREC
   0x23A4-0x2406 and was not analysed -> **open question**.

What zos must keep consistent so that the stock firmware and USB mode keep working:
* never write blocks 0/1 (BREC) or the firmware units 0..0x3F except through a proper FW update;
* keep the spare format (2.2) on every programmed sector, especially bytes 0-3 and 14 (0x55), and the
  ECC bytes (or FF FF FF FF, which the reader accepts as "no ECC");
* keep table[0x7FD] pointing at an **erased** unit; keep each logical unit on exactly one physical unit
  whose spare carries its L; never leave two units with the same L (do the stock merge order a-d);
* keep the 0xBB table consistent with the spares. The stock mount rescans the spares and rewrites the
  table on every boot, so a stale table is probably repaired [inf, not verified]; but zos itself (and
  any reader that trusts the table, like ftlread.py) must not rely on that.
* USB mode (USBDISK.AP / ADFU) uses the same UD/SD drivers and therefore the same format [inf].

### 3.1 zos implementation (2026-11): `zos/modules/common/ftlw.inc` + `ftlw.h`
Module-side (the core image has no room), assembler for size (~1 KB code, 37 B RAM); used by
`modules/usb` (SCSI WRITE(10), console `wr on|off`, default off) and `modules/ftltest`. Core exports
for it: `nand_read` and `ftl_tbl(zone, entry)` (blk.c, table entry through the same reader as
`hal_blk_read`). Algorithm = the stock one above: temp unit = table[0x7FD] of the zone, same sector
offsets, superpages strictly ascending (a write to an earlier superpage, a rewrite of a written
sector or another unit closes the unit), skipped superpages copied with single-plane copy-back
`00 a 35 / 85 a' [85 col FF..] 10` (written sectors of the current superpage masked with FF, so no
byte is programmed twice; a fully written superpage is not copied: NOP <= 4), close = copy the rest,
two-plane erase of the old unit (`60 r 60 r|0x40 D0`), copy-back of the temp unit into it, erase
the temp unit; the map is never changed. Sector program as UD 0x37C9: `80` + 5 address, ECC start,
DMA1 512 bytes ZRAM1 -> NAND (DA3 = 0x84, mode WS3), `85` spare column, 16 bytes
`FF FF L_lo L_hi FF FF FF FF ECC(CD CC CF CE) FF FF 55 FF`, `10`, status (fail / timeout / WP = sticky
error, the temp copy is erased if the old unit is still intact). Before opening a unit it checks:
table entries < 2048 and different, the data unit's spare `FF FF` + (L or FFFF), the temp unit's first
page of both planes erased. LBAs >= 2^23 and units of missing zones are refused; LBA numbering =
`hal_blk_read`, so the firmware units 0..0x3F, the VM unit 0x40 and blocks 0/1 cannot be reached.
Reads: `hal_blk_read` reads the old physical unit, so callers close the open unit first
(`ftlw_sync(lba, n)` before a READ(10) that may touch it; USB also closes on SYNCHRONIZE CACHE, eject,
1 s idle and disconnect).
Emulator verification: `zos/tools/emu_ftltest.py` (writes into free clusters of the real dump,
backward / rewrite / unit-straddling / boot-sector-identical writes, checks every sector + spare of
the touched units against the state before, temp units erased, nothing else changed, `ftlread info` /
`fatcheck` with `--overlay`, then boots the stock firmware on the result); the NAND model counts
rule violations (program over programmed bytes, NOP > 4, page order inside an erased block).

Open questions: meaning of table[0x7FE]; behaviour when the table block is full; duplicate-L resolution;
wear levelling (none observed: data units stay in place, only the temp unit is cycled); the exact
`0x0A36` / `0x0B13` / `0x2075` / `0x0600` responsibilities (0x0B13 checks a 16 KB structure in IPM-L
0x4000-0x7FFF with 0x55AA at 0x7FFC and a word checksum at 0x7FFE).

---------------------------------------------------------------------------------------------------
## 4. Verification
* `tools/ftlread.py DUMP info`: both zone tables found (0x55AA), 0 spare/table mismatches over all
  mapped units; free lists 73 / 71 units.
* `export-fw --compare ../my-ximpro.fw`: the 16 MB firmware region starts with a byte-identical FWIMAGE.FW.
* `export-disk`: 1 981 937 sectors; the image file passes `fatcheck` (1084 files, 63 dirs, 0 problems,
  FAT copies equal). `ecc 1000`: 1000/1000 sectors match the stored ECC.
* MP3 files: all 29 MP3s have valid ID3v2 + two consecutive MPEG frame headers (scratch check).
* Emulator (`atj2085emu --nand`, real BREC init + real UD_SF641): boots to the menu with the device's
  own settings (theme, EQ, volume, resume point); MUSIC.AP lists the root MP3 and resumes it at 28:56;
  the PCM is found in an independent libsndfile decode of the file extracted with ftlread at 28:58.8
  with **SNR 66 dB** (`atj2085emu/logs/nand_compare.txt`). So the emulated hardware + the real driver and
  ftlread.py agree on the data.

---------------------------------------------------------------------------------------------------
## 5. Dump container (s1clone)
`s1mp3_github/common/CloneImage.h` + `s1clone/CloneDevice.cpp` (read path): header 16408 bytes =
`"s1clone\0"`, u32 flash_id (`EC D3 51 95`), u32 pages (0x80000), u32 pages/block (64), u32 page size
(0x840), then the 16 KB **boot ROM** image read at 0x8000 with ports 01=02=0. Then one record per
physical page, **all pages present, in order**, each `{u16 checksum, u16 encoded_size, encoded bytes}`;
encoding = bytes copied, a byte equal to its predecessor is followed by a 7-bit varint repeat count
(then the pair state resets); checksum = rotate-right-add over the 0x840 decoded bytes. Every page
includes its 64 spare bytes. Decoder + cached offset index: `tools/s1clone.py`.
Side result: the BROM image at 0x8800-0x8E77 matches the IAR C runtime the emulator loads from
`comlib.s28` in 1644 of 1652 bytes (`rom/comlib.s28` assumption essentially confirmed; the 8 differing
bytes were not analysed).
