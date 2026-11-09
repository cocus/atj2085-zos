# MP3 playback on the ATJ2085: Z80-side driver specification (v04 / LSD_501_04)

This spec covers everything the Z80 does to play an MP3 with the stock DSP program
`MP3DEC.DSP` of the user's firmware (my-ximpro.fw, v04), down to the port level. With it you
can write a driver from scratch without the stock OS.

**Sources.** Each item cites its source:

- v04 `MUSIC.AP` codec banks 0x01 (helpers), 0x02 (`c_mp3decINIT`) and 0x03 (IRQ handler). All
  three run in the window 0x2400-0x26FF. Bank 0x03 is stored as `.db` in `disasm_v04`, so it was
  disassembled linearly for this spec.
- v04 MP3 basal banks 0x21 (read hook) and 0x23 (open, play, stop).
- v04 `SYSBNKA.SYS` API pages 0x23 (InitDSPCode), 0x24 (StartDsp, StopDsp, SetDspMips), 0x27
  (DAC) and 0x28 (PA). Each page is disassembled at 0x0400.
- The SDK 4.3 codec library `mp3dec51` (`analysis/mp3dec51_lib.txt`, with symbol names) and the
  basal sources `basal_mp3_de*.c` and `mp3decC.h`.
- The emulator trace of the real InitDSPCode (`atj2085emu`, `FWREAD` log).

**Notation.**

- `bank:addr` = a MUSIC.AP bank and its Z80 address. `A:pp:addr` = a SYSBNKA page and its address.
- "lib" = the SDK mp3dec51 symbol of the same routine. The v04 code is the same code relocated:
  lib 0x06E6..0x070F maps to v04 0x2400..0x2429 (+0x1D1A), and later variables are shifted by 1-6
  bytes.
- **(inf)** marks an inference not proven by code or hardware.
- DSP words are 24-bit. "IDM-L 0x1C03" means the low byte lane of DM word 0x1C03.

**Correction to `dsp_interface.md` / README.** The v04 handler that "also handles 0x00-0x30,
0x88 with replies 0x3A/0x3C/0x88/0xDD/0xE1" is **bank 0x0B**. Bank 0x0B is the driver of the
WMA/AG4/AG8/PCM decoders (its program names are in bank 0x0A), not of MP3. The v04 MP3 handler
(bank 0x03) handles exactly the 11 request codes in section 4.

---

## 0. Summary (minimal facts a driver needs)

| item | value |
|---|---|
| program | `MP3DEC.DSP`, ADEF container, 139 sectors. DM image 0x3800 words, PM image 0x2400 words. No overlays. |
| load | Lane page per section: DMLS/DMMS/DMHS → port05 0x34/0x35/0x36 and PMLS/PMMS/PMHS → 0xC0/0xC1/0xC2 (the stock OS uses 0xF4/F5/F6/F0/F1/F2, which give all four halves to the MCU). Write section byte *i* to 0x4000+*i*, which is word *i*. All sections start at word 0. Then set port05 bits 7-4 = 0. |
| memory used while it runs | **all of PM and all of DM.** PM 0x0000-0x23FF is code. DM 0x0000-0x37FF is the image, and DM 0x3E08.. is used at run time. Because ownership is per 8 KB half and every half holds MP3DEC data, **no DSP lane byte is free for the MCU while MP3DEC runs**. The handler only borrows the IDM low half (0x24/0x25/0x26) or IDM-L (0x34) while the DSP is blocked waiting for a reply. |
| start | `42 = (42&E3)|0B`, `42 |= 40`, 2 ms, `2E = 7B`, `2F = 03`, `27 |= 01`, load, `05 = 0`, `2E |= 80` |
| IRQ | `26` bit 0. Read `35` twice, `out(26,01)`, handle, then write the reply code to `35` last. |
| requests | 0x11 → 0x22. 0xEE → 0xDD (bitstream). 0xCC → 0x99 (format, DAC rate). 0xBB → 0x00 (time report). 0xAA = end (no reply). 0x55 → 0x66. 0xE0 = exit ack (no reply). 0xA8 → 0x95. 0xA9 → 0x96. 0x77 → 0x88. 0x2B → 0x3B (overlay, never used by v04 MP3DEC). |
| bitstream | 512 B per 0xEE. DMA1 from ZRAM to IDM-H word 0x1CB3 with ports 08..12 = `00 00 00 B3 1C 00 40 06 FF 01 30`, then `13 = 01`. |
| end of file | Answer 0xEE with port30 = 3 and no DMA. The DSP drains and raises 0xAA. |
| time | Total: IDM-L 0x1C01 s, 0x1C02 min, 0x1D52 h (valid at 0xCC). Current: IDM-L 0x1D57 h, 0x1D58 min, 0x1D59 s, 0x1D5A/B ms (at each 0xBB). |

---

## 1. DSP program loading (InitDSPCode, API 0x0023)

### 1.1 ADEF container (MP3DEC.DSP v04, 71168 bytes)

```
+0x00  'ADEF', u16 0, u16 total_sectors (0x8B = 139), 8 bytes 0
+0x10  6 x {char tag[4]; u16 start_sector; u16 sector_count}   (file sectors of 512 B)
         DMLS  1 28 | DMMS 29 28 | DMHS 57 28 | PMLS 85 18 | PMMS 103 18 | PMHS 121 18
+0x40  7 x {u16 a; u16 len; u16 chk; u16 0}   8-byte records
         rec0 = (0, 0x40, 0x160B)   header record (header length 0x40, checksum)
         rec1..6 = (0, len, chk) per section in the same order: DM len 0x3800, PM len 0x2400
+0xF8  "date:2006 02 13", "time:11:47:24AM" (build stamp)
+0x140 / +0x180  overlay descriptors (all zero in MP3DEC -> no overlays)
```

- **Records.** `a` is 0 in every MP3DEC record. It is 1 in AG8DEC/AG8ENC, so it may be a start
  word or a flag (inf). `len` is the number of words (= bytes per lane). **The checksum is not a
  byte sum** (algorithm unknown). **InitDSPCode reads neither the records nor the checksum.**
  It uses only the table at +0x10, so a loader can ignore +0x40.
- **Lane data.** In a section, byte *i* is that lane's byte of DSP word *i*. For example, PM
  word *w* = PMLS[w] | PMMS[w]<<8 | PMHS[w]<<16.
- **Highest non-zero byte.** DM: 0x3670. PM: 0x23E4.

### 1.2 Stock algorithm (SYSBNKA page 0x23)

The page table entry is fn 0 = 0x0567. The emulator trace confirms the steps.

1. `A:23:0567` saves C (the part) at 0x0467. `call 0x2E00` looks up `DE` (the file name) in the
   FWIMAGE directory and returns HL = the entry. If the file is not found, it calls syserr 0x0200.
   The entry's +0x0C/+0x0E (u32 byte offset of the file in the firmware) goes to 0x0463/0x0465.
2. `A:23:046B` saves port 05 and sets `out(05, 0xF4)`: all four IPM/IDM halves go to the MCU,
   and the lane is IDM-L.
3. It reads file sector 0 (the header, 512 B) through the firmware-read entry `call 0x32DC`. The
   request struct at 0x045B is `{u32 offset; u8; u16 dest; u8 page}` with dest = 0x3000 and
   page = 4. Page 0-2/4-6 together with dest < 0x4000 means "DMA into DSP lane `page`, word
   `dest`", so the header lands in IDM-L words 0x3000-0x31FF. DMLS later overwrites it.
4. It copies the 0x30-byte section table from 0x7010 (= IDM-L word 0x3010 = header +0x10) to
   0x0413.
5. **Overlay (C ≠ 0).** It copies 0x30 bytes from header +0x140 to 0x042B instead. If HDR4
   (`in(0x34)`) ≠ 0 it uses +0x180. This replaces the three PM entries and puts the PM
   destination word (u16 at block +0x20 → 0x044B) into the dest variable. The DM load is skipped
   (`jp 04EB`).
6. **Lane loader** `A:23:0517(A = lane, IY = &entry.start_sector)`. For each lane in the order
   **DM-L (lane 4, port05 0xF4), DM-M (5, 0xF5), DM-H (6, 0xF6), PM-L (0, 0xF0), PM-M (1, 0xF1),
   PM-H (2, 0xF2)**:
   - offset = file_base + start_sector*512. Only the **low byte** of start and count is used, so
     both are ≤ 255.
   - dest = 0 (or the overlay word).
   - `count` times: firmware-read 512 B → lane `A`, word `dest`. Then offset += 512 and
     dest += 0x200.
7. It restores the caller's port 05. It does **not** start the DSP and does **not** check
   anything.

**Order in the codec** (`c_mp3decINIT` 02:2500, lib `mp3deDecoderINIT`/`RestDsp`):

1. Clock and reset (section 2).
2. `InitDSPCode("mp3dec.dsp", C=0)`.
3. `out(05,00)`, `out(05,00)` (all halves back to the DSP).
4. `ReSetIO5` (01:24C4): `05 = (in(05) & F0) | (saved & 0F)`. This restores the lane bits and
   keeps bits 7-4 = 0.

### 1.3 Loader for a from-scratch OS

Use the device-measured pages:

```
for (sec in DMLS,DMMS,DMHS,PMLS,PMMS,PMHS):
    out(0x05, page[sec])        // 0x34,0x35,0x36,0xC0,0xC1,0xC2  (both 8 KB halves of that lane)
    copy sec.count*512 bytes of the section to 0x4000..   // byte i -> word i; max 0x4000 bytes
out(0x05, 0x07)                 // bits 7-4 = 0: every half back to the DSP; lane 7 = ZRAM2 view
```

- The header sector does not need to be copied anywhere.
- One-half pages also exist: 0x80/0x40 (IPM low/high half), 0x20/0x10 (IDM low/high half), each
  OR the lane.
- **Reload DM before every start.** The DSP modifies DM at run time. PM stays unchanged (there
  are no overlays), so it only needs a reload if the lanes were reused. The stock reloads
  everything on every start, both for the scan pass and for the play pass.

### 1.4 Memory MP3DEC occupies

| space | image | run-time use seen from the Z80 side | MCU access while it runs |
|---|---|---|---|
| PM | words 0x0000-0x23FF (low half full, high half 0x2000-0x23FF) | — | none. Both IPM halves must stay DSP-owned. PM 0x2400-0x3FFF is unused by the image but sits in the high half. |
| DM | words 0x0000-0x37FF | mailbox variables 0x1C01-0x1D60, bitstream buffer 0x1C09-0x1CB3 (inf, from the DMA), resume bytes IDM-L 0x3E08-0x3E0A, registers 0x3F00-0x3FFF (DAC FIFO 0x3FED, PG) | only while the DSP is blocked on a request: 0x24/0x25/0x26 (low half, L/M/H lane) or 0x34 (IDM-L, both halves) |

**Consequence for zos M2 (.ZM modules in DSP pages).** Nothing may live in IPM/IDM lanes while
MP3DEC is running. Evict the modules before loading MP3DEC, or reload them after stopping it.
ZRAM2 (port05 lane 7) and ZRAM1 are unaffected.

### 1.5 Overlays (request 0x2B)

The handler `03:2814` (lib `DealProc_2b`) does:

1. `2E &= FD` (DSP clock off).
2. `InitDSPCode(name, C=1)` (loads PM per section 1.2 step 5).
3. `2E |= 02`.
4. Reply 0x3B.

**v04 MP3DEC has an all-zero overlay area (+0x140..+0x1BF), so it never asks for 0x2B.** OGGDEC,
WMA and the others were not checked.

---

## 2. Reset, clock, start, stop

| routine | where | port sequence |
|---|---|---|
| DSP clock + reset (RestDsp) | 02:2560 (lib `RestDsp` 0x06A6) | `42 = (in42 & E3) | 0B` (DSP clock field bits 4-2 = 010 = 36 MIPS; bit1 PLL2EN; bit0 FSSEL = 11.2896 MHz). `42 |= 40` (PLL1EN). `DELAY1MS` ×2. `2E = 7B` (bit7 = 0 **reset held**, DUMMYS = 111, bit3 = 1, bit2 = 0 PLL source, bit1 = 1 **DSPCKEN**, bit0 = 1 IRQ2DSP idle). `2F = 03`. `EnableDspInt` (01:2475): `27 |= 01`. Then load (section 1). |
| StartDSP | 03:2500 (lib 0x0000) | `2E |= 80` (run). Then start the watchdog (0x2440 = 0x14). The DSP starts at PM 0 (inf). |
| STOPDSP | 03:2A7B (lib 0x053B) | `2E = A & 7F`. **Note:** A is whatever the caller had. In `c_Mp3decEXIT` that is the port-05 value. Use `in(2E) & 7F`. Then `2E = in(2E) & 7D` (reset + clock off), `42 &= BF` (PLL1 off), `DisableDspInt` (01:247C): `27 &= FE`, `out(26,01)`. |
| OS StartDsp (API 0x0024, E = MIPS step) | A:24:043B | `42 = (42 & 83) | E<<2 | 60` (PLL1EN + M48EN), 2 ms, `2F = 03`, `2E = 7B`, `out(26,01)`, `27 |= 01`, `2E |= 80` |
| OS StopDsp (0x0124) | A:24:0413 | `27 &= FE`, `out(26,01)`, `2E &= 7D`, `42 &= 9F` |
| OS SetDspMips (0x0224, E) | A:24:0484 | `2E &= FD` (clock off), `42 = (42 & E3) | E<<2`, 2 ms, `2E |= 02` |
| OS 0x0324 / 0x0424 | A:24:042A / 0469 | stop without the PLL change / start without the clock setup |
| MIPS up (request 0x77) | 03:27DC | if `(42 & 1C) < 0C` then `42 += 04` (one step, maximum field 011 = 48 MIPS). Reply 0x88. |

- **Delays.** `DELAY1MS` (01:2487) is 4×255×(`ld hl,nn` + `djnz`) ≈ 23.5 k T-states (≈1 ms at
  24 MHz).
- **Halt / hold.** There is no other halt bit. "Hold" is DSPCKEN = 0 (`2E` bit 1), used during
  overlay loads. Reset is `2E` bit 7 = 0.
- **MCU → DSP interrupt.** `2E` bit 0 (active low, PG) is never pulsed by the MP3 driver. The
  driver writes HSR7 (`37`) bit 6 only from its watchdog (section 4.3).

---

## 3. DAC, analog, PA, volume, sample rate, close

### 3.1 OS APIs used by MUSIC.AP

**DAC_Enable** (0x0027, A:27:0413, DE → {u8 mode, u8 kHz})

- Always: `80 = 00`.
- mode 0: `rate(kHz)`, then `8B &= EF`, `80 = 28` (DACEN + on-chip SDM, unmuted), `8B = 18`
  (FIFO counter running, QU = 8).
- mode 1: `FF &= 0F`, `80 = A8`, `rate(kHz)`.
- MUSIC calls it at 4E:115E with {0, 8}, then `80 |= 08` and `8B |= 10`.

**DAC_SetRate** (0x0227, A:27:0460, E = kHz)

- Looks up the code in the kHz table at A:27:04EA: `30 00 18 0C 20 00 10 08 2C 00 16 0B FF`.
  The index is the code: 48k→0, 24k→2, 12k→3, 32k→4, 16k→6, 8k→7, 44.1k→8, 22.05k→0xA,
  11.025k→0xB.
- If `(81 ^ code) & 0F` ≠ 0: `80 &= DF` (DAC off), `81 = code`. Then **code bit 3 set (44.1k
  family) → `42 |= 03`, else `42 &= FC`**. Then wait 0x0C00 loop turns (≈4 ms).
- Finally `80 = (80 & FD) | 20` and `8B` bit 4 0→1 (FIFO reset pulse).

**DAC_Disable** (0x0127): `80 = (80 | 02) & DF`.

**PA_Enable** (0x0028, A:28:0413, DE → {u8 flags, u8 vol})

- flags bit0 → `80 &= FD` (DAC to PA); bit1/2/3 → `D5 |= 04/08/10` (MIC/line/FM to mixer).
- `9E = vol` (0..31, else it fails).
- `DD |= C0` when flags bit0 is set (analog DAC + mixer/PA), else `|= 40`. Then `DD |= 02`.
- MUSIC: 4E:1443 with {0x01, volume}.

**PA_Disable** (0x0128, A:28:046B): `D4 |= 80`, `DD &= 3F`, `D5 &= E3`, `80 |= 02`, about
3×65 k loop delay, `D4 &= 7F`. MUSIC calls it on exit (4E:14B8).

**PA_SetVolume** (0x0228): `9E = E` (E ≤ 31). **PA_GetVolume**: `9E & 1F`.

### 3.2 Codec

- **`c_mp3decINIT`** (02:250C): `8B = 05`. This clears bit 4, so the FIFO counter is in reset
  until the 0xCC request arrives.
- **Rate change on 0xCC**, `c_mp3deChangDAC3fs` (03:2ABC, lib 0x057C):

  ```
  code = in(34)                      // DSP's rate code (HDR4)
  if ((in(81) ^ code) & 0x0F) {
      out(80, in(80) & DF)           // DAC off
      out(81, code)
      if (code & 0x80) { out(42, in(42) | 03); delay 20 x 1 ms }   // 11.2896 MHz audio PLL
      else               out(42, in(42) & FC)                         // 12.288 MHz
  }
  out(80, in(80) | 20)               // DAC on
  out(8B, in(8B) & EF); out(8B, in(8B) | 10)   // FIFO counter reset pulse
  out(80, in(80) & FD)               // unmute
  ```

**DSP rate code table.**

| rate (Hz) | 48000 | 32000 | 24000 | 16000 | 12000 | 8000 | 44100 | 22050 | 11025 |
|---|---|---|---|---|---|---|---|---|---|
| port 0x81 low nibble | 0 | 4 | 2 | 6 | 3 | 7 | 8 | A | B |
| port 0x42 bits 1-0 | 00 | 00 | 00 | 00 | 00 | 00 | 11 | 11 | 11 |

- The codec selects the PLL from **bit 7** of the DSP's code (SDK label `_sample_44_10_22_05`).
  The OS selects it from **bit 3** of its own code. So the DSP reports 0x88/0x8A/0x8B for the
  44.1k family (inf, but the code depends on it).
- **Robust driver rule:** the low nibble goes to port 0x81, and the 11.2896 MHz clock is used for
  low nibbles 8, A and B.

**Volume ramp** (used around pause, seek and stop by the basal):

- `c_mp3deMinishVolumn` (03:2AFB) saves `9E` in 0x243B, then decrements `9E` to 0 with 3 ms per
  step.
- `c_mp3InMinishVolumn` (03:2B15) increments back to the saved value with 3 ms per step.
- `c_mp3deCLearDAC3FIFO` (03:2B2D) writes 0 to `88/89/8A` 20 times.
- The DSP also gets a digital volume. IDM-L **0x1C07** ← `in(9E)` on the first 0xEE, and
  ← g_mp3deVolumeVal (0x2469, set by MC_SETVOLUME at 22:2130) on every 0xBB.

**Close.** MC_CLOSE (22:211C) closes the file and does `8B |= 10`. AP exit runs PA_Disable.

---

## 4. The v04 MP3 IRQ handler (MUSIC bank 0x03)

### 4.1 Entry and dispatch

`mp3deDecoderHIPINT` (03:253A) is registered with `IRQ_Intercept(handler, IRQ_DSP)`. The kernel
calls it from the IM1 vector when `26` bit 0 is set.

```
if (HipIntType(0x2432) == 0) {          // no request being handled
    in(35); a = in(35)                   // read HDR5 TWICE (the first read is discarded)
    HipIntType = a
    if (a == 0x55) SentenceFlag(0x2436) = 1
    out(26, 01)                          // ack the DSP interrupt (W1C)
    dispatch()                           // 03:2561
}                                        // else: ignore (a request is still pending)
```

`dispatch` (03:2561, lib `mp3deInterruptType`):

1. Save port 05 (0x2464).
2. If DecodeInfor (0x2431) == 0xFE (finished) or PauseTrueFlag (0x243A) == 0xFF (paused), exit
   **without a reply**. The DSP stays blocked.
3. Otherwise branch on HipIntType.
4. Every path ends at 03:27FA `ReSetIO5`: `05 = (in05 & F0) | (saved & 0F)`.

**The reply.** The reply is `out(35, code)`. Data ports (`30`-`34`) are written before it, and
`HipIntType = 0` is set just before or after it. An unknown request code falls through without
a reply and leaves HipIntType set (stuck).

### 4.2 Request table

| req | handler | reads | writes | reply |
|---|---|---|---|---|
| **0x11** start handshake | 03:2574 | — | `05 = 34`. `34` ← SecondRestFlag (0x2400): **00 play, 01 resume, FF scan (duration only)**, E5 after a watchdog restart. If 01, 3 saved bytes → **IDM-L 0x3E08..0x3E0A** (0x7E08). Then `05 = 00`. | **0x22** |
| **0xEE** bitstream request | 03:26AF (section 5) | `32`/`33` = u16 sector step (v04 addition, → 0x2444/45), `34` = direction 0x11 forward / 0x22 backward, `31` = frame-sync status (0 = no sync) | First time only (0x2439 == FF): page 0x24 IDM-L **0x1C07** = `in(9E)`; resume → DMA time state (section 5.4); **file length** u32 → IDM-L/M/H **0x1C03** (bytes 0, 1, 2) + IDM-L **0x1C04** (byte 3); sentence gates IDM-L/M/H **0x1D4D** ← 0x2401-03 (default 0x00202F) and **0x1D4C** ← 0x2404-06 (0x000150); start time 0x1D50 L/M ← 0x240B/0C; end time 0x1D4F L/M ← 0x240D/0E (1/8 s units, inf from `mp3decC.h`). Then DMA 512 B; `30` = data status; `33` = 0x2448 (always **1** in v04, set at 23:2146, meaning unknown) | **0xDD** |
| **0xCC** format known | 03:2762 | `05 = 24`: `30` mode (MPEG channel mode, 3 = mono (inf)), `31`-`33` bitrate as 3 ASCII digits (" 64", "128"), `34` DAC rate code. First time (0x2438 == FF): **IDM-L 0x1C01 = s, 0x1C02 = min, 0x1D52 = h** = total time → c_mp3deSongTimeLength (0x2420-22) | `05 = 00`. **Scan mode** (0x2400 == FF): DecodeInfor = FE, ExitEnable = 1, **no reply** (the basal then stops the DSP). Otherwise: DAC rate change (section 3.2) | **0x99** (play only) |
| **0xBB** periodic status | 03:25AA | `05 = 24`: **IDM-L 0x1D57..0x1D5B** → NowTime h, min, s, ms_lo, ms_hi (0x2423-27). FF/REW state IDM-L 0x1C05 (section 5.3). It also re-reads `30`-`34` like 0xCC (03:2651 → 283B → 285A); bitrate and mode can change (VBR) | FF/REW mute frames IDM-L 0x1D5E/0x1D5D/0x1D5C ← 0x241B/1C/1D (defaults 0x26, 0x39, 0); FF/REW command 0x1C05 = 1, 0x1C06 = 0x55/0xAA; A-B command 0x1D4E (0x44 set A, 0x77 play from A); tempo 0x1D60 ← 0x2449 (inf); EQ 11 bytes **0x1D3E..0x1D48** ← flag + 10 band values (0..10, default 5); volume **0x1C07** ← 0x2469. **v04 only:** `FB |= 80`, `FC |= 80`, `FD &= 7F`, `33` ← `in(FD)`, then FB/FC are restored (undocumented ports, probably an ID/anti-clone value for the DSP, inf). `34` ← PauseKeyToDspFlag (0x243F; 0 once after a pause, else 1), then 0x243F = 1. `05 = 00`. FF/REW not yet due → exit **without a reply** (the 20 ms timer replies later) | **0x00** |
| **0xAA** end of stream | 03:2799 | — | DecodeInfor = FE, ExitEnable (0x244E) = 1 | none |
| **0x55** sentence point | 03:2692 | — | (flag set at entry). If DecodeInfor == F5, no reply | **0x66** |
| **0xE0** exit acknowledged (after a fade-out) | 03:274C | — | ExitEnable = 1, fade-out flag 0x244F = 0, watchdog off | none |
| **0xA8** chip check | 03:27AA | `in(2F)` | `34` ← `in(2F)` | **0x95** |
| **0xA9** chip verdict | 03:27BC | `34` → 0x2434 (0 = OK) | ≠ 0 → watchdog off, DecodeInfor = 0xEE (refused) | **0x96** |
| **0x77** more MIPS | 03:27DC | `42` | `42 += 4` if `(42 & 1C) < 0C` | **0x88** |
| **0x2B** load overlay | 03:25A0 → 2814 | — | `2E &= FD`, InitDSPCode(name, 1), `2E |= 02` | **0x3B** |

**Watchdog reload.** After each request the 20 ms watchdog 0x2440 (`DowmMachCount`) is reloaded:
0x28 for 0xEE, 0x14 for most others, 0 (off) for 0xCC, 0xE0 and 0xA9-bad.

### 4.3 Periodic timer (optional for a new driver)

The basal starts the timer with `TM_SetTimer(RTCTimerServe, 1)` (10 ms tick). `RTCTimerServe`
calls 03:29F6 every 2nd tick (20 ms).

- **`DownMachDeal` 03:2A3B.** If 0x2440 > 0, decrement it. When it reaches 1, the DSP has been
  silent for 0.4-0.8 s: SecondRestFlag = E5 (unless scanning), retry counter 0x2441 (6) is
  decremented (at 0 → DSPResetFlag 0x2433 = FF), `37 |= 40` (HSR7 bit 6, meaning unknown), and
  DecodeInfor = **FA**. The basal then calls `c_mp3decINIT`, which skips the variable reset when
  it is FA, reloads and restarts the DSP.
- **03:2A02 (FF/REW pacing).** If FF/REW is active (0x241F = 55/AA), count 0x2442 up to the
  interval table 0x2450[speed] = {0x19, 0x14, 0x0F, 0x0A, 0x03} ticks. When it is due and a
  request is pending (0x2432 ≠ 0), call `dispatch` (this answers the held-back 0xBB).

### 4.4 Pause, resume, exit

- **Pause** `mp3dePauseDisposal` 03:2A8F: watchdog off, PauseKeyToDspFlag = 0, PauseTrueFlag
  (0x243A) = FF. From then on every request goes unanswered, the DSP blocks, and the DAC runs dry.
- **Resume** `mp3deContiuneDisposal` 03:2AA0: watchdog = 0x14, 0x243A = 0, `di`, (0x0037) = 1,
  `dispatch()` (answers the pending request), (0x0037) = 0, `ei`. The next 0xBB then carries
  `34 = 0`.
- **Exit** `c_Mp3decEXIT` 03:297D. The caller loops `while(!c_Mp3decEXIT())`.
  1. If no fade-out was requested (0x244F ≠ FF): ReadFileEndFlag (0x244D) = 0 (the next 0xEE
     gets "no more data") and 0x243A = 0.
  2. If ExitEnable (0x244E) == 0, return 0. It becomes 1 after the 0xAA, 0xE0, scan-0xCC or
     "not MP3" path.
  3. Otherwise: HipIntType = 0, `DisableDspInt`, clear FFRevSpeed/0x2435/SecondRestFlag/
     DecodeInfor, save port 05, `STOPDSP`, `ReMemBerATime` (section 5.4), restore port 05,
     return 1.
- **Fade.** Setting 0x244C = 2 (lib `c_mp3decFadeOut` 07B8) makes the next 0xEE answer send
  `30 = 2` (fade out) once. The DSP fades and sends 0xE0. 0x244C = 1 (fade in, 01:24D2) sends
  `30 = 1`.
- **Minimal driver.** It can skip the fade: just reset the DSP (`2E &= 7F`), after muting the
  DAC (`80 |= 02`) or ramping `9E` down to avoid a click.

---

## 5. Bitstream feeding

### 5.1 One 0xEE request (03:26AF; lib `compare_ee` / `sendDDDD__` / `NeeDReadData___`)

Driver state:

- `buf` = 512 B in Z80 RAM. v04 uses 0x1DCD; the pointer is at 0x2467.
- `have` = BitStreamTimes (0x2430), 1 if `buf` holds an unsent sector.
- `more` = ReadFileEndFlag (0x244D), 0 after a failed read.

```
step = in(32) | in(33)<<8         // v04: sector step for the read that follows (1 = sequential)
dir  = in(34)                     // 0x11 forward, 0x22 backward
if dir != last_dir: last_dir = dir; have = 0         // FF_Rew_ReadCheck: drop the prefetched sector
if in(31) == 0:                   // no frame sync in what it has
    if ++nosync == 100 and not after_first_BB: file is not MP3 -> abort, NO reply
loop:
  if fade == 3 or more == 0:      // end of data
      fade = 3; out(30, 3); out(33, 1); out(35, 0xDD); return          // NO DMA
  if have:
      if first_time: write the DM parameters (section 4.2 0xEE row), with port05 = 24/25/26
      DMA buf -> DSP (5.2); have = 0
      out(05, saved & 0F)
      out(30, 0 | 1 fade-in | 2 fade-out); out(33, 1); out(35, 0xDD)   // reply
  // the refill runs AFTER the reply, still inside the IRQ: prefetch for the next request
  out(05, saved & 0F); if have == 0: more = hook_read(buf)   // FS read, see 5.3
  have = 1
  if a reply was already sent: return; else goto loop          // first request: read, then send
```

- **First request.** The first 0xEE finds `buf` empty, reads the first sector, then DMAs and
  replies.
- **Later requests.** They DMA the sector that was prefetched by the previous request, reply at
  once, then read the next sector. The FS latency is hidden behind the DSP's decoding.

### 5.2 DMA1 programming (01:2497 `DmaDriver`, table 03:2B47 = lib `DMA_BitStreamTable`)

| port | value | meaning (PG p.14-16) |
|---|---|---|
| 06, 07 | buf low, buf high | SA[15:0] = Z80 address of the 512-byte buffer (ZRAM1/2) |
| 08 | 00 | SA[23:16] |
| 09 | 00 | SA3: internal, ZRAM |
| 0A | 00 | ISA lane (unused) |
| 0B, 0C, 0D | B3, 1C, 00 | DA = DSP word **0x1CB3** |
| 0E | 40 | DA3: internal, IPM/IDM/ZRAM2 |
| 0F | 06 | IDA lane = **IDM high byte** |
| 10, 11 | FF, 01 | byte count 0x01FF → 512 bytes (count−1 encoding (inf)) |
| 12 | 30 | mode: **bit5 DSP transfer mode** (lane cycles per word) + **bit4 destination down-count** |
| 13 | 01 | start. Then poll `while (in(13) & 1);` |

- **Effect (inf).** The 512 bytes go to 170⅔ words at descending addresses from 0x1CB3 to
  0x1C09, three bytes per word starting with the high lane. Copy the values exactly; the DSP
  depends on this layout.
- **CPU halt.** DMA1 halts the CPU until it completes (PG p.22), so the poll ends at once.
- **Completion flag.** DMA1 also sets `25` bit 3 (DMA1 end, W1C). The stock driver neither uses
  nor clears it, and no DMA interrupt is enabled (`13` bits 7/6 = 0).
- **Port 05 during the DMA.** The stock driver issues the DMA while port 05 = 0x24 (the IDM low
  half mapped to the MCU). The destination is in the low half, but the DMA works anyway.

### 5.3 Reading, seeking, FF/REW, end of file (read hook 21:1C00, `m_Mp3deRead_Data`)

**v04 hook:**

1. `05 = (05 & F0) | 07` (ZRAM2 view during the FS call).
2. If REW (0x241F == AA): `FS_FSeek(1, mode 2)`, which seeks one sector back. A failure at the
   start of the file means "reached head".
3. A-B bookkeeping (0x241E 0x22/0xAA/0x77, positions 0x1DB6.., seek back to A).
4. **If the u32 step (0x2444..47, low word from ports 32/33) ≠ 1:**
   `FS_FSeek(step, mode 1 = relative forward)` before the read.
5. `FS_FRead(buf, 1 sector)`. Failure → set the status (EOF 0x0E / 0x06, error 0x0C) and return
   0. Success → return 1.

**Semantics for a new driver:**

- **Sequential play:** step = 1 → read the next 512-byte sector.
- **step ≠ 1:** skip `step` sectors forward, then read (inf: whether the skip is `step` or
  `step−1` depends on the stock FS_FSeek mode-1 semantics, unverified). The DSP uses this for
  fast forward. Backward reading comes with `34 = 0x22`; the stock REW path seeks one sector back
  before each read.
- **End of file:** the hook returns 0 once FS_FRead fails, i.e. after the last (possibly partial)
  sector has been sent. The next 0xEE is answered with `30 = 3` and no DMA. The DSP knows the
  exact length from DM 0x1C03/04 and ignores the padding of the last sector (inf). It plays out
  its buffer and raises **0xAA**.
- **User seek (jump to a position):** the stock player does not seek while running. It stops the
  DSP, seeks the file (`FS_FSeek(pos)`), and restarts with SecondRestFlag = 1 (resume, 23:21BE),
  or 0 for a plain jump with the DSP clock starting at 0.
- **FF/REW while playing.** The UI sets 0x241F = 55 (FF) / AA (REW) and 0x241A = speed index.
  On each 0xBB whose interval has elapsed (table 0x2450, in 20 ms ticks) the handler writes
  IDM-L 0x1C05 = 1 and 0x1C06 = 55/AA. The DSP then skips and asks with steps and direction.
  0x1C05 is cleared by the DSP when it has consumed the command. The basal also adjusts the
  displayed time.

### 5.4 Resume / A-B time state (03:29C0 `ReMemBerATime`, 03:29E7 `ReseTABTime`)

**At stop:**

- DMA 12 B from **DM 0x1D53** (0x1D19 if TimeBackupMode 0x244B ≠ 0, i.e. A-B) to RAM 0x2455.
  Descriptor 03:2B54: SA = 0x1D53, `08` = 00, `09` = 40 (internal DSP), `0A` = 04 (IDM-L),
  DA = 0x2455, `0E` = 00 (ZRAM), `10/11` = 0B 00 (12 B), `12` = 20 (DSP mode, up-count).
  This is 4 words, L/M/H each.
- Copy IDM-L 0x3E08..0A (port05 0x34, 0x7E08) to 0x2461.
- Save the file position (FS_FTell, in sectors) as the break point.

**At restart with 0x11 → 1:**

- Write the 3 bytes back to IDM-L 0x3E08 before replying 0x22.
- On the first 0xEE, DMA the 12 bytes back to DM **0x1D53 and 0x1D19** (tables 03:2B61 / 2B6E,
  ZRAM → IDM-L, DSP mode).
- The host has already positioned the file at the saved sector.

---

## 6. Time, duration, format

| what | where | when valid |
|---|---|---|
| total time | IDM-L 0x1C01 = seconds, 0x1C02 = minutes, 0x1D52 = hours (port05 0x24 → 0x5C01, 0x5C02, 0x5D52) | at the first 0xCC (scan or play). In scan mode the DSP computes it, using the file length from DM 0x1C03/04 (CBR estimate or Xing (inf)). |
| current time | IDM-L 0x1D57 h, 0x1D58 min, 0x1D59 s, 0x1D5A ms low, 0x1D5B ms high | at each 0xBB (the stock reads it only there) |
| bitrate | ports `31`, `32`, `33` = 3 ASCII digits, kbps | at 0xCC and each 0xBB |
| sample rate | port `34` = DAC rate code (section 3.2 table) | at 0xCC (and 0xBB) |
| channel mode / channels | port `30` (stored as c_mp3deMp3Mode; MPEG mode field, 3 = single channel → mono (inf)) | at 0xCC / 0xBB |
| not an MP3 | 100 × 0xEE with `31` = 0 before the first 0xBB | — |

The emulator model fills exactly these locations (`emu/dsp.py`).

---

## 7. Interrupt / port-05 hygiene

- Read `35` twice, then `out(26, 01)` **before** handling. The DSP raises the next request only
  after it has seen the reply (inf).
- Save port 05 on entry. The handler maps 0x24/0x25/0x26/0x34 only while the DSP is blocked.
  Always leave with bits 7-4 = 0 (`05 = saved & 0F`). A DSP-owned half is invisible to the MCU,
  and an MCU-owned half is invisible to the DSP.
- Long work inside the IRQ (the FS read) is fine. The DSP waits for the reply, and the stock
  driver reads the next sector inside the IRQ after replying.

---

## 8. Minimal driver (pseudo-code)

```c
/* ---- one-time ---- */
dac_on(): out(80,00); dac_rate(44100);
          out(8B, in(8B)&~0x10); out(80,0x28); out(8B,0x18);             // DAC_Enable(0, 44)
pa_on(v): out(80, in(80)&~2); out(9E, v&31); out(DD, in(DD)|0xC2);      // PA_Enable({1,v})

/* ---- start a file (mode 0 = play, 0xFF = scan for duration, 1 = resume) ---- */
start(file, mode):
    out(8B, 0x05)                                    // c_mp3decINIT
    out(42, (in(42)&0xE3)|0x0B); out(42, in(42)|0x40); delay_ms(2)
    out(2E, 0x7B); out(2F, 0x03); out(27, in(27)|1)  // reset held, clock on, DSP IRQ enabled
    load_adef("MP3DEC.DSP")                          // section 1.3 (DM every time, PM if lanes reused)
    out(05, 0x07)                                    // bits 7-4 = 0
    st = {mode, size=file.size, first=1, have=0, more=1, dir=0x11, fade=0, done=0}
    out(2E, in(2E)|0x80)                             // run

/* ---- DSP IRQ (26 bit 0), or poll it from the main loop ---- */
dsp_irq():
    in(35); r = in(35); out(26, 1); s05 = in(05)
    switch r:
    0x11: out(34, st.mode); [mode==1: out(05,34); write 3 saved bytes to 0x7E08]; out(05,s05&15); out(35,0x22)
    0xEE: step = in(32)|in(33)<<8; d = in(34)
          if d != st.dir: st.dir = d; st.have = 0
          if !st.have and st.more: st.more = read_sector(st.buf, step, d); st.have = st.more
          if !st.have: out(30,3); out(33,1); out(35,0xDD); break         // end of data
          if st.first: st.first=0
              out(05,0x24); [0x5C07]=in(9E); [0x5C03]=size.b0; [0x5C04]=size.b3
              [0x5D4D]=0x2F; [0x5D4C]=0x50; [0x5D50]=0; [0x5D4F]=0                   // lane L
              out(05,0x25); [0x5C03]=size.b1; [0x5D4D]=0x20; [0x5D4C]=0x01; [0x5D50]=0; [0x5D4F]=0  // lane M
              out(05,0x26); [0x5C03]=size.b2; [0x5D4D]=0;    [0x5D4C]=0                      // lane H
              out(05,s05&15)
          dma_to_dsp(st.buf)                         // section 5.2 table, poll 13 bit0
          st.have = 0
          out(30, st.fade); out(33, 1); out(35, 0xDD)
          st.more = read_sector(st.buf, 1, st.dir); st.have = st.more   // prefetch (optional)
    0xCC: info = in(30..34)
          out(05,0x24); total = {[0x5D52], [0x5C02], [0x5C01]}; out(05,s05&15)
          if st.mode == 0xFF: st.done = 1; stop(); break                // scan: no reply
          set_dac_rate(info[4]); out(35, 0x99)                          // section 3.2
    0xBB: out(05,0x24); now = {[0x5D57],[0x5D58],[0x5D59],[0x5D5A]|[0x5D5B]<<8}
          [0x5C07] = volume; [0x5D3E..0x5D48] = eq (optional)
          out(05,0); out(FB,in(FB)|0x80) ... out(33, in(FD)) ... (v04 ID bytes, section 4.2)
          out(34, paused_since_last ? 0 : 1); out(35, 0x00); out(05, s05&15)
    0xAA: st.done = 1                                # finished: stop()
    0x55: out(35, 0x66)          0xE0: st.done = 1
    0xA8: out(34, in(2F)); out(35, 0x95)
    0xA9: ok = in(34)==0; out(35, 0x96)
    0x77: if ((in(42)&0x1C) < 0x0C) out(42, in(42)+4); out(35, 0x88)
    0x2B: out(2E,in(2E)&~2); load_overlay(); out(2E,in(2E)|2); out(35,0x3B)   // never for MP3DEC v04
    out(05, s05 & 0x0F)

read_sector(buf, step, dir):   // plain "read next 512 bytes" callback
    if step > 1: skip (step-1 or step) sectors (inf)   ; if dir == 0x22: go back 1 sector first (stock REW)
    return read_next_512(buf)  // 0 at end of file

/* ---- pause / resume ---- */
pause():  st.paused = 1    // the IRQ handler stops answering (just ack 26 and return; keep r)
          optionally ramp 9E down; out(80, in(80)|2)
resume(): out(80, in(80)&~2); ramp 9E up; st.paused = 0; handle the kept request r now

/* ---- stop / exit ---- */
stop(): out(27, in(27)&0xFE); out(26,1); out(2E, in(2E)&0x7F); out(2E, in(2E)&0x7D); out(42, in(42)&0xBF)
close(): out(8B, in(8B)|0x10); pa_off(): out(D4,in(D4)|0x80); out(DD,in(DD)&0x3F); out(D5,in(D5)&0xE3);
         out(80,in(80)|2); delay; out(D4,in(D4)&0x7F)

/* ---- duration scan ---- */
duration(file): start(file, 0xFF); run the IRQ until st.done; return total     // full DSP load per file
```

Fixed DM parameter values are the v04 defaults:

- 0x1D4D = 0x00202F and 0x1D4C = 0x000150 (sentence RMS/time gates (inf)).
- 0x1D50 / 0x1D4F = 0 (start/end time).
- EQ flag 0 and bands 5.

The 0xBB extras (FB/FC/FD, tempo, EQ) can be copied verbatim. Whether MP3DEC needs the
FB/FC/FD value has not been tested.

**Emulator test of this sequence.** `atj2085emu/tools/test_dsp_loader.py` implements exactly
this driver in Python against the emulated ports, including the loader via port 05 pages, the
stock DMA descriptor and the DAC sequence. It plays the test files with PCM identical to a
direct decode.

---

## 9. Open questions (need the device)

1. The DMA1 byte count encoding (`10/11` = count−1?) and the lane/address order in DSP mode with
   down-count. Use the stock values verbatim.
2. Whether the DSP really sends rate codes with bit 7 set (0x88/0x8A/0x8B). A driver that decides
   by the low nibble is safe either way.
3. The meaning of `33 = 1` in the 0xDD reply, the FB/FC/FD value in the 0xBB reply, and HSR7
   bit 6 from the watchdog.
4. Whether MP3DEC uses DM words above 0x37FF other than 0x3E08-0x3E0A, and whether it touches PM
   0x2400-0x3FFF at run time. This is irrelevant while the halves must stay DSP-owned anyway.
5. FS_FSeek mode-1 step semantics for steps ≠ 1 (skip `step` or `step−1` sectors).
6. The ADEF checksum algorithm (not needed for loading).
