# OGG playback, voice recording above 8 kHz, FM RDS: what the stock code does and what zos needs

Scope: research only. I changed no zos sources, built nothing and ran nothing on the device.
Sources: the v04 firmware (`lisong_rebuild/build/fw/fwimage/*`, identical to `ref/myximpro/fwimage`; the MD5s of
MP3DEC, OGGDEC and PCMDEC match the user's `my-ximpro.fw`), the disassembly in `disasm_v04/{MUSIC,RECORD,VOICE,FMRADIO}_AP`,
and SYSBNKA.SYS (a page of 512 B at file offset page×0x200 is mapped at 0x0400). The Actions 913f SDK C sources are in
`ATJ2085/62720720AK1011_9_1_52_Nodrm/Firmware/sdk_i_913f/msrc/basal/` and `inc/codec.h` / `oggdecC.h`.
Helper scripts (dbdis.py, bnkdis.py, adef.py, oggtab.py, dmdump.py) were kept in the session scratchpad and are not in the repo.
The emulator was not used: its DSP is an MP3-only HLE (`atj2085emu/emu/dsp.py` docstring). A stock OGG run would stop
at StartDsp with no DSP requests, so it would show nothing beyond the static analysis below.

---------------------------------------------------------------------------------------------------------------

## 1. OGG Vorbis (OGGDEC.DSP)

### 1.1 Files
| file | size | content |
|---|---|---|
| `OGGDEC.DSP` | 83 456 B, ADEF, 163 sectors | DM 0x4000 words (DMLS/MS/HS sectors 1/33/65 ×32), PM 0x2C00 words (sectors 97/119/141 ×22). Overlay descriptors at +0x140 are all zero. Build date 2005-03-21. |
| `OGGTABLE.BIN` | 13 824 B = 3 × 0x1200 | a PM table, one 0x1200-byte block per lane (L, M, H); only the first 0x1198 bytes of each block are used |

Facts found by checking the binaries (adef.py / oggtab.py):
* **The OGGDEC DM image is all zeros except words 0x3E00-0x3E83.** Those words hold constants: "vorbis" and "OggS"
  as bytes; the sample-rate table 48000, -, 24000, 12000, 32000, -, 16000, 8000, 44100, -, 22050, 11025, indexed by the
  DAC rate code; and Q23 constants. So the DSP sets up its DM at run time.
* **OGGTABLE.BIN lane k equals OGGDEC PM lane k, words 0x1A49..0x2BE0 (0x1198 words), byte for byte** (4504/4504 for all
  three lanes). The PM image has a zero gap at 0x15CC-0x1A48. Conclusion: the DSP uses PM 0x1A49-0x2BE0 as scratch
  while it sets up the stream (codebooks), then asks the host to restore the table (request 0x2A below).

### 1.2 Stock driver (v04 MUSIC.AP)
* OGG basal: bank 0x2D, window 0x2020. Load at 0x207F, play at 0x20F9 (`oggPlayCommand`), caltime at 0x2020.
  It matches the SDK `basal_ogg_de.c` / `basal_ogg_de_b.c` / `basal_ogg_de_rd.c`.
* Codec driver: bank 0x1A (data + helpers at 0x2400-0x24A1), bank 0x1B (`oggdecInit` at 0x24C4), bank 0x1C (0x24C0-0x2974:
  StartDSP spin, IRQ handler, DMA). The OGG driver is a "decXXX" codec (913f generation, `codec.h` structs). This is
  **not** the mp3dec51 family that MP3DEC uses.

RAM layout (bank 0x1A data) = `decInitPara_t` at 0x2400, `decControlInfor_t` at 0x240C, hook at 0x2425, `decCurStatus_t` at 0x2429,
`decReadFilePara_t` at 0x2430 (dir 0x2430, pages 0x2431-34, SetAB 0x2435, ReadAddr 0x2436), `decFileInfor_t` at 0x2448.

**Load (bank 2D 0x207F):** read hook = 0x1C00 (`m_oggdeRead_Data`), SongType = 0, BitsreamBuff = 0x1DFC, TagInforLen = 0,
TimeLimit = 0x150, EnergyLimit = 0x22F. Then `FS_FOpen`, and **PosOfAPoint = file length in 512-byte sectors** (`FS_GetFileLen`).

**Play (0x20F9):** TellDecDoWhat = 0 (from the start). For a resume, TellDecDoWhat = 1 and PosOfAPoint = the resume sector.
The basal then calls `FS_FSeek(0)` (**always from the file start**: Vorbis needs the three header packets), `IRQ_Intercept(0x24D9)`,
`oggdecInit`, then `TM_SetTimer(0x28CF, 2)`, then `oggdecStart`.

**`oggdecInit` (1B:24C4):** clears 0x1E state bytes at 0x2429 and puts the buffer address into the DMA descriptor. Dir = 0x11,
pages = 0. It prefetches one sector through the hook (`call 0x248A` = `jp (hook)`), sets have = 0xFF, and calls
**`InitDSPCode("OGGDEC.DSP", C=0)`** (DE = 0x2472). It does **no RestDsp and no DAC setup**: the AP has already run
DAC_Enable / PA_Enable for all codecs (mp3_driver_spec 3.1).

**`oggdecStart` (1C:24C0):** OS **`StartDsp(E=2)`** = 36 MIPS. The full clock/reset/IRQ-enable/run sequence is in mp3_driver_spec §2.
It then spins until 0x2438 = 0xFF, which the 0xCC handler sets.

**IRQ (1C:24D9):** `out(26,1)`, `in(35)` (read once), port 05 saved/restored, then dispatch. While paused (0x243A = 0xFF)
requests are not answered. They stay latched and the 2-tick timer answers them later (1C:2964), the same idea as zos `pend`.

| req | handler | host action | reply |
|---|---|---|---|
| 0x11 start | 1C:25D5 | HDR4 = TellDecDoWhat (0 play, 1 resume, 2 caltime). DM **0x00AF** L/M/H (port05 0x24/25/26, addr 0x40AF) ← PosOfAPoint bytes 0-2 (file sectors, or resume sector) | 0x22 |
| 0xEE data | 1C:260A | `out(33, fade)` (0 normal). step = DM 0x00AF L/M/H (24 bit), dir = HDR4 (0x11 forward / 0x22 back). It adjusts step by ±1 for the prefetched sector (have flag 0x243B), and if step ≠ 0 calls the hook (seek + read 1 sector). **Read fails (EOF/error): no DMA, no reply.** Otherwise DMA 512 B with port05 = 0x24 | 0xDD, then prefetch the next sector (pages = 1) |
| 0xBB status | 1C:26EA | time ← DM 0x00B3 L/M/H = **s, min, h**. DM 0x00B5 ← volume (0x240C), DM 0x00B6 ← EQ (0x2412). A-B: DM 0x00B0. FF/REW: DM 0x00B1/0x00B2 | 0x00 |
| 0xCC format | 1C:282F | sets 0x2438 = 0xFF. **Caltime mode:** bitrate[3] ← DM 0x00B4 L/M/H, total s/m/h ← DM 0x00B3 L/M/H, **no reply** (the basal then exits the DSP). **Play mode:** rate = HDR4 & 0x0F = DAC code → `DAC_SetRate(table[code])` (table 0x245A = 48,0,24,12,32,0,16,8,44,0,22,11 kHz), HDR4 = 0 | 0x99 |
| 0x2A table | 1C:2580 | `2E &= FD` (DSP clock off), **GetOggTable("oggtable.bin")** (API 0x0125), `2E |= 02` | 0x3A |
| 0x2C | 1C:256F | HDR4 = 0 | 0x3C |
| 0xAA end | 1C:259C | HDR4: 0x30 = normal end (status 0, also `37 |= 40`), 0x10 → status 0xFC, 0x20/0x21/0x22 → 0xF0 (format error) | none |
| 0xE0 | 1C:255E | fade/exit ack | 0xE1 |
| 0x77 | 1C:254F | `SetDspMips(E=3)` = straight to 48 MIPS | 0x88 |

**GetOggTable (SYSBNKA page 0x25 fn1 = 0x04F2):** opens the FWIMAGE file. For lane i = 0..2: `out(05, 0xC0+i)` (IPM lane i,
both halves to the MCU), seek to i×0x1200, read **0x1198 bytes to 0x5A49** (= PM words 0x1A49..0x2BE0). Then it restores port 05.
Constants at 0x0589 = 0x5A49, 0x058B = 0x1198, 0x058D = {0, 0x1200, 0x2400}.

**Bitstream DMA (descriptor 1A:247D, sent to ports 06..12):** SA = buffer, SA2 = 0, SA3 = 0 (ZRAM1), ISA = 0, **DA = 0x000002,
DA3 = 0x40, IDA = 0x04 (IDM-L), count 0x01FF, mode 0x20** (3-lane DSP mode, ascending). Start `13 = 1`, poll bit 0.
Port 05 = 0x24 during the DMA. The only differences from MP3: MP3 uses DA 0x1CB3, IDA 0x06, mode 0x30 (descending).

**Read hook semantics (SDK `m_oggdeRead_Data`):** pages = 0 → `FS_FSeek(0, cur)` then read; forward and pages = 1 → read the next
sector; forward and pages = N → `FS_FSeek(N, relative forward)` then read; backward → `FS_FSeek(N, relative back)` then read.
**The Z80 does no Ogg page parsing.** The DSP finds OggS pages, headers and granules itself, and only tells the host which
512-byte sector to send next.

**Time / seek / end:**
* Current time comes in every 0xBB (DM 0xB3).
* The total time comes **only from a separate caltime pass** (TellDecDoWhat = 2: DSP load, run, read 0xCC, stop).
* Seek = restart with mode 1 and PosOfAPoint = target sector. The DSP reads the headers from sector 0 and then asks for a jump.
* End = 0xAA with code 0x30. The DSP knows the file length from the 0x11 parameter, so a normal end never hits a failed read.
* Channels are never reported. Bitrate comes only in caltime.

**Watchdog (1C:28F0):** if no request arrives for about 8 timer ticks after a reply, the driver sets HSR7 (`37`) bit 6.
Inferred from ADSP-2111 HIP conventions: this is a DSP software reset / "host gave up" signal. A 7th timeout sets status
0xF0. zos does not need it.

### 1.3 What zos needs (vs MP3PLAY)
Reusable as is: ADEF lane loader (`load_dsp`, parameterise the name), `analog_on`, the DAC-rate code at 0xCC (OGG sends the
low nibble only, so use `code & 0x0F`, as mp3.c already does), `pll_change`, pause/pend, stop/off, the ZRAM1 DMA trampoline.

New or different:
1. DSP start: the DSP clock comes up the stock way (zos already does RestDsp + DSPCTL_RUN; StartDsp(E=2) is equivalent).
   0x77 → set MIPS to 48 (`PLL_MIPS_48`), reply 0x88.
2. DMA template: patch DA = 0x0002, IDA = 0x04 (IDM-L), mode = DMA_MODE_DSP (no DSTDN). There are 4 immediate bytes in mp3dma.s,
   so patch the ZRAM1 copy after the memcpy.
3. Mailbox addresses: DM 0x00AF / 0x00B3 / 0x00B4 / 0x00B5 / 0x00B6. They use the same `PAGE_IDMLO_x` + rd8/wr8 as MP3, but at
   0x40AF.. instead of 0x5Cxx/0x5Dxx.
4. 0xEE: read the 24-bit step from DM 0xAF (3 lanes) plus dir from HDR4. Keep the stock prefetch adjustment
   (have && same dir → step-1; dir changed → step+1). If step = 0, send the prefetched sector. Otherwise `fat_seek(pos ± (step-1)·512)`
   then read one sector. Write HDR3 = 0 (fade), DMA, reply 0xDD, then prefetch. On a read failure (EOF): no reply, and stop after
   about 0.3 s if no 0xAA arrives.
5. 0x11: HDR4 = 0 (or 1 for a seek/resume), DM 0xAF ← sector count (play) or start sector (seek/resume). Reply 0x22.
6. 0x2A: DSP clock off (`DSPCTL &= ~DSPCTL_CLKEN`). For each PM lane, `page_write(PAGE_IPM_x)` 0x1198 bytes at 0x5A49 from OGGTABLE.BIN
   offset lane×0x1200, or from OGGDEC.DSP PMxS sector base + 0x1A49 (no extra file needed). Clock on, reply 0x3A. That is 27 sector
   reads, ~30 ms. **How often 0x2A comes is unknown** (probably once per start): check it on the device with a counter.
7. 0x2C → HDR4 = 0, 0x3C. 0xE0 → 0xE1. 0xAA → done (code 0x30) or error (0x2x). 0xBB → time from DM 0xB3, volume → DM 0xB5, reply 0x00.
8. Total time: **skip the stock caltime pass** (a second 163-sector DSP load). Do it on the Z80 instead: (a) rate = u32 at offset 12 of
   the identification packet (first page, after "\x01vorbis"); (b) read the last ~4-8 KB of the file, find the last "OggS",
   granule = u32 at page offset 6 (the low 32 bits are enough: 27 h at 44.1 kHz); (c) total = granule / rate with the existing `udiv`
   (the quotient fits 16 bits). Time display = `base_s + now` as for MP3, with base from the seek target.
9. Tags: Vorbis comment header (second packet, page 1 or 2): `TITLE=` / `ARTIST=`, length-prefixed UTF-8. This replaces id3.c.
10. DSP RAM: OGGDEC uses all 16K DM and PM 0-0x2BFF. Like MP3, it must be a ZRAM2 module with every lane DSP-owned.

### 1.4 Size and structure
MP3PLAY has about 41 B ZRAM2 and 71 B CODE2 free, so OGG cannot go inside it. Estimates (SDCC, like mp3.c):

| part | bytes |
|---|---|
| OGG request handler (0x11/EE/BB/CC/2A/2C/AA/E0/77) incl. 24-bit step + seek | 450-550 (hot, ZRAM2) |
| 0x2A table restore | 80-120 (cold) |
| duration (first-page rate + last-page granule) | 200-280 (cold) |
| Vorbis comment TITLE/ARTIST | 150-200 (cold) |
| DMA template patch + DSP name | 20 |
| **OGG-specific total** | **~0.9-1.2 KB** |
| MP3-specific code it replaces (REQ_SENT/CHIP/VERDICT, CHIPID quirk, gate params, id3.c) | ~0.5-0.6 KB |

Recommendation: **a separate OGGPLAY.ZM (ZRAM2 module) built from the MP3PLAY sources.** main.c/ui.c stay shared through
`#include "../mp3play/…"` or a codec switch; ogg.c / oggcold.c / vorbis.c replace mp3.c / mp3cold.c / id3.c.
* The net growth over MP3PLAY is ~+0.4-0.6 KB. The cold parts go to CODE2, where a fresh module has the full budget.
  Move the duration/comment code into CODE2 first. If it still does not fit, run the comment parser from the 512-byte `bs`
  buffer before the DSP starts (it is a one-shot).
* MUSIC: accept `*.OGG` and add a type bit per playlist track (mus_trk_t).
* MP3PLAY ↔ OGGPLAY chain through `MOD_CHAIN_NAME` when the next track's type changes (the playlist is already in HAL_SHARED).
* A shared "decoder-agnostic core" is only worth it if WAV (section 2) is added too. OGG and PCMDEC use the same decXXX protocol
  (only the DM addresses, DMA descriptor and DSP name differ), so one **DECPLAY.ZM** with a 12-byte codec descriptor
  {name, DMA DA/IDA/mode, port05 page, DM step/time/vol addresses} can play OGG + IMA-WAV for about +80 B over OGG alone.

Effort: ~2-3 days to code plus device bring-up (bring-up risks: the 0x2A frequency, the EOF behaviour, the 48 MIPS clock change
with a 48 MHz CPU, already handled by `pll_change`).

---------------------------------------------------------------------------------------------------------------

## 2. Microphone recording above 8 kHz, and playback

### 2.1 Stock recorder (RECORD.AP, also FMRECORD/LINE, and MUSIC bank 0x0F)
* DSP programs referenced: `ADENCM.DSP` (type 0x82), `AG8ENC.DSP` (0x84), `AG4ENC.DSP` (0x83, not in FWIMAGE). Selection in RECORD
  bank 0B 0x2990. `MP3ENC.DSP` and `ADPCMENC.DSP` are in FWIMAGE but **no v04 AP references them** (strings scan of all APs).
* Basal (SDK `basal_adag_en.c`): **sample_rate = 8 (kHz), channel = 1**, gain 3, polling-mode writes, buffers `ADBuffer[1024]` +
  `DataBuffer[512]`. `FS_FCreate`, then `FS_FWrite` of 1 placeholder sector. Every encoded 512 B → `FS_FWrite`. At stop the header
  sector comes from the DSP and the stock code reopens the file and rewrites sector 0.
* **ADENCM writes standard IMA ADPCM WAV.** Its DM template (words 0x3D.., 3 bytes per word, LE) reads: `RIFF … WAVE fmt `, fmt
  size 20, **wFormatTag 0x0011 (IMA/DVI ADPCM)**, 1 channel, 8000 Hz, 4000 B/s, blockAlign 512, 4 bits, cbSize 2,
  **samplesPerBlock 1017**, then `fact`, `data` and a LIST/INFO "ACTIONS" chunk. The DM also holds the IMA index and step tables
  (dmdump.py on ADENCM.DSP).
* Driver (RECORD bank 0C), protocol:
  * `adagencStart`: `D7 = (D7 & FC) | CD` (ADC IRQ on, FIFO run/sync, threshold), `27 |= 80`, `StartDsp(E=1)` (24 MIPS).
  * ADC ISR (0C:2A18): `in(9A)` twice per sample (hi, lo) into a 2-half buffer while `D7 & 20`.
  * DSP IRQ: 0x11 → HDR4 = flag, reply 0x22.
  * 0xEE → reads DM 0x0FFB.. (5 bytes) with port05 = 0x24. If a half buffer is full: DMA (desc 0A:2844) **SA = half buffer,
    DA = DM 0x1200, IDA = 4 (IDM-L), count = 0x13F (320 B = 160 samples), mode 0x20**, then HDR4 = 0, reply 0x55
    (stop: HDR4 = 0xAA, 0x55).
  * 0x88 → DMA (desc 0A:2837) **SA = DM 0x1000 lane IDM-L (SA3 0x40, ISA 4), DA = Z80 buffer, 512 B, mode 0** (one byte per
    DSP word). If HDR4 = 0x99 it is the final header sector. Otherwise reply 0x77 once the main loop has written the previous one.
  * Init (0B:2929): InitDSPCode by type; encoder params → DM 0x0FF6..0x0FFA (low lane); ADC via API 0x0026 ADC_Enable
    {src, rate kHz, gain, chmode, precision}.

### 2.2 PCMDEC.DSP (playback of recordings)
* PCMDEC (ADEF, PM 0x600, DM 0x200) holds the same IMA tables plus a `RIFFWAVEfmt data` byte template. Its rate table
  48000..11025 is indexed by DAC code, the same as OGG. It is loaded for SongType 0x82 = the Actions ADPCM recordings, which
  §2.1 shows are **IMA ADPCM WAV (0x0011)**.
* There is **no evidence that it decodes linear PCM (tag 1)**: no code path seen, and the stock never records PCM. Treat
  PCMDEC as "IMA ADPCM WAV player, 8-48 kHz". Linear PCM via PCMDEC is unverified; test it on the device (a 16-bit WAV
  either plays or ends with 0xAA code 0x2x).
* Host protocol = the decXXX family (VOICE.AP bank 09, shared with WMA/AG8). It is very close to OGG:
  * DSP image named by the driver; `StartDsp(E=0)` (12 MIPS).
  * Mailbox DM **0x1264-0x127B**: step = DM 0x1267-0x1269 read through page 0x26, volume → DM 0x127A (page 0x24),
    total/format at 0xCC from DM 0x1264-0x1269.
  * 0xCC: HDR4 & 0x0F → DAC_SetRate, HDR4 = 0, reply 0x99.
  * 0x11: HDR4 = mode; resume position → page 0x26 at 0x4100..0x4103.
  * 0xAA end codes as in OGG. 0x2C → 0x3C, 0x77 → 0x88, 0xE0 → 0xE1, 0x55 → 0x66.
  * Bitstream DMA (desc VOICE 07:247D): **DA = DM 0x0100, DA3 0x40, IDA 0x06 (IDM-H), 512 B, mode 0** (one byte per word),
    with port05 = 0x26.
  * Memory: PM 0-0x5FF overlaps the page-module lane C1 window (0x4000-0x45FF), so the player must be a ZRAM2 module
    (OGGPLAY/DECPLAY).

### 2.3 Rates, formats, data rates
ADC rate field D5.7-5: 0x00 8k, 0x20 12k, 0x40 16k, 0x60 24k, 0x80 32k, 0xA0 48k (48k family, PLL_AUDIO = 12.288 MHz). With
`PLL_AUDIO_441` the same codes give ×0.91875: **0x60 → 22.05 kHz**, 0xA0 → 44.1 kHz, 0x20 → 11.025 kHz (capture.c already
notes the 29.4 kHz effect). The microphone is mono (right-only FIFO reads, D7.4).

| format | 16 kHz | 22.05 kHz | 32 kHz |
|---|---|---|---|
| 16-bit PCM WAV (tag 1) | 32 kB/s, 1.9 MB/min | 44.1 kB/s, 2.6 MB/min | 64 kB/s, 3.8 MB/min |
| IMA ADPCM WAV (tag 0x11, 512-B blocks) | 8.1 kB/s | 11.1 kB/s | 16.1 kB/s |

**Recommendation: IMA ADPCM WAV, mono, 22.05 kHz by default (16 / 32 kHz selectable).**
* It is the stock format at a higher rate, so PCMDEC plays it on the device and Windows/VLC play it on the PC.
* 4:1 smaller files.
* Playback uses the clean DSP path, with no Z80 DAC feeding.
* Use 16-bit PCM WAV only as an optional "HQ" mode played by a Z80 DAC feeder. Mono 22.05 kHz is 44 k DAC word-pairs/s,
  less than half the 48 kHz stereo load that was poor over USB, but the FS reads compete with it, so it stays a second step.

### 2.4 NAND write throughput (derived; nothing on the device measured it)
* NAND: K9K8G08-class SLC (READ ID EC D3 51 95). Datasheet-class timings: tPROG 200 µs typ / 700 max, tR 25 µs,
  tBERS 1.5-2 ms.
* `ftlw` writes per 512-B sector (partial-page program) into the temp unit. A unit (256 KB = 512 sectors = 16 clusters of
  16 KB) closes with 256 page copy-backs + 2 two-plane erases.
* Per unit: 512 × (~80 µs DMA + spare + tPROG) ≈ 140 ms typ / 400 ms max, plus the close ≈ 60 ms typ / 190 ms max.
* **≈1.1 MB/s typical, ≈0.45 MB/s worst.** The vendor quotes 955 KB/s for stock MSC writes, the same FTL algorithm.
* Need: 11-64 kB/s, so **headroom is 7-100×**. Bandwidth is not the problem; **latency is**:
  1. Interrupts are off while CE1 is selected: one program ≤0.7 ms, one erase ≤2 ms. The ADC FIFO is 12 words =
     375 µs at 32 kHz, 545 µs at 22.05 kHz, so a pure IRQ/poll capture loses samples there.
  2. The unit close blocks the caller for 60-190 ms (more on a file's first/last partial unit, where the old data must be
     copied: up to ~400 ms). That is 2.6-17 KB of 16-bit PCM at 22.05 kHz.

**Capture design that survives both:**
* **(a)** Add an idle hook to the ftlw ready-wait loop (`ftlw.inc` ready wait, UD 0x39FB equivalent). The hook is a ZRAM1
  routine of ~30 B that drains the ADC FIFO into the ring. It is safe with CE1 selected: it touches only ports D7/9A and RAM,
  and only between NAND bus cycles.
* **(b)** Put the PCM ring in DSP RAM (no DSP runs while recording if the Z80 encodes): e.g. IDM-L/M/H + IPM-L = 4 × 16 KB
  = 64 KB = 1.5 s at 22.05 kHz. The hook switches port 05 to the ring lane and back (it runs from ZRAM1, like `mp3_dma_ram`).
* **(c)** The main loop drains the ring through the IMA encoder into 512-B blocks and calls `ftlw_write`.
* A recorder as a page module (lane C1) keeps 16 KB of code and leaves 4-5 DSP lanes for the ring.
* No core change is needed. An ADC IRQ would need a core ISR hook, and the core RAM has 6 B left.

**Encoder choice:**
* **Z80 IMA ADPCM encoder in asm.** ~250 B, ~200-300 T-states/sample, ≈ 20-27 % CPU at 22.05 kHz / 24.576 MHz, 30-40 % at
  32 kHz. Recommended: no DSP while NAND is busy, and DSP RAM stays free for the ring.
* **Alternative: the stock ADENCM.DSP** with the protocol above, the ADC rate raised, and the header bytes 24-31 (rate,
  byte rate) patched in the final header sector. The DSP does not get the rate: ADENCM's template is fixed at 8000 Hz.
  Zero Z80 CPU, but the DSP and its 0xEE/0x88 traffic must keep running while ftlw blocks. That needs the same idle hook
  to service mailbox and DMA, or an encoded-sector ring in the IPM high half (pages 0x4x; ADENCM uses PM 0-0x3FF only).
  More moving parts; keep it as the fallback.

### 2.5 Creating the file safely (FAT16)
The volume: FAT16, 32 sectors/cluster (16 KB), **2 FATs × 242 sectors**, 512 root entries, 61 919 clusters
(`ftlread.py info`).

Write order, so that a crash never corrupts existing data:
1. Read the BPB (sector 0). Scan FAT1 (242 sectors via hal_blk_read) for the **largest run of free clusters**. Contiguous
   data means sequential `ftlw` units and the fewest closes. Keep the allocation in RAM.
2. Name: `RECnnnn.WAV` in the root (fixed root, no directory cluster to allocate; a `/RECORD` subdirectory can come later).
   Pick the first free root slot (0x00 or 0xE5). Optionally write the 8.3 entry now with size 0 and cluster 0: a crash
   then leaves an empty file and no orphans.
3. Stream data sectors (sector 0 = placeholder header) into the run. If the run fills up, continue in the next run and keep
   the chain in RAM (a 30-60 min recording at 11 kB/s = 20-40 MB = 1.3-2.5 k clusters; only store run starts/lengths).
4. Stop:
   1. Write the final header into sector 0 (RIFF size, data size, fact sample count).
   2. `ftlw_flush`.
   3. Write the **FAT chain into FAT1, then the same sectors into FAT2** (+242).
   4. Write the **directory entry** (first cluster, size, date/time).
   5. `ftlw_flush`.
   6. `fat_mount()` or drop the core FAT cache; `ftlw_sync` before rereading anything just written.
* The FAT/root area sits in the first 1-2 FTL units, so a commit costs one or two unit closes (~0.1-0.4 s), which is fine at stop.
* Do **not** commit periodically during recording: every commit closes the data unit plus the FAT unit.
* A crash before commit = the data clusters stay free, so the recording is lost but the disk is consistent (optional entry from
  step 2 = a 0-byte file).
* Check free space up front: the run length bounds the recording time; show it in the UI.
* Size: FAT writer (BPB, free-run scan, chain to both FATs, dir slot/entry, header patch) ≈ 0.7-0.9 KB C.
  IMA encoder 0.25 KB asm. ADC setup (reuse capture.c's `cap_adc_on` with a rate argument) 0.15 KB. Ring + hook 0.15 KB.
  UI 0.5 KB. **REC.ZM ≈ 2-2.5 KB page module.**
* Playback: add the PCMDEC codec descriptor to DECPLAY/OGGPLAY (+~80-150 B) and let the browser accept `*.WAV`.
* Effort: recorder 3-4 days, WAV playback 1 day after OGGPLAY.

---------------------------------------------------------------------------------------------------------------

## 3. FM tuner: which chip, and is RDS possible?

### 3.1 Stock driver
* `FMRADIO.AP` bank 0x12 (0x2500-0x2BCD) is a single TEA5767 driver. Its labels are the SDK `FM_i2cdrv1/2.msa` names:
  `WRITE_ONEFRAME`, `READ_ONEFRAME`, `CALCULATE_PLL`, `FM_setfrequency` with the HLSI retry, `testFMi2c`.
* The only I2C addresses in the AP are `ld a,#0xC0` (0x282D, 0x285D, 0x2B0B) and `ld a,#0xC1` (0x2895). No other tuner AP
  or driver exists; the SDKs' ap_radio sources hold only the TEA5767 driver.
* **No chip-ID read and no second tuner type.** The "probe" (`testFMi2c` 0x2AFA) only checks for an ACK at 0xC0 after 200 ms.
* zos FM and I2CTEST use the same 0x60 5-byte protocol.
* I2CTEST already scans 0x03-0x77 (bit-bang) and prints every ACKing address. The only recorded device result is
  "0x60 → ACK, 0x50 → NACK" (hardware-I2C diag comment in i2ctest/main.c). A full scan list from the device is not on record.

### 3.2 Candidates
* RDA5807SP/P, the usual cheap "TEA5767-compatible" part: it answers on **three** I2C addresses at the same time. 8-bit
  0x20/0x21 (7-bit **0x10**, sequential access, reads start at register 0x0A), 0x22/0x23 (7-bit **0x11**, random access
  with a register index), and 0xC0/0xC1 (7-bit **0x60**, TEA5767 5-byte mode).
* RDA5807 chip-ID: register 0x00 = 0x58xx on the M/FP parts; the SP programming manual gives registers 0x0C/0x0D = 0x5804.
* The RDA5807SP/M have an RDS/RBDS decoder: register 0x02 bit 3 RDS_EN, 0x0A bit 15 RDSR, blocks A-D in 0x0C-0x0F.
* A genuine TEA5767, RDA5767 or other clones (BK1080, AR10xx, KT08xx, …) have **no RDS**.

### 3.3 Probe the user can run (read-only, harmless)
1. **I2CTEST, PLAY (scan):** if the list shows **10 11 60** (or 10/11 next to 60), it is an RDA580x. If it shows only
   **60**, it is a TEA5767/RDA5767-class part with no RDS: stop here.
2. If 0x11 ACKs, do a random read of register 0x00: START, 0x22 (0x11<<1 write), 0x00, repeated START, 0x23, read 2 bytes
   (ACK, NACK), STOP. Expect **0x58 0x0x**. Cross-check with a sequential read at 0x10: START, 0x21, read 8 bytes = registers
   0x0A, 0x0B, 0x0C, 0x0D; on the SP, 0x0C/0x0D read 0x5804.
3. Add this as an "ID" key to I2CTEST: ~80 B, reusing `i2c_start/i2c_tx/i2c_rd/i2c_stop`. It needs one routine that does a
   repeated START without a STOP.

### 3.4 Is RDS possible?
* **Only if step 1 finds 0x10/0x11.** Then drive the chip in RDA native mode (registers 0x02-0x05: enable, RDS_EN, band,
  channel, tune/seek) instead of the TEA 5-byte frames.
* Poll 0x0A bit 15 (RDSR) every ~40 ms. RDS sends 11.4 groups/s, about one per 88 ms. Read 0x0C-0x0F (8 bytes) and decode
  group 0A/0B (PS name, 8 chars) and 2A (RadioText, 64 chars). The Z80 cost is negligible.
* The bit-banged bus at ~100 kHz is fine. Do the transfers in small pieces between `pump()` calls, as the FM module already
  does for USB OUT.
* Size: RDA driver (init, tune, seek, status) 300-400 B plus RDS decoder (PS + RT, block-error flags) 250-350 B plus UI line
  100 B ≈ **0.7-0.9 KB** in the FM page module. Effort 1-2 days after the chip is confirmed.
* Keep the TEA path as the fallback for boards that only ACK 0x60.

### 3.5 zos implementation (FM module, 2026-11-10; emulator-verified, device-untested)
Files: `zos/modules/fm/tuner.c` (I2C + both drivers), `rds.c` (decoder), `main.c` (UI); emulator model
`atj2085emu/emu/i2cbus.py` (`--tuner tea|rda|none` in `headless.py` and `zos/tools/emu_nand_zos.py`).
* **Probe** (after the 200 ms pin settle): random read of 00h at 0x11 (START 0x22, 0x00, repeated START 0x23,
  2 bytes, STOP); high byte 0x58 and an ACK for a sequential read at 0x10 (0x21) = RDA, native mode. Else an
  ACK at 0x60 = TEA5767 (5-byte frames, unchanged). Else "NO TUNER". An RDA is never driven through 0x60.
* **Init:** 02h = DHIZ|DMUTE|SOFT_RESET|ENABLE (0xC003), 50 ms, 02h = DHIZ|DMUTE|RDS_EN|NEW_METHOD|ENABLE
  (0xC00D), 04h = 0x0800 (DE: 50 us, as the TEA path's DTC=0), 05h = 0x888F (INT_MODE, SEEKTH 8, LNA_PORT_SEL
  LNAP, VOLUME 15 = max; the PA volume stays the user control). All writes: random access at 0x11
  (index, then big-endian words). Power down: 02h = 0 (ENABLE 0, DHIZ 0 = outputs high-Z).
* **Tune:** 03h = (f - 87.0 MHz)/100 kHz << 6 | TUNE (0x10); BAND 00 = 87-108, SPACE 00 = 100 kHz.
  **Seek:** 02h |= SEEK (0x100) | SEEKUP (0x200), SKMODE 0 = wrap; poll every 30 ms; READCHAN (0Ah 9-0) moves
  the frequency display; STC (0Ah b14) = done, SF (b13) = failed; a key writes 02h without SEEK and retunes to
  the shown channel.
* **Status** (sequential read at 0x10 starts at 0Ah): 0Ah b15 RDSR, b14 STC, b13 SF, b12 RDSS, b11 BLK_E,
  b10 ST, b9-0 READCHAN; 0Bh b15-9 RSSI (0-127; bars use RSSI/8 as the TEA 0-15 level), b8 FM_TRUE,
  b7 FM_READY, b4 ABCD_E, b3-2 BLERA, b1-0 BLERB (0 = clean, 1 = 1-2 corrected, 2 = 3-5, 3 = uncorrectable).
* **RDS:** every 40 ms read 0Ah's high byte; only with RDSR set continue to 12 bytes (0Ah-0Fh = status +
  blocks A-D). Drop groups with BLERB > 1 and back-to-back identical reads; PI from A only with BLERA <= 1
  (a new PI clears everything). The M reports no error level for C/D, so a PS / RT segment is accepted only
  when two separate groups carry the same characters. 0A/0B: PS segment = B b1-0, chars in D, shown when all
  4 segments are confirmed (PI shown until then). 2A: segment B b3-0, 4 chars C+D (64); 2B: 2 chars in D (32);
  A/B flag B b4 toggles = new text. CR (0x0D) ends the text. Display: lower case to capitals, the EBU accented
  Latin letters (0x80-0x9F, 0xC0-0xDF) to their base capitals, other chars dropped (font = 0x20-0x5A).
  Cleared on every tune / seek.
* **Size:** FM 10425 -> 12905 B code, 755 -> 996 B RAM (13.9 of 16 KB page).
* **To check on the device:** chip ID value; whether 0x11 random access works on the RRD-102 (some RDA
  clones only have 0x10: then switch rda_wr to 0x10 sequential writes of 02h..05h); seek speed / SEEKTH 8;
  whether RDSR clears on read (the decoder tolerates both); audio level vs the TEA at volume 15; DE 50/75 us
  (Chile and the Americas use 75 us: 04h = 0); I2C timing (~100 kHz bit-bang, unchanged).

## Sources (web, for the RDA5807 facts)
* [RDA5807SP ProgManual 2.5](https://www.mikrocontroller.net/attachment/247637/RDA5807SP_ProgManual_2.5.pdf) (addresses 0x20/0x22/0xC0, chip ID 0x5804)
* [RDA5807P ProgManual 1.0](https://www.mikrocontroller.net/attachment/247618/RAD5807P_ProgManual_1.0.pdf) (RDA mode vs TEA5767 mode by I2C address)
* [RDA5807SP datasheet](https://mikrocontroller.net/attachment/180225/RDA5807SP.pdf)
