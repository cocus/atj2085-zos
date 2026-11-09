# How the player drives the DSP (MP3 / WMA / OGG / ADPCM / images)

Sources: SDK 4.3 basal C source (`Sdk_i_43/msrc/basal/basal_mp3_de*.c`), headers (`inc/basal.h`,
`mp3decC.h`, `mmcmd.h`), the binary codec library `mp3dec51.r01` (dumped with XLINK, symbol names
kept: `analysis/mp3dec51_lib.txt`), and the v04 firmware (`disasm_v04/`, library-matched names).
Port meanings are from code behaviour; register names from the ATJ2085X programming guide are
added in `analysis/atj2085_hw.md` (cross-check pending).

## 1. Layers

```
MUSIC.AP UI (banks 0x40+)          -- keys, screens, lyrics display
   | mp3pSendCommand / wmapSendCommand / oggp.. / ag8pSendCommand(cmd, param)   [banked call]
Basal layer (module window 0x1C00-0x2100, banks 0x20-0x3E)   -- C, source in SDK msrc/basal
   | c_xxxdecINIT / StartDSP / c_XxxdecEXIT / pause / continue   + IRQ_Intercept(handler, IRQ_DSP)
Codec driver (codec window 0x2400-0x26FF, banks 0x01-0x1C)   -- asm lib (mp3dec51, wmadec, oggdec, adagdec, adagenc)
   | ports 0x26/0x27 (DSP IRQ), 0x2E/0x2F (DSP control), 0x30-0x35 (host interface), 0x05 (DSP memory window),
   | 0x06-0x13 (DMA), 0x42 (DSP clock), 0x80-0x8B (DAC/PA)
OS (SYSBNKA/B): InitDSPCode 0x0023, StartDsp 0x0024, StopDsp 0x0124, SetDspMips 0x0224, IRQ_Intercept/Release
   |
DSP running XXXDEC.DSP (24-bit core), decodes, pushes PCM to the DAC
```

## 2. Player command API (`basal.h`, `mmcmd.h`)
One entry per codec: `BOOL xxxpSendCommand(BYTE cmd, int param)` (`mp3p`, `wmap`, `oggp` (LISONG),
`wavp`, `ag4p`, `ag8p`; recorders `xxxrSendCommand` + `xxxrProceed`).

| cmd | value | param |
|---|---|---|
| MC_OPEN | 0x01 | `Open_param_t*` {filename, breakPT_t* resume data} |
| MC_CLOSE | 0x02 | — (closes file, sets port 0x8B bit 4) |
| MC_LOAD | 0x03 | — open file, run a DSP *scan pass* to get duration |
| MC_PLAY | 0x04 | `Nor_open` / `Nor_Break_open` (resume) / `ABPlay_open` |
| MC_PAUSE / MC_STOP | 0x05 / 0x06 | |
| MC_STEP | 0x07 | ±speed → fast forward / rewind |
| MC_FADEINOUT | 0x0D | FadeIn / FadeOut |
| MC_SETVOLUME | 0x40 | volume |
| MC_SETA_B | 0x41 | ClrABPoint / SetAPoint / SetBPoint / ReturnApoint / ABFinished |
| MC_SETUSEREQ / MC_SETEQ | 0x42 / 0x43 | 10-band values / preset (none, rock, pop, classic, soft, jazz, dbb, user) |
| MC_BACKUP / MC_RESTORE | 0x44 / 0x45 | save / restore resume + A/B points |
| MC_SETSPEED | 0x46 | -8..+8 tempo (VPS table −16..+32) |
| MC_GETTIME / MC_GETSTATUS / MC_GETAUDIOFMT | 0x80 / 0x83 / 0x82 | Full_time_t* / mp3_play_status_t* / audio_format_t* |

Status codes: PLAYING_STOP 0, PLAYING 1, PAUSE 2, AB 3, … REACH_END 6, ERROR 0x0C.

## 3. Sequence (MP3, from basal_mp3_de_b.c + mp3dec51)
**MC_LOAD**: `FS_FOpen`; length = sectors×512 adjusted by `FS_GetUnalignedLen`; set
`hook_read = m_Mp3deRead_Data`, `hook_Buffer = m_Mp3deDataBuffer[512]`;
`IRQ_Intercept(mp3deDecoderHIPINT, IRQ_DSP)`; `c_mp3decINIT()`; `StartDSP()`; spin until
`c_mp3demp3DecodeInfor == 0xFE` (DSP finished scanning) → song length from `c_mp3deSongTimeLength`;
`c_Mp3decEXIT()`; `IRQ_Release`.
**MC_PLAY**: seek to 0 / resume point / A point, `IRQ_Intercept`, `TM_SetTimer(RTCTimerServe, 1)`
(play clock), `c_mp3decINIT()`, `StartDSP()`. Everything after that is interrupt driven.

`c_mp3decINIT` → `mp3deDecoderINIT`: `out(0x8B, 0x05)` (DAC/analog on), clear the 0x24-byte state
block, then **RestDsp**: port 0x42 `&0xE3 | 0x0B` then `|0x40` (DSP clock), 2 ms, `out(0x2E,0x7B)`,
`out(0x2F,0x03)`, enable DSP interrupt (port 0x27 bit 0), **`InitDSPCode(DE="MP3DEC  DSP", C=0)`**
(OS loads the .DSP image), port 0x05 = 0. `StartDSP`: port 0x2E bit 7 = 1 (run).

## 4. Host-interface mailbox (DSP → Z80 interrupt)
The DSP raises IRQ_DSP; the handler (`mp3deDecoderHIPINT`) reads the request code from **port
0x35** (read twice), acknowledges the interrupt with **out(0x26, 1)**, dispatches, and answers by
writing a reply code to **port 0x35**. Data bytes go through **port 0x34** (Z80→DSP) and ports
**0x30–0x34** (DSP→Z80 status: mode, page counter…); bulk data through DSP memory mapped into the
Z80 space by **port 0x05** (page 0x24 → DSP variables at 0x5C01…0x5D60, page 0x34 → 0x7E08).

| request | meaning | Z80 action | reply |
|---|---|---|---|
| 0x11 | time/display sync | port 0x34 ← resume flag; (page 0x34) 3 bytes → 0x7E08 | 0x22 |
| 0x2B | load next code overlay | DSP hold (0x2E bit1=0), `InitDSPCode(name, C=1)`, release | 0x3B |
| 0xBB | parameters | page 0x24: file length / FF-REW / A-B data → 0x5D57.. | — |
| 0xEE | **bitstream request** | port 0x34 = direction, 0x31 = status; `hook_read` (FS_FRead 512 B) + DMA | — |
| 0xCC | frame header decoded | read ports 0x30–0x34 + DSP RAM (bitrate, mode, length); `c_mp3deChangDAC3fs` (DAC sample rate) | 0x99 |
| 0x55 | sentence / silence point (A-B, follow-read) | flag | 0x66 |
| 0xAA | end of stream | DecodeInfor = 0xFE | — |
| 0xE0 | exit acknowledged | | — |
| 0xA8 | **chip check** | port 0x34 ← port 0x2F (chip version) | 0x95 |
| 0xA9 | chip-check verdict (0 = OK) | non-zero → decoder refuses (state 0xEE) | 0x96 |
| 0x77 | needs more MIPS | port 0x42 bits 2-4 += 1 step (max 0x0C) = DSP clock up | — |

LISONG v04 MUSIC.AP runs a newer codec revision: its MP3/ADPCM handler (bank 0x0B) also handles
0x00–0x30 and 0x88, replying 0x3A/0x3C/0x88/0xDD/0xE1; OGG (bank 0x1C) is a near-twin; the
ADPCM encoder (bank 0x0F) uses 0x01/0x05/0x11/0x88/0x99/0xEE with replies 0x22/0x55/0x77.

## 5. DMA (bitstream to DSP)
`StartMoveBitstream`: source address (Z80 buffer `m_Mp3deDataBuffer`) → ports **0x06/0x07**;
11 descriptor bytes → ports **0x08–0x12** from a constant table: `00 00 00 B3 1C 00 40 06 FF 01 30`
(by inspection: destination in DSP memory ≈0x1CB3, length 0x01FF+1 = 512, mode 0x30); start with
**out(0x13, 1)**, completion polled on port 0x13. One 512-byte sector per 0xEE request.

## 6. Analog / clock
Port 0x42: DSP clock / MIPS (bits 2-4 step), bit 6 enable. Port 0x8B: DAC/analog enable (0x05 at
init, bit 4 set on close). Ports 0x80/0x81, 0x88–0x8A: DAC sample rate & volume
(`c_mp3deChangDAC3fs`, `c_mp3deMinishVolumn`). Port 0x9E used by the volume ramp.

## 7. Where it lives in v04 MUSIC.AP
| banks | window | contents |
|---|---|---|
| 0x01, 0x03 | 0x2400/0x2500 | MP3 codec driver (mp3dec51: `mp3deDecoderHIPINT`, `STOPDSP`, `c_mp3deChangDAC3fs`, volume ramp) |
| 0x04–0x06 | 0x2400/0x2500 | WMA codec driver (wmadec) |
| 0x09–0x0B | 0x2400/0x2500 | ADPCM/PCM decoder driver (adagdec) — same code as VOICE.AP bank 0x09 |
| 0x1A–0x1C | 0x2400/0x24C0 | OGG Vorbis driver (oggdec) |
| 0x0D–0x0F | 0x2500/0x2600 | ADPCM encoder (follow-read / compare recording) |
| 0x21, 0x23, 0x29 | 0x1C00/0x2000 | MP3 basal (`Mp3PlayCommand`, `Mp3StopCommand`) |
| 0x25, 0x26 | 0x1C00/0x2020 | WMA basal (`m_WmadeFRead_Data`, `WmaPlayCommand`) |
| 0x2D | 0x2020 | OGG basal (`oggPlayCommand`) |
| 0x27 | 0x1C00 | recording basal (`wavrProceed`) |
| 0x19, 0x31–0x33 | | LRC lyrics (read, parse, time table) |
| 0x34–0x38, 0x3E | | ID3 / WMA / OGG tags, Unicode conversion |
| 0x39, 0x3A | | file selector (next/prev/delete, random), `ExtMusic` extension filter |

DSP programs referenced: MUSIC: mp3dec, WMADEC (+wmatable.bin), OGGDEC, PCMDEC, AG4DEC/AG8DEC,
AG4ENC/AG8ENC/ADENCM (recording), mtvdec (MTV video); VOICE: AG4/AG8/PCM/WMA; RECORD/FMRECORD/LINE:
ADENCM/AG4ENC/AG8ENC; AMVDEC: AmvDec; MAIN/VIEWPIC: DSCDec (JPEG), GifDec. The DSP also decodes images.

## 8. .DSP file format
The `sdk_i_913f/bin` images examined here do **not** start with `ADEF`. For example, `PCMDEC.DSP`
starts with bytes `11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 75`; the little-endian 32-bit
value at offset `0x10` is `0x1C00`, its exact file length (7168 bytes). The next four bytes are
`00 02 00 00`, followed by the same eight bytes (`0D D0 76 5C 50 94 69 B0`) seen in the sampled
MP3 and AG8 images. The 2091N and 62720720 PCM images have the same size but different hashes.
These are observed header bytes, not yet-decoded field definitions; the payload/loader transform
is unresolved. Do not feed these files to the ADEF parser in `emu/dsp.py`.

The older 2099 SDK's `WAVDEC.DSP` has a different header again (`41 41 44 44 45 46...`), so it
is not a directly interchangeable plain-ADEF reference. In particular, the prior tentative ADEF
section-table interpretation does not describe the 913f PCM image.

There is, however, an ADEF-format `PCMDEC.DSP` in the extracted firmware images, e.g.
`lisong_rebuild/orig/fwimage/PCMDEC.DSP` (6656 bytes, 13 sectors). Its table is:

| Section | Start sector | Sectors | Byte-lane size / DSP words |
|---|---:|---:|---:|
| DMLS / DMMS / DMHS | 1 / 2 / 3 | 1 each | 512 bytes / 0x200 DM words each |
| PMLS / PMMS / PMHS | 4 / 7 / 10 | 3 each | 1536 bytes / 0x600 PM words each |

It is a 512-byte ADEF header followed by six **uncompressed 24-bit memory byte lanes**: each word
is assembled as `LS[i] | MS[i]<<8 | HS[i]<<16`. Its record lengths agree with those word counts,
and its overlay descriptors are zero. This describes the extracted ADEF firmware image, not the
separate 7168-byte 913f SDK binary. The 913f `case_i_d913f/fwpkg/fwimage.cfg` also comments out
`FWIM=PCMDEC.DSP`, so that SDK binary is not included in that package by default.

## 9. PCM/WAV path: image selection and host protocol
The 913f `basal_adag_de.c` uses one `wav_command` implementation for `wavpSendCommand`,
`ag8pSendCommand`, and `ag4pSendCommand`; it allocates a 512-byte bitstream buffer and calls the
shared `adagdecInit` / `adagdecStart` / `adagdecHipIntSub` codec interface. Its `MC_LOAD` sets
`SongType` to `WAVE_FORMAT_ACTIONS_ADPCM` (`0x82`), `..._AG8` (`0x84`), or `..._AG4` (`0x83`).

The linked `adagdec.r01` decoder library resolves which DSP image is used. Its `adagdecInit`
routine at `0x0690` calls `GetDspProgName` at `0x06E9`. The routine reads `g_decInitPara.SongType`
(`0x0742`) and selects these names from its embedded table:

| `SongType` | Selected name | Result |
|---|---|---|
| `0x00` | `WMADEC.DSP` | accepted |
| `0x82` | `PCMDEC.DSP` | accepted; this is the wrapper's Actions ADPCM path |
| `0x84` | `AG8DEC.DSP` | accepted |
| other values, including `0x81` and `0x83` | — | sets decoder status to `0xF0` (format error) |

This mapping is directly visible in `ref/sdk913f_a/adagdec.s28` and its symbol map:
`GetDspProgName` loads the WMA name by default, switches to the PCM name before comparing with
`0x82`, switches to the AG8 name before comparing with `0x84`, and returns the format-error status
if neither matches. The same selection bytes occur in `ref/sdk913f_b/adagdec51.s28`. Although the
embedded table also contains `AG4DEC.DSP`, this selector does not choose it; that conflicts with
the basal wrapper's type-3 assignment of `0x83`. Treat AG4 playback as unsupported or mismatched
for these library builds unless another code path/version is found.

Thus `PCMDEC.DSP` is loaded for the wrapper's `wavpSendCommand` / Actions ADPCM (`0x82`) case;
the filename does **not** mean the literal Actions PCM tag (`0x81`) is accepted here. The `0x81`
path appears to be rejected by this library build.

### 9.1 What the extracted PCM decoder computes
Reassembling the ADEF image's three DM lanes exposes a strong algorithm fingerprint:

- DM word 1 begins the low-lane byte string `RIFFWAVEfmt data`.
- DM words `0x11`, `0x13`-`0x15`, and `0x17`-`0x19`, `0x1B`-`0x1C` contain the common WAVE
  sample rates 48000, 24000, 12000, 32000, 16000, 8000, 44100, 22050, and 11025.
- DM words `0x36`-`0x3D` are `-1,-1,-1,-1,2,4,6,8`, the IMA/DVI ADPCM step-index adjustment
  table.
- The following 89 entries at DM `0x3E` exactly match the standard IMA/DVI ADPCM step-size table
  (7 through 32767).

Together with the `SongType=0x82` selection and the RIFF/WAVE template, this identifies the
extracted `PCMDEC.DSP` as a RIFF/WAVE Actions-ADPCM decoder using the IMA/DVI ADPCM step-table
algorithm—not a general raw-PCM decoder. The `0x82` format tag is Actions-specific; the table
match identifies the reconstruction algorithm, but does not establish that the on-disk WAVE
variant is byte-for-byte Microsoft IMA ADPCM.

The 24-bit PM image contains the implementation, but it has not yet been instruction-disassembled.
Consequently, nibble order, per-channel block-header layout, predictor initialization/clipping,
and exact output sample packing remain unverified. These conclusions apply to the extracted ADEF
image; the separate 913f SDK image still has an unresolved wrapper/payload transformation, and
should not be assumed byte-identical to this older program.

The v04 AP's shared ADAG/WAV host-side IRQ dispatcher is visible in
`disasm_v04/VOICE_AP/seg_bank09.s` (a matching dispatcher is in MUSIC bank 0B). It confirms this
mailbox surface:

| DSP request | Host-side behavior observed |
|---|---|
| `0x11` | Exchange a byte on port `0x34`; optional shared-memory copy; reply `0x22`. |
| `0xEE` | Service a DSP data request, transfer the staged buffer by DMA into DSP memory, then reply `0xDD`; special status values select end/marker handling. |
| `0xBB` | Exchange playback/status/control fields through shared DSP memory. |
| `0xCC` | Read codec format fields, use the low nibble from port `0x34` to set the DAC rate, and reply `0x99`. |
| `0xAA` | Handle stream-end/status flags; may set the host's end/status state rather than returning a normal reply. |
| `0x2B` | Stop the DSP, call `InitDSPCode` with the overlay name, restart it, and reply `0x3B`. |

This is good evidence for the **host protocol of the shared ADAG/WAV driver**. The extracted ADEF
program's algorithm and memory-plane format are now partly identified, but its PM instruction
flow and the 913f image's transformation remain open. Resolving those requires disassembling the
PM or tracing a live `0x82` playback and the exact 913f loader path.

## 10. OGG comparison and implications for FLAC
The 913f OGG library (`ref/sdk913f_a/oggdec.s28`, `oggdec.map`) and its basal wrapper
(`basal_ogg_de_b.c`) provide a second example of the same host/DSP architecture:

- The basal sets `SongType=0`, provides a 512-byte `BitsreamBuff`, installs a file-read hook,
  and supplies file length in filesystem pages. Playback seeks to the start or a saved page and
  calls `oggdecInit` / `oggdecStart`; pause/continue and the DSP IRQ are handled by the codec
  library and host IRQ path.
- The OGG mailbox dispatcher recognizes `0x11`, `0xEE`, `0xBB`, `0xCC`, `0x2C`, `0x2A`,
  `0xAA`, `0xE0`, and `0x77`. The `0xEE` request uses the host's read/DMA path, `0xCC` handles
  stream format/rate information, and `0x2A` invokes OS API `GetOggTable` and replies `0x3A`.
  This is close to the MP3 request/response model, but OGG has its own table request and the
  status/control details should not be assumed identical without tracing each handler.
- On the observed OGG host path, `0xCC` reads the low nibble of port `0x34`, maps it through the
  embedded rate table, calls `DAC_SetRate`, and replies `0x99`. This confirms reuse of the
  hardware DAC-rate service, not that every FLAC sample rate is supported by the DAC.
- The library embeds the names `OGGDEC.DSP` and `oggtable.bin`; the extracted firmware has a
  separate 13,824-byte `OGGTABLE.BIN`. The name and API call establish an external OGG-table
  dependency, but the table's internal format and exact DSP destination are not yet established.
- The extracted OGG ADEF image is 83,456 bytes (163 sectors): DMLS/DMMS/DMHS each describe
  16,384 words, while PMLS/PMMS/PMHS each describe 11,264 words. Thus this build occupies the
  full 16K-word DM address range and 0x2C00 PM words, versus the extracted ADPCM image's 0x200
  DM and 0x600 PM words. The 913f SDK OGG image is a distinct 83,968-byte wrapper-format file,
  so these ADEF memory counts do not directly characterize that SDK binary.
- These image/layout and mailbox observations do not reveal the OGG DSP's instruction-level
  algorithm or packet payload interpretation. The PM code has not been disassembled; those
  internals remain a separate reverse-engineering task.

### 10.1 What is reusable, and what FLAC needs
The MP3 and OGG examples support reusing the existing firmware services rather than inventing a
new transport: codec-image loading through `InitDSPCode`, interrupt/mailbox dispatch, 512-byte
filesystem staging, DMA into DSP memory, DAC rate selection, volume, and play/pause/stop controls.
The host integration still needs a FLAC-specific driver/basal entry and the appropriate image
selection/loading path; it cannot simply label FLAC data as OGG or MP3.

FLAC's codec work is separate from that shared shell. A decoder needs STREAMINFO/metadata parsing;
frame sync, header and CRC checks; subframe decoding (constant, verbatim, fixed predictors and
LPC); Rice/Rice2 residual decoding; wasted-bit handling; stereo side-channel reconstruction; and
conversion to the DAC's supported sample width/rates. Seeking can use a FLAC SEEKTABLE when
present, otherwise it needs a frame scan or a constrained approximate strategy.

Recommended implementation sequence:

1. Implement a host-side/reference FLAC decoder and frame test vectors first, establishing which
   bit depths, channel assignments, block sizes, sample rates, and malformed streams must work.
2. Prototype fixed-point frame/subframe decoding on the DSP and measure worst-case cycles at the
   target sample rate, including stereo LPC and high-compression settings. The OGG image shows
   substantial memory exists, but does not prove a FLAC decoder will fit or meet real-time
   throughput; the DSP instruction set/toolchain and actual clock budget still need to be verified.
3. Keep the existing 512-byte refill/DMA and DAC/mailbox services where possible, and add only
   FLAC-specific frame state, seek behavior, and error reporting. Validate across arbitrary
   sector boundaries, since FLAC frames are not aligned to the host's 512-byte reads.

A host-only implementation is useful as a correctness oracle, but real-time playback on the Z80
host is not established and should not be assumed. The most plausible playback route is a custom
DSP decoder plus a thin host driver, after proving the ISA/build workflow and cycle budget. The
current evidence is enough to outline that route, not to claim that FLAC already fits the ATJ2085.

## 11. DSP instruction-set investigation (preliminary)
IDA 6.8 is installed with the `ad218x` processor module ("Analog Devices ADSP 218x"). I tested
its batch disassembler on the **extracted ADEF** PCMDEC, MP3DEC, and OGGDEC images. To prepare
IDA raw input, reconstruct each PM instruction word as
`PMLS[w] | PMMS[w]<<8 | PMHS[w]<<16`, then write its three bytes least-significant byte first.
The tested invocation was:

```text
idat.exe -A -B -pad218x -o<output.idb> <interleaved-PM.bin>
```

IDA treats each 3-byte word as one PM address and recognizes the expected vector layout: reset at
word `0x0000`, IRQ entries on four-word boundaries (including `0x0004`, `0x0008`, `0x0010`,
`0x0014`, `0x0020`, `0x0024`, and `0x0028`). Across the images, repeated vector word `0x0A001F`
decodes as `if neg call $3000`; PCMDEC and OGGDEC reset word `0x18030F` decodes as
`sr = lshift mr0 by 24 (hi)`. Other decoded words use ADSP-21xx-style parallel ALU and DM/PM
indirect moves, e.g. `ar = ay0 - 1, mx1 = dm(i3, m0), ay1 = pm(i4, m5)`. That structure and the
SDK's `InitAdsp.h` naming make an **ADSP-21xx/218x-compatible encoding a strong lead**. IDA can
therefore be used immediately to start mapping instruction fields, control flow, registers, and
parallel operations.

This is not proof that the ATJ2085 core is a stock ADSP-218x, nor is ADSP-218x the only possible
DSP architecture. The ATJ2085X documents describe 24-bit PM **and 24-bit DM**, whereas a standard
ADSP-218x has a 24-bit PM and 16-bit DM. The common vector pattern supports a shared or compatible
instruction encoding; datapath width, registers, instruction extensions, and peripherals may be
Actions-specific. The guide's block diagram also shows the TROM table ROM connected to
`DSP DB[23:8]` alongside the DSP external-data-memory interface. TROM is therefore a DSP-visible
data source in hardware, not just a Z80 banked ROM; however, the guide does not specify its DSP
address range, mapping/control, or 8-to-24-bit read behavior. The confirmed Z80 mapping
(`02h=00h, 01h=02h`) is a separate access path and cannot by itself be used as the DSP address.

IDA's `-B` output leaves most PM words as data because automatic analysis follows only discovered
code references; the current listing is not a complete DSP disassembly. Treat the mnemonics as a
working decode hypothesis until confirmed by reachable code and known hardware effects.

On the PCMDEC automatic analysis, IDA marked only 59 PM words at `0x0000`-`0x003A` as code.
It decoded calls to `$3000`, `$3371`, and `$3041`, all outside the loaded 0x600-word PM section;
the remaining words were not reached by automatic analysis. These targets could be hardware ROM
or loader-provided routines, or evidence that the decode assumptions are incomplete. They are
unresolved, not confirmed symbols.

SDCC is for the Z80 MCU/host side in this project; it does not provide an ATJ DSP backend. Do not
use it to assemble DSP programs. Before writing DSP code, the next validation is to trace actual
DSP control flow from reset/interrupt vectors, identify a reachable store to the documented DAC
DM register `0x3FED`, and cross-check address/data widths and opcode effects against several
independent code paths. Separately, probe for external-DM reads that could access TROM and identify
their addresses and word reconstruction. This will decide whether the existing IDA backend is
adequate or needs an Actions-specific processor extension. No instruction-level conclusions
should be drawn from the unrelated 913f wrapped `.dsp` binaries until their wrapper/payload
transformation is decoded.
