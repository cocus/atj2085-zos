# Audio hardware path: DAC, ADC, analog routing (what `zos/modules/audiotst` uses)

Sources: ATJ2085X Programming Guide v3.1 ("PG", text extracted with pdftotext), ATJ2085X datasheet v1.8 ("DS"),
`analysis/atj2085_hw.md`, `analysis/dsp_interface.md`, the OS bank `orig/fwimage/SYSBNKA.SYS` (disassembled with
`tools/z80dis.py`), `disasm_v04/{RECORD,FMRECORD,LINE,FMRADIO,MUSIC,VOICE}_AP`, SDK 4.3 `inc/device.h`.
Status legend: **[doc]** = stated in the programming guide / datasheet, **[fw]** = seen in firmware code,
**[untested]** = not yet verified on the device.

## 1. Answer: the Z80 reaches both converters directly, no DSP is needed

| direction | path | evidence |
|---|---|---|
| Z80 -> DAC | 20-bit PCM words written to ports **0x88 / 0x89 / 0x8A**; the write to 0x8A pushes the word into the 8-word playback FIFO. FIFO-full = **0x8B bit 5**. The same FIFO is what the DSP fills through DM address 0x3FED. | **[doc]** PG p.52 ("Write to this register will transfer 20-bit PCM data into DAC PLAYBACK FIFO ... also mapped into DSP memory mapped EM port 0x3FEDh"), DS 4.10 ("playback FIFO (8 x 20-bit PCM data for L/R channel)"). |
| DSP -> DAC | DSP writes DM 0x3FED, polls 0x3FEE.2 (= port 0x8B.5). This is what the stock player does (the Z80 only configures the DAC). | **[doc]** PG p.52; **[fw]** no `out (88h/89h/8Ah)` anywhere in v04 APs; MUSIC init only does `out (8Bh),05h` (MUSIC.AP bank02 0x250E) and the DAC rate/volume. |
| ADC -> Z80 | Port **0x9A**, read order R-hi, R-lo, L-hi, L-lo (or only R-hi, R-lo with D7.4 = 1). FIFO not-empty = **0xD7 bit 5**; optional IRQ (0xD7.7 enable, 0xD7.3 pending, master bit 0x27.7) or DMA2 DRQ2B. | **[doc]** PG p.54, p.58. **[fw]** the stock recorder does exactly this: `RECORD.AP/FMRECORD.AP/LINE.AP bank0c` (the same code is in MUSIC bank0f for the ADPCM encoder): `adagencStart` 0x2900 (`D7 = (D7 & FC) \| CD`, `27 \|= 80`, StartDsp), ISR body `adagencADCDMAIntSub` 0x2A18: `in a,(9Ah)` twice (0x2A24 / 0x2A28) into a RAM buffer, loop while `D7 & 20` (0x2A5C). The Z80 then hands the PCM buffer to the DSP encoder (AG8ENC/ADENCM). |
| DAC DMA | none: there is no DAC DRQ in the DMA trigger lists; ADC has DRQ2B. | **[doc]** atj2085_hw.md §3.3 |

So the stock firmware uses the DSP only as a codec (MP3/WMA/ADPCM). For raw PCM test tones and a raw recorder a
DSP program is unnecessary: PCMDEC.DSP / AG8ENC.DSP would need the whole loader and mailbox protocol
(`dsp_interface.md`) just to move samples the Z80 can already read/write itself. The simplest thing that works is the
direct port path, so that is what `audiotst` does. Caveat: stock never writes the DAC from the Z80 (**[untested]**
on the device, see section 6); the ADC path is the stock one.

Timing: DAC FIFO 8 words (= 4 stereo frames); at 8 kHz that is 0.5 ms of slack, the zos RTC ISR (10 ms) is
short enough. ADC FIFO is 12 words (threshold setting in D7 says 8/4/1 of 12) = 1.5 ms at 8 kHz.

## 2. The stock OS routines (SYSBNKA.SYS) = the register recipes

The ACTOS API (`rst 10h`, HL = API number; `analysis/api_names.json`: 0x26 ADC_Enable, 0x27 DAC_Enable,
0x28 PA_Enable, 0x29 AI_Enable, each with Disable/SetRate/... at +0x100 steps) lives in the OS bank A. Each
API group is a 0x200-byte slot in the file with a header of entry points; file offset = load address + 0x4800
(slot base 0x0400). Disassembled (SDK structs in `Sdk_i_43/inc/device.h`):

**ADC_Enable(adc_t*)** — file 0x4C12 (entry 0x0413). adc_t = {adin, rate, gain, channelmode, precision}:

| step | port | meaning |
|---|---|---|
| `D5 = (D5 & FC) \| adin` | D5.1-0 | 0 mic, 1 FM, 2 line-in, 3 analog mixer (AD_MICIN/FMIN/LINEIN/MIXIN) |
| `D5 = (D5 & 1F) \| ratecode` | D5.7-5 | table 0x04F9/0x0500: 48k=A0, 32k=80, 24k=60, 16k=40, 12k=20, 8k=00 |
| `D3 = (D3 & FC) \| gain` | D3.1-0 | ADC input gain -6/-3/0/+3 dB (gain < 4) |
| `D7 = (D7 & EF) \| mono<<4` | D7.4 | 1 = right channel only |
| `9B = precision` | 9B.0 | 0 = 16 bit, 1 = 18 bit |
| `D2 \|= 80`, `D9 &= FC` | D2, D9 | **not in the PG** (clock/power of the ADC? undocumented) |
| `D4 \|= 88` | D4.7, D4.3 | ADC enable + dither |

ADC_Disable (file 0x4C7D): `D2 &= 7F; D4 &= 7F; D7 = (D7 & 33) | 48`.

**AI_Enable(ain_t*)** — file 0x5212 (entry 0x0413 of slot 0x5200). Flags bit0/1 = mic: `9F = DA`, `D4 |= 40`;
bit2 = line: `9F = 7A`, `D4 |= 20`; bit3 = FM: `9F = 7A`, `D4 |= 10`. (The SDK comments for linein/fmin are swapped;
the code and the PG agree: D4.5 = line, D4.4 = FM stage power.) AI_Disable = `D4 &= 8F` (file 0x525C).
AI_SetGain(sel=e, gain=c) (file 0x5263): sel 0 mic `D4.2-0`, sel 1 line `D6.6-4`, sel 2 FM `D6.2-0`.
Stock FM radio init calls AI_Enable then AI_SetGain(2, 5) = FM gain 0 dB (FMRADIO.AP bank12 0x2521-0x252C).

**DAC_Enable(dac_t*)** — file 0x4E12. dac_t = {select (0 internal), rate}. Internal path (0x4E40): rate code
via table 0x04EA (48k=0, 24k=2, 12k=3, 32k=4, 16k=6, 8k=7, 44.1k=8, 22.05k=A, 11.025k=B) -> `out 81h`;
`42 &= FC` for the 48k family, `42 |= 03` for the 44.1k family (PLL2); delay; then
`8B &= EF; 80 = 28; 8B = 18` (FIFO reset pulse, QU = 8; DACEN + SDM, mute off). DAC_SetRate (0x4E60) re-does the
rate and pulses 8B.4.

**PA_Enable(pa_t*)** — file 0x5012. pa_t = {pain bits, volume}. bit0 dacin: `80 &= FD` (unmute DAC);
bit1 fmin `D5 \|= 04`; bit2 linein `D5 \|= 08`; bit3 micin `D5 \|= 10` (the analog-mixer unmute bits);
volume -> `9E` (<= 0x1F); then `DD \|= C0 (dac in) or 40 (only analog inputs)`, `DD \|= 02`.
PA_Disable (0x506B): `D4\|=80; DD &= 3F; D5 &= E3; 80 \|= 02; ~3 ms delay; D4 &= 7F`.
PA_SetVolume (0x50A2): `9E = vol` (vol <= 0x1F). GetVolume `9E & 1F`.

## 3. Analog topology (PG pp.53-58, DS block diagram)

```
MIC --(x22..x57, D4.2-0)--+                         +--> ADC (18 bit sigma-delta, FIFO 9Ah)  <- D5.1-0 selects
FMIN (L/R) -(D6.2-0)------+--> analog mixer --------+       (00 mic, 01 FM, 10 line, 11 mixer output)
LINEIN -(D6.6-4)----------+     ^  D5.4/3/2 = unmute mic/line/FM into the mixer
DAC (20 bit, 0x88-0x8A / DSP) --+---> mixer/PA (DD.6) -> headphone, volume 9E.4-0 (1.8 dB/step)
```
* The FM tuner's audio goes **analog** to the FM input pins; it does not pass the Z80 or DSP. To hear the radio:
  tuner programmed over I2C (`i2c.md`), D5.2 = 1, DD = 0x42 (mixer+PA on, DAC off), volume 9E. To record it:
  additionally ADC source D5 = 01, D4.4 = 1 (FM stage), 9F = 0x7A, ADC enable D4.7.
* The board has no line-in (task statement); the microphone is the ATJ2085's MIC pin (VMIC bias).
* ADC mono-mic: "Select MIC, R and L is same" (PG p.57) so right-only reads are fine for the mic.
* DAC sample rates (0x81): 0000 48k, 0010 24k, 0011 12k, 0100 32k, 0110 16k, 0111 8k, 1000 44.1k, 1010 22.05k,
  1011 11.025k. ADC rates (D5.7-5): 8/12/16/24/32/48 k.
* 0x42 bits 1-0 select the DAC clock family (PLL2 for 44.1k). `audiotst` uses 8 kHz and clears them as the OS does.

## 4. What `zos/modules/audiotst` does

Menu (LEFT/RIGHT move, PLAY run, MENU back):
1. **TONE GENERATOR** — 8 kHz, Z80 writes the FIFO in a polled loop. Waveforms (PLAY cycles): sine 440 Hz, sine 1 kHz,
   square 440 Hz, square 1 kHz, sine 250 Hz, sine 2 kHz. LEFT/RIGHT = headphone volume 9E (VOL key = +3 and wrap).
   Power-up sequence: `42 &= FC`, `81 = 07`, `80 = 2A` (muted), FIFO reset pulse, `9E = 0`, `DD = C2`, wait 100 ms,
   `80 = 28`, then ramp 9E to the chosen volume 1 step / 10 ms (anti-pop).
   It first measures, silently, the FIFO depth and the FIFO words consumed per second, and decides whether one
   sample frame is 2 FIFO writes (L, R) or 1 (the PG only says "L/R channel"): shows `FIFO DEPTH`, `WORDS/S`,
   `WRITES/FRAME`. Expected: depth 8, words/s ~16000, writes/frame 2. If the tone is an octave off or
   half-speed, set "DAC WORDS/FRAME" on the main menu to 1 or 2 manually.
2. **MIC: METER+REC** — ADC on (mic): `D5 = 00 (mic, 8k)`, `D3 = +3 dB`, `9B = 0`, `D2 |= 80`, `D9 &= FC`, `9F = DA`,
   `D4 = C8 | gain`, D7 FIFO reset/sync with right-only. LIVE shows a log level bar, raw sample and DC (hex), and the
   measured **samples/s** (must be ~8000 if the ADC runs). LEFT/RIGHT change the mic gain (x22..x57). PLAY starts
   recording (8 kHz, DC-blocked, G.711 mu-law 8 bit, ~1.1 s), PLAY again or buffer full -> automatic playback through the DAC
   (LEFT/RIGHT/VOL volume), then back to LIVE. VOL replays the last recording.
3. **FM: METER+REC** — same, but starts the TEA5767 (bit-banged I2C as in `i2c.md`, copy of `modules/i2ctest`
   code, 98.7 MHz at start), routes FM to the analog mixer/PA (so you hear the radio live; volume with VOL), ADC source
   = FM (`D5 = 01`, `9F = 7A`, `D6.2-0 = 5`, `D4 = 98`). LEFT/RIGHT tune in 100 kHz steps (RDY / signal level LV 0-15 from
   the tuner status are shown). PLAY records; playback replaces the live FM audio by the recording. The tuner is
   put into standby and the I2C pins restored on exit. The headphone cable is the antenna.
4. **DAC WORDS/FRAME** — AUTO / 1 / 2 override for the measurement above.
5. **PORT DUMP** — ports 42 80 81 8B 9E 9F D2 D3 D4 D5 D6 D7 D9 DD 9B as found at module entry (= state the stock OS
   left) and now. Useful to learn the stock defaults of D2/D9.

Storage: RAM only. The module's own page (0xC1) is the only page it can see: the module code executes from the
window, so it cannot remap the window to another lane without core support (`page_set`/`page_write` are not in
`core/api.txt`). 9100 bytes of the 16 KB page are the record buffer. Everything changed is saved on entry and
restored (including 0x42, 0x80, 0x81, 0x8B, 0x9E, 0x9F, 0xD2-0xD7, 0xD9, 0xDD, 0x9B, GPIO C pins); the IRQ mask
0x27 is never touched (polling only).

## 5. Facts about the DAC/ADC the module relies on, per source

| fact | status |
|---|---|
| 0x88/89/8A = PCM[3:0]/[11:4]/[19:12], 8A commits; sample = 16 bit << 4 | **[doc]** PG p.52, **[untested]** signedness (two's complement assumed, as the DSP writes) |
| FIFO full = 8B.5, FIFO reset = 8B.4 low | **[doc]** |
| OS power-up order DAC (80/81/8B) then PA (DD/9E) | **[fw]** SYSBNKA 0x4E12, 0x5012 |
| ADC 9A read pairs, D7.5 loop | **[fw]** RECORD.AP 0x2A24-0x2A62 |
| ADC enable recipe incl. D2/D9 | **[fw]** SYSBNKA 0x4C12 |
| FM routing D5.2 / DD=0x42 / AI_Enable | **[fw]** SYSBNKA 0x5012/0x5212; FMRADIO.AP bank12 |

## 6. Uncertain / to check on the device
* The Z80 -> DAC FIFO path is documented but never used by the stock firmware: whether 1 or 2 writes make a
  frame (measured at run time), signedness, and whether the DAC needs anything beyond `80/81/8B/DD/9E` (e.g. the
  DSP being stopped; zos does not run it).
* Volume 0 on 9E is assumed to be quietest (-1.8 dB/step, 32 steps). Start volume 14; start low with headphones.
* D2 and D9 are undocumented; the OS touches them in ADC_Enable (D2 |= 80, D9 &= FC) and so does the module.
* ADC DC offset / level: the meter shows raw hex so the real range can be read from the screen.
* FM: needs the TEA5767 ACK at 0x60 (`i2c.md`: the board may not have it), antenna = headphone wire; FM gain
  fixed at 0 dB. If "NO TUNER ACK" shows, the analog FM path is still routed but there is nothing to hear.
* No stock code was found that enables the ADC IRQ together with the zos ISR, so the module polls; the ADC FIFO
  can overflow while the screen is redrawn (a few lost samples, harmless).
* Record length is ~1.1 s (RAM only, own page). To get more: 4-bit ADPCM or a core API for page_write/page_read.
