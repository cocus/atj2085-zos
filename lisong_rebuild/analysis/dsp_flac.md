# FLAC on the ATJ2085: DSP feasibility report

Scope: research only. Nothing in zos was modified, nothing run on the device. Binary claims give file + offset/word index.
Sources: `atj2085_hw.md`, `dsp_interface.md` (incl. the recently added sections 8-11 by another tool, re-checked below),
`mp3_driver_spec.md`, ATJ2085X datasheet v1.8 and Programming Guide v3.1 (pypdf text), `orig/fwimage/*.DSP`, the SDK folders.
"Verified" = I checked it against the binary/PDF in this session. "Unverified" = taken from another document.

## 1. The DSP

**Core.** "24-bit Harvard architecture DSP with on-chip ICE", 16K x 24 PM, (16K-256) x 24 DM, memory-mapped registers
including the DAC, up to 72 MIPS (datasheet v1.8 section 4.3, text p.12). PLL steps 12/24/36/48/60/72/84 MIPS
(PG port 0x42 bits 4-2, `atj2085_hw.md`). Datasheet section 6.2 gives DSP current at 6..84 MIPS, so 84 is a real setting
(60 MIPS and up need VDD 2.3 V). The stock MP3 driver runs it at 36 MIPS (`mp3_driver_spec.md` RestDsp, `42 = ...|0x0B`
-> field 010 = 36) and raises it on DSP request 0x77.

**Instruction set: ADSP-218x-compatible encoding (verified, strong).** My own check (not IDA), on the ADEF images
`orig/fwimage/{PCMDEC,MP3DEC,OGGDEC,AG8DEC}.DSP`, PM word i = PMLS[i] | PMMS[i]<<8 | PMHS[i]<<16:
- Vector table at PM 0x0000-0x002B, 4 words per slot as on the 218x. Word 0 is the reset vector:
  PCMDEC/OGGDEC/AG8DEC `18030F`, MP3DEC `1C8E6F`. Every other slot is `0A001F` except slot 0x24 (the HIP/IRQ0 entry:
  PCMDEC `18265F`, OGGDEC `18319F`, AG8DEC `1809FF`, MP3DEC slot 0x28 `1901AF`); the remaining words are `000000`.
  `0A001F` is **RTI** and `000000` is NOP in the 218x encoding; `0A000F` (RTS) occurs 69/88/180/88 times.
- 218x format: `0x18xxxx` = JUMP, `0x1Cxxxx` = CALL, addr14 in bits 17:4, condition in bits 3:0 (0xF = always).
  So the reset words are `JUMP 0x30` / `CALL 0x08E6`. All 244/244 jump/call targets in PCMDEC and 359/359 in AG8DEC fall
  inside the loaded PM; MP3DEC 919/932, OGGDEC 1006/1063 (the rest are probably data words that look like opcodes).
- Caveat on the other tool's IDA notes (`dsp_interface.md` section 11): they say `0A001F` decodes as "if neg call $3000". With
  the 218x tables I know, `0A001F` is RTI; I could not re-run IDA, so treat IDA's mnemonic output as suspect until
  compared with a second decoder. The "calls to $3000/$3371/$3041 outside PM" are then most likely IDA mis-decodes, not
  ROM entry points (unverified either way).
- Not stock 218x: DM words are 24 bits wide (datasheet; DM images are three byte lanes), so the data bus and probably the
  multiplier are wider than 16x16 (TROM is Q23, `trom_21k.bin`). The MAC width, register set additions and the peripheral
  map (0x3FED/0x3FEE) are Actions-specific and unverified at instruction level.

**Memory.** PM 0x0000-0x3FFF, DM 0x0000-0x3EFF plus registers at 0x3F00-0x3FFF (DAC 0x3FED, status 0x3FEE; PG DAC section,
PG text near "0x3FEDh"). Images actually loaded: PCMDEC PM 0x600 / DM 0x200 words; AG8DEC PM 0xE00 / DM 0xC00;
MP3DEC PM 0x2400 / DM 0x3800; OGGDEC PM 0x2C00 / DM 0x4000 (all from the ADEF tables, verified:
section sizes at file offsets 0x10-0x3F of each `.DSP`). So a codec up to MP3/OGG size fits with room.
Ownership of the 8K halves is per port 0x05 (`atj2085_hw.md` 1.2); while the DSP runs, the MCU has no DSP RAM except what
the codec hands over (`mp3_driver_spec.md` 0.x).

**TROM.** 21504 B = 7168 Q23 words (`analysis/trom_21k.bin`), twiddles then integer (Huffman-like) tables for the stock codecs.
It is wired to DSP DB[23:8] in the block diagram but the DSP address/access method is undocumented. FLAC needs no
table ROM at all (only the 5 fixed-predictor coefficient sets; Rice decoding is arithmetic), so this is irrelevant.

**ADEF loading.** `.DSP` = 512-byte header: `'ADEF', u16 0, u16 total_sectors=13` (PCMDEC bytes 0-7:
`41 44 45 46 00 00 0D 00`), then 8-byte entries `tag[4], u16 start_sector, u16 sector_count` at offset 0x10:
DMLS/DMMS/DMHS then PMLS/PMMS/PMHS (PCMDEC: DM at sectors 1,2,3 x1; PM at 4,7,10 x3). Lanes are plain bytes (no
compression, no encryption). `InitDSPCode(name, C)` DMAs each lane into IDM/IPM; the DSP starts at PM 0 on `2E |= 80`
(`mp3_driver_spec.md` section 1; zos loader `zos/modules/mp3play/mp3cold.c load_dsp`). Because zos already has this
loader, a new image only needs to follow the same container (and the DM must be reloaded before every start).
The `913f` SDK `.dsp` files start with `11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 75` (PCMDEC offset 0) and are a different
wrapped format for a different chip (AK1011); not usable as-is (unresolved, as the other tool also notes).

**Host link.** HIP mailbox ports 0x30-0x37, DSP->MCU IRQ = 0x26/0x27 bit 0; request codes 0x11/0x2B/0xBB/0xEE/0xCC/0x55/0xAA/
0xE0/0x77, DMA of one 512-byte sector per 0xEE into DM 0x1CB3 with port 0x05 = 0x24 (`dsp_interface.md` sections 4-5,
`mp3_driver_spec.md`). DAC: DSP writes a 24-bit word to DM 0x3FED (D23..D4 -> 20-bit FIFO, D3..0 dropped), polls 0x3FEE bit 2
(FIFO full). FIFO is 8 x 20-bit; rate set by the host via ports 0x80/0x81 on the 0xCC request (DSP reports a rate code in port
0x34). I found no `3FED` direct-address word in the four PM images (verified by scan), so the stock code uses register-indirect
addressing (I-register preloaded with 0x3FED) for the DAC write.

## 2. Can we write our own DSP code?

- **Toolchain: none exists in the SDK folders.** Searched all of `ATJ2085/` (sdk 80h, 913f, 2091N, B002, 128_ATJ2035L): the only
  DSP-related files are prebuilt `.DSP`/`.dsp` binaries and Z80-side headers/libs (`InitAdsp.h`, `*.r01`). `tools/` holds
  APBuilder/FIMaker/FWBuilder/BuildActos (packagers for the Z80 side and image files) and a `.chm`-less set of .doc
  manuals; no DSP assembler, linker script or `.asm/.dsp` source. Actions' DSP codecs ship binary only.
- **ISA is compatible with something that has documentation and open-source references.** The ADSP-218x encoding is public
  (ADSP-2100 family user manual; open-source emulators/disassemblers such as MAME's adsp2100 core implement every opcode;
  I did not fetch them). A usable assembler is a small job (~ 1-2 days: ~100 opcode formats, but the whole ALU/MAC/shift/move
  set is table driven) and the loader format is trivial.
- **Gap: no way to test.** `atj2085emu` models the DSP only as HLE (`emulator-usage.md`), so code can't run in it. We would
  need to write (or port from MAME) a 218x simulator with the Actions deltas (24-bit DM, DAC port, mailbox) before the first
  trial on hardware. Everything beyond opcode semantics that the encoding leaves open (24-bit multiplier/accumulator
  widths, MR/SR width, whether MX/MY are 24-bit, circular buffer regs, zero-overhead loop `DO` behaviour with 24-bit)
  must be inferred from disassembling MP3DEC/OGGDEC, which is a good test oracle.
- **Risk of damaging hardware is low** (DSP only reads/writes its own RAM, DAC FIFO and HIP). The worst case is a hung DSP,
  fixed by resetting `2E.7`; a watchdog already exists in the MP3 driver design.
- Reference to confirm: the stock handlers' use of ports and sequencing is fully specced in `mp3_driver_spec.md`; a FLAC
  image can reuse the same host protocol (0x11/0xCC/0xEE/0xAA...), so the zos mp3play module is largely reusable.

## 3. FLAC on this hardware

**Workload** (16-bit 44.1 kHz stereo = 88,200 samples/s). Per sample: Rice decode (unary count + k bits + zigzag),
LPC predict order p (p MACs + shift), stereo decorrelation, DAC write. FLAC -5/-8 files: order 8-12 on most frames;
-0..-2 files use fixed orders 0-4 or order <=8.

**DSP budget.** At 72 MIPS: 816 cycles/sample; at 36 MIPS (stock setting): 408; at 12 MIPS: 136. A 218x does one
MAC + two memory moves per cycle, so LPC order 12 = ~15-20 cycles; Rice decode with a bit reader built from 24-bit
shift/or ops is roughly 30-60 cycles/sample (the 218x has an exponent/normalize unit that finds the unary run in one op);
bookkeeping/stereo/DAC poll ~ 15 more. Estimate **~70-120 cycles/sample => 6-11 MIPS for 44.1k stereo; 25-40 MIPS worst case
if written naively**. The decoder fits at 36 MIPS with a large margin; 72 MIPS is available for 24-bit/96 kHz material
(not tested). These are estimates, not measurements (no DSP simulator).

**DSP memory.** FLAC max block 4096 samples (subset for 44.1k; some encoders go to 8192 for >48k only). Two channels x 4096 x
1 word = 8192 DM words of 16,128 available (data can also live in the unused PM tail, 0x3FFF-code). Residual can be decoded
in place and predicted in place, so no extra buffers. Remaining DM for stack, the 512-byte (342-word-packed or 512-word-
unpacked) DMA sector buffer at 0x1CB3, bit-reader state, coefficients (32 x 24 bit) and the PCM output ring. It **fits**,
but the buffer layout must avoid the fixed ports: DM 0x3F00+ registers, DM 0x1C01-0x1D60 mailbox variables
(only if the stock driver is reused; a new image can choose its own addresses, but the DMA destination 0x1CB3 and mailbox
data window are chosen host-side and could stay).
The existing OGG image already uses all 16K DM + 0x2C00 PM words, so memory pressure is not an obstacle.

**Z80 (24.576 MHz stock; 48 MHz possible per `atj2085-device-facts.md`, LCD-tested only; NAND at 48 MHz suspect).**
Budget per output sample: 24.576 MHz / 88.2 kHz = **278 T-states**; 48 MHz = **544**. Z80 has no multiplier: a 16x16
software multiply is ~250-400 T-states (shift-add) so LPC order 8 = 2000-3000 T-states/sample (4-10x over budget even at
48 MHz) before Rice decoding (~150-250 T/sample for a bit-serial reader) and DAC writes. Even fixed predictors (shift/add
only) leave ~100 T-states for everything else. **Z80 FLAC decoding is hopeless** for stereo CD-quality; possibly
mono 22 kHz or low-order fixed-predictor files only, at 48 MHz, with no CPU left for UI/FS.

**Options**

(a) **DSP FLAC decoder (own image + thin zos driver).** Technically feasible by budget and memory. Needs: assembler,
218x simulator, a FLAC decoder written in 218x asm (or a tiny C-like macro layer), integration with the mailbox/DMA
protocol. Highest value; highest effort.

(b) **PC-side transcode to WAV/PCM and play through the Z80->DAC path (what `usbaudio` already does).** `zos/modules/usbaudio/audio.c`
(header comment + `DAC_CHUNK`/`dacfeed.s`) already feeds 44.1/48 kHz stereo 16-bit into the DAC FIFO from the Z80 ("the DAC has
no DMA", ring in ZRAM1, `dacfeed.s` pushes words), so PCM playback needs only a file reader instead of USB. Constraint:
a sector stream at 176.4 kB/s plus Z80 word pushes (2 x 88.2k writes/s of 3 OUTs each, roughly 80-100 T-states per word pair)
must keep up - Z80 load is moderate (~35-45% at 24.576 MHz) but the filesystem read throughput under that load is unmeasured.
Cost: WAV is ~3x FLAC size (10.6 MB/min).
It would not be "FLAC" playback; the PC conversion could be automated (a zos-side or companion script `flac -> .wav`).

(c) **PCMDEC.DSP is not a lossless path.** Verified: PCMDEC.DSP DM holds the IMA/DVI ADPCM index table (`-1,-1,-1,-1,2,4,6,8`
at DM 0x36-0x3D) and step-size table starting `7, 8, ...` at DM 0x3E, plus a RIFF/WAVE template string at DM 1 and the sample-rate
list (DM 0x11 = 0xBB80 = 48000, ...) (`orig/fwimage/PCMDEC.DSP` sectors 1-3). The stock libraries select it only for
`SongType` 0x82 (`dsp_interface.md` section 9; library claim unverified by me). It decodes 4-bit ADPCM, i.e. lossy 4:1
compression, so it cannot carry lossless audio. AG8DEC is another lossy Actions ADPCM. The `913f` SDK also carries
`apedec.dsp` (Monkey's Audio, a lossless codec) for the AK1011 chip (`ATJ2085/62720720.../sdk_i_913f/bin/apedec.dsp`, 42,496 B,
`inc/apedecC.h`, `lib/apedec.r01`): shows Actions' DSP family can run lossless decoding in real time, but it is APE (not
FLAC), a wrapped format for a different chip, and cannot be loaded on the ATJ2085 as-is. It is only useful as a size/MIPS
reference and possibly a disassembly resource.

(d) Low-cost compromise: **host-side lossless-to-near-lossless** is not possible with stock DSPs; MP3/OGG at high bitrate
are the lossy equivalents already supported.

## 4. Recommendation

1. **Short term (days): option (b).** Make a zos "WAV/PCM player" using the existing usbaudio DAC feeder with a raw 16-bit
   44.1/48 kHz stereo file source; convert FLAC on the PC. Confirm FS read throughput >= ~180 kB/s while pushing to the DAC;
   risk: file-system read speed and Z80 load, low otherwise.
2. **Medium/long term (weeks): option (a).** Ordered steps and rough effort:
   - 218x assembler + simulator with the Actions deltas, validated by running MP3DEC/PCMDEC unmodified against recorded
     output (the existing emulator DSP is HLE; need a real core): 5-8 days.
   - Prove the ISA/DAC path with a "play a sine wave" DSP image (boot, DM 0x3FED writes, rate setting): 2-3 days, includes
     device testing risk of rate/FIFO misuse only.
   - FLAC subset decoder (STREAMINFO from the host; DSP decodes frames: header, constant/verbatim/fixed/LPC, Rice, stereo
     modes, 16-bit; no CRC check) in asm: 7-12 days; add 24-bit and seek (SEEKTABLE) later.
   - zos driver: reuse `mp3play` mailbox/DMA code, new image name, status/time: 3-4 days.
   Total ~3-4 weeks of focused work.
3. **Main risks:** (1) wider-than-218x datapath semantics (MR/MAC widths, 24-bit memory moves) are only inferable from
   disassembly; (2) no DSP simulator; (3) zos can't have a custom image in the stock OS, since the OS `InitDSPCode` reads
   the named file from the FS (`mp3_driver_spec.md` section 1) - zos must supply its own loader path (already done in `mp3cold.c`);
   (4) real-HW differences vs. emulator (DMA into DSP RAM needs port05=0x24, masked DSP IRQ loses requests: `atj2085-device-facts.md`);
   (5) the DSP clock above 48 MIPS needs the VDD step and the host must handle the 0x42 reprogramming crash seen at fast MCU clocks.
4. Not recommended: a Z80 FLAC decoder.

## 5. Corrections/notes on `dsp_interface.md` sections 8-11

- Section 8 PCMDEC ADEF table: verified byte for byte. DM lane facts (rates, IMA tables): verified (DM 0x11 = 0xBB80,
  0x36-0x39 = 0xFFFFFF, 0x3E = 7). The `913f` header analysis: not re-checked, plausible.
- Section 9 `GetDspProgName`/SongType selection: not re-verified by me. Claim "PCMDEC = IMA ADPCM, not raw PCM": verified by DM tables.
- Section 11 "IDA decodes 0A001F as `if neg call`": contradicted by my 218x decoding (RTI); IDA module is likely applied with
  a mismatched table, treat its mnemonics as unverified. The "24-bit DM vs 16-bit stock 218x DM" observation is correct (datasheet 4.3).
