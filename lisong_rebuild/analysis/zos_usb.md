# zos USB modules: full-speed mass storage, CDC-ACM, and USB Audio Class capture

Page modules: `zos/modules/usb/` -> `USB.ZM` is only the launcher menu (skinned chrome, settings);
it chains to one function module per function and they chain back: `usbdisk/` -> `USBDISK.ZM`
(mass storage), `usbcon/` -> `USBCON.ZM` (CDC-ACM console), `usbaudio/` -> `USBAUDIO.ZM` (audio
class). The shared device layer is `zos/modules/common/usb/` (usb.h, usbdev.c and desc.c built with
USB_MSC_ONLY / USB_CDC_ONLY / USB_AUDIO_ONLY, usbz.inc, usbfn.c = the function-module frame); the
settings that outlive one module (writes, READ(10) path, 48 MHz, event-log position) are in the
HAL_SHARED block `usbsh.h`, the event log itself in ZRAM1 (`zr_t.logv`). Hardware reference: `usb_msc.md` (controller,
stock USBDISK.AP), `atj2085_hw.md` 1.1/2.4, disassembly `disasm_v04/USBDISK_AP/` (addresses
`b21:XXXX` = bank 0x21, `b20:` = bank 0x20, `text:` = AP text segment). Register names are the
`regs.h` names (section "USB full-speed SIE", exported to assembler as `regs.inc`).

Marks: **[stock]** = same sequence as USBDISK.AP (address given), **[giveio]** = as wiRe's
`giveio.asm` (working polled transport on this chip family), **[inferred]** = not seen in any
working code, chosen by reasoning; listed again in section 9.

---

## 1. What it is

* Polled USB 2.0 full-speed device (no USB interrupt: zos' ISR only handles RTC and DSP; `P_IRQEN`
  bit `IRQ_USB` stays 0). Every wait kicks the watchdog (`hal_watchdog`), checks VBUS
  (`P_USB_STAT` & `USBST_VBUS`) and bus reset (`P_USB_IRQ` & `USBIRQ_RESET`) and leaves through
  `longjmp(usb_jb)` - the stock ISR does the same with `jp sISREnd` / `Connect_ISR` (b21:1c49).
  MENU during a wait stops the module the same way.
* Three USB functions, chosen on the USB module screen (one at a time):
  1. **MASS STORAGE (RO)**: Bulk-Only Transport, SCSI subset, one LUN = the zos block device
     (`hal_blk_read`), read-only exactly like the stock firmware's write-protected LUN.
  2. **SERIAL CONSOLE**: CDC-ACM (Windows 10+: inbox `usbser.sys`), a small command line.
  3. **USB AUDIO** launches the companion module, where the source is selected. It provides
     USB Audio Class 1 input, mono 16-bit PCM at 32 kHz. The ADC
     samples either the microphone or the TEA5767 FM tuner; the radio is tuned to 98.7 MHz.
     Audio data uses isochronous EP2 IN (0x82), 64 bytes per 1 ms frame: only EP1/EP2 can run in
     ISO mode (PG p.22-23, 0x61 bit 1 ISO, bit 2 ISONBR = zero-length packet while unarmed); EP0 and
     EP3 are 8-byte endpoints (the first version used EP3 and the hosts could not stream). Its standard UAC1 endpoint
     descriptor is 9 bytes (including `bRefresh` and `bSynchAddress`). The two 32-sample buffers bridge
     the ADC FIFO to USB; excess samples are counted as drops.
* UI: LEFT/RIGHT choose, PLAY connects (pull-up on as soon as VBUS is present; unplug/replug is
  handled), PLAY or MENU disconnects, MENU (disconnected) leaves. VOL toggles the CPU clock 24/48
  MHz while disconnected (default 24.576 MHz = the stock USB setting). The screen shows the bus
  state, counters, and SETUP/bus-reset counts (bring-up diagnostics).

| file | content |
|---|---|
| `usb.h` | shared declarations, ZRAM1 scratch layout, ZRAM2 address map |
| `usbdev.c` | SIE bring-up, attach/detach, bus reset, endpoints, waits, EP0 standard requests, ZRAM2 access |
| `desc.c` | descriptors |
| `msc.c` | BOT + SCSI |
| `cdc.c` | CDC-ACM class requests + console |
| `usbaudio/main.c` | microphone/radio chooser and USB Audio run loop |
| `usbaudio/usbdev.c` | audio-only build of the shared USB device layer |
| `usbaudio/desc.c` | USB Audio Class descriptors |
| `usbaudio/audio.c` | ADC capture, radio tuner control, EP2 ISO IN packets |
| `usbz.s` | ZRAM1 helper template (block read into ZRAM2), `port_in`/`port_out` |
| `main.c` | screen, keys, run loop |

Audio isochronous packets are not modeled by the current emulator USB host. The USB emulator test
covers MSC/CDC regressions plus audio descriptors and alternate-setting control requests. Verify
enumeration and stable 32 kHz streaming on the player before relying on this mode.
The audio device revision is incremented when its descriptors change so Windows can distinguish
the corrected configuration from its previously cached descriptor set. The microphone/radio input
and USB streaming terminals have no associated-terminal IDs; the Feature Unit source ID and
AudioStreaming terminal link define the signal path. The Feature Unit does not advertise unsupported
feature controls. The isochronous endpoint advertises no sampling-frequency control, because the
device supports a fixed 32 kHz rate and does not implement endpoint SET_CUR/GET_CUR requests.

The USB module uses `hal_blk_read` for MSC and launches `USBAUDIO.ZM` through `mod_load`/`mod_run`;
these APIs are appended to `core/api.txt`.

## 2. Memory and buffer model

The SIE DMA reaches only ZRAM2 (B1 + B2, 2 KB each). ZRAM2 is shown in the 0x4000 window with
port 05 = `PAGE_ZRAM2`, i.e. it **replaces this module's own code**. So the module never touches
ZRAM2 directly:

* small copies go through the core's `page_write`/`page_read` (ZRAM1 code; the `xfer_t` and the
  source/destination are on the stack or in the ZRAM1 scratch);
* user-disk blocks for READ(10) are read **straight into ZRAM2** by `usbz.s`: a position-independent
  template copied at start-up to `ZR->stub` (= `HAL_MODRAM_BASE`, free while a page module runs).
  It saves port 05, maps `PAGE_ZRAM2`, calls `hal_blk_read(lba, dst)` through the API jump table
  (absolute address, SDCC `__sdcccall(1)`: lba in HL:DE, buffer on the stack, popped by the callee
  - checked in the generated `core_fat_c.asm`), restores port 05. hal_blk_read's NAND data phase is
  a PIO `ldir` into the buffer, so a ZRAM2 destination works.

ZRAM1 scratch (`zr_t` at `HAL_MODRAM_BASE` 0x3400): stub 48, ep0 128, rx 64, cbw 32, csw 16,
tx 512, sec 512, zero 64.

**Bank ownership** (`P_CLKCTL2` bits `CLK2_B1_MCU`/`CLK2_B2_MCU`, 1 = MCU): zos uses two layouts.

1. **Default: both banks USB-owned** (bits = 00), USB address space B1|B2 = 0x000-0xFFF [giveio:
   `usb_rx/usb_tx` with `0x70 &= 0xCF`, CBW at offset 0, data at 0x20 up to 4 KB]. Fixed areas:

   | USB offset | use |
   |---|---|
   | `ZO_RX` 0x000 (B1) | EP1 OUT: CBW, console input packet, WRITE(10)/MODE SELECT drain (<= 2 KB per arm) |
   | `ZO_EP0` 0x700 (B1) | EP0 data stage, both directions |
   | `ZO_TX` 0x800 (B2) | EP2 IN: replies, CSW, console output |

   The CPU copies with both banks MCU-owned (bits = 11: B1 at 0x4000, B2 at 0x4800), then sets the
   previous value back (`zr_put`/`zr_get`). The stock code also takes a bank to the MCU while EP1 OUT
   is armed (EP0 replies in its idle state, `control_read` b21:1ad9), so this is the stock risk level.
   With separate areas an EP0 reply never overwrites a received bulk packet, and the console can have
   EP1 OUT and EP2 IN armed at the same time (it does not, see 6).
   **READ(10), default ("flat", since device test 1)**: for each group of up to 4 blocks: both banks
   to the MCU, the ZRAM1 helper reads the blocks straight into B2 (CPU 0x4800-0x4FFF), both banks
   back to USB, one EP2 IN transfer of n*512 from ZO_TX; no overlap of NAND reads and USB. Uses only
   what the console proved on the device (same ownership values, same offset 0x800).
2. **READ(10), optional (console `rdmode 1`): the stock ping-pong** (f_b21_1bbc, sInvXYMemMapping b21:1c36): start with bits = 01
   (B1 USB, B2 MCU, seen at 0x4000); read up to 4 blocks into 0x4000..; wait until the previous IN
   transfer is done; toggle both bits (the filled bank goes to USB, the other one appears at 0x4000);
   arm EP2 IN at **offset 0** (= the USB-owned bank) for n*512; continue filling. At the end the banks
   go back to layout 1. No other endpoint is armed during READ(10) (one command at a time).

## 3. Register sequences

All [stock] unless marked. "arm" = `P_USB_EPI`=epi, `P_USB_EPMODE`=mode, `P_USB_MPS`=mps,
`P_USB_SAH/SAL`=offset, (bulk endpoints: `P_USB_EPIRQ` = 1<<epi), `P_USB_CNTH`=n>>8 (clears BR),
`P_USB_CNTL`=n (sets BR) (sSet_Ep*_Mode b21:1fa6-1ff0). After arming, wait (bounded) for BR = 1,
then "done" = BR (`P_USB_BR` bit 1<<epi) back to 0 (sWait_Ep0_Send_Out b21:1ff1, f_b21_1c49,
sWaitPrevRDTransEnd b21:1c5e), then ack the `P_USB_EPIRQ` bit.

| step | sequence | evidence |
|---|---|---|
| clock | `P_PLLCTL` \|= `PLL_HOSCEN`\|`PLL_PLL1EN`\|`PLL_M48EN` (only if not all set), wait 40 ms; CPU clock unchanged (zos default HOSC 24.576 MHz /1 = stock USB setting) | b20:1648; PLL lock time from clock.h |
| attach | refuse if no VBUS, or if `USBST_DP` and `USBST_DM` are both high with the pull-up off; else banks -> USB (00), `P_USB_ADDR` = `USB_ADDR_EN`, `P_USB_IRQ` = `P_USB_EPIRQ` = 0xFF, `P_USB_PULLUP` \|= `USB_PULLUP_EN` | UDiskRun b21:21ef (the IRQ enable is left out: polled) |
| bus reset | `P_USB_ADDR` = 0x80; repeat {for epi 0..7: EPI = epi, CNTH = 0} until `P_USB_BR` == 0 (VBUS checked); `P_USB_IRQ`, `P_USB_EPIRQ`, `P_USB_NAKST`, `P_USB_STALLST` = 0xFF; banks -> USB | USB_Reset_ISR b21:1637 |
| SETUP | `P_USB_IRQ` = `USBIRQ_SETUP`; `P_USB_SETUPST` & 0xDF must be 0x58, else stall; read `P_USB_SETUP0..7` | EP0_SETUP_ISR b21:16a4 |
| control IN | copy to ZO_EP0; len = min(len, wLength) if wLengthH = 0 and wLengthL != 0; len % 8 != 0: arm CR mode 3 (`EPMODE_CRLAST`) mps 0xC7; else arm CR mode 0 mps 0xC7, then if `SETUPST_LOCK` still set: ZLP mode 0 mps 0x07, and if still set: ZLP mode 3 mps 0x07; ack `EPB_CR` | control_read b21:1acf (offset 0x700 instead of 0: [inferred]) |
| status (no data) | arm CW mode 2 (`EPMODE_CWDONE`) mps 0xC7 count 0; wait; ack `EPB_CW` | no_data_control b21:1b46 |
| stall EP0 | arm CW mode 1 mps 0xC7 count 0 | syntax_err b21:1ac5 |
| control OUT data | arm CW mode 2 mps 0xC7 at ZO_EP0 count wLength; wait (0.5 s timeout); ack `EPB_CW`; read ZO_EP0 | **[inferred]**, stock never receives EP0 OUT data (used for CDC SET_LINE_CODING) |
| SET_ADDRESS | status stage first, then `P_USB_ADDR` = addr \| 0x80 | b21:1915 |
| SET_CONFIGURATION(1) | EP2 IN and EP1 OUT: mode 0, mps 0xBF (force DATA0), CNTH = 0; CDC: EP1 IN mode 0 mps 0x87 (8 bytes, DATA0); then EP1 OUT armed mode 0 mps 0x3F (toggle kept) for 64 at ZO_RX; then status | sInitEp1Ep2 b21:1944 (stock arms EP1 OUT directly with 0xBF) |
| clear halt | repeat {CNTH = 0} until BR clear; mode 0, mps 0xBF, offset; ack; EP1 OUT re-armed | clear_endpoint*_stall b21:1a0f/1a46 |
| set halt / invalid CBW | arm mode 1 (`EPMODE_STALL`) mps 0xBF count 0x40 on EP1 OUT and/or EP2 IN | b21:19d1/19df, not_a_valid_CBW b21:17c9 |
| CBW | armed EP1 OUT 64 at ZO_RX; ready when `P_USB_EPIRQ` & `EPB_EP1OUT`, or BR fell; read 31 bytes | EP1_OUT_ISR b21:16c2 is triggered by 53h bit 3 |
| bulk IN | copy to ZO_TX, arm EP2 IN mode 0 mps 0x3F, wait, ack `EPB_EP2IN` | f_b21_1f5c, sWait_Ep2_Send_Out b21:204f |
| detach / stop | pull-up off, all BR cleared, `P_USB_IRQ`/`P_USB_EPIRQ` = 0xFF, banks -> MCU (11); at module exit `P_PLLCTL` back to the value found (unless the CPU runs at 48 MHz) | UDiskExit text:0ee6 (stock writes 42h = 0x80) |

## 4. Descriptors (desc.c)

VID **0x1209 (pid.codes)**, PID **0x0001** (MSC) / **0x0002** (CDC): pid.codes' **test PIDs**
(private testing only, https://pid.codes/1209/0001/). Not the Actions VID 0x10D6, so Windows does not
bind the stock driver/s1res; different PIDs so Windows caches one driver per function.

* MSC device: `12 01 10 01 00 00 00 08 09 12 01 00 10 00 01 02 03 01`; configuration (32 B):
  `09 02 20 00 01 01 00 80 32 | 09 04 00 00 02 08 06 50 00 | 07 05 01 02 40 00 00 | 07 05 82 02 40 00 00`
  (class 08, **subclass 06 SCSI** (stock: 05), protocol 50 BOT; EP 0x01 OUT / 0x82 IN bulk 64 =
  stock endpoints). Strings: "zos", "zos USB disk (read-only)", serial "ZOS000000001".
* CDC device: `12 01 00 02 EF 02 01 08 09 12 02 00 10 00 01 02 03 01` (bcdUSB 2.00 because of the
  IAD; full speed only: GET_DESCRIPTOR(device qualifier) stalls). Configuration (75 B): config,
  IAD (`08 0B 00 02 02 02 01 00`), interface 0 = 02/02/01 with CDC header 1.10, call management
  (none, data interface 1), ACM (`bmCapabilities` 0x02: line coding + control line state), union
  0->1, **EP 0x81 interrupt IN 8 bytes, 16 ms (EPI 2, never armed = NAK forever)**; interface 1 =
  class 0A with EP 0x01 OUT and EP 0x82 IN bulk 64. Windows 10+ matches
  `USB\Class_02&SubClass_02&Prot_01` in its inbox `usbser.inf`; no driver needed.
* Standard requests: GET_DESCRIPTOR (1, 2, 3; others stall), SET_ADDRESS, SET/GET_CONFIGURATION
  (0/1), GET/SET_INTERFACE (alt 0), GET_STATUS (device/interface 00 00, endpoint = halt flag),
  SET/CLEAR_FEATURE (ENDPOINT_HALT on 0x01/0x82/0x81; device features accepted and ignored).

## 5. Mass storage (msc.c)

* **LBA mapping**: host LBA n = `hal_blk_read(n)` = UD sector 0x200 + n (the FAT volume's sector 0)
  = stock `Compute_LBA` b21:205a (`hl += 0x0200`).
* **Capacity** = the stock value (f_b20_16e4 on `UD_GetCap`): last LBA = 256 * cap - 0x8000 - 0x210
  with cap = RAM 0x008A (BREC's flash detect, 128 KB units; 0x1EC0 on this player, fallback 0x1EC0)
  -> **last LBA 1 981 936 (0x1E3DF0), 1 981 937 blocks**. Check: the FAT boot sector of the user disk
  says 1 981 937 sectors (the volume was formatted by Windows through the stock driver, so the stock
  READ CAPACITY returned this value as the *last LBA*); the zos FTL has 1 981 952 blocks after
  0x200, so every reported block exists. READ FORMAT CAPACITIES reports last + 1 blocks (the stock
  table repeats the last-LBA value there and uses descriptor code 01; zos uses 02 = formatted).
* Data phases always move exactly dCBWDataTransferLength bytes in the host's direction (replies
  zero-padded, as the stock `ldir bc=(28c6h)`), so no phase errors / stalls in normal operation.
* Commands: TEST UNIT READY, REQUEST SENSE (fixed 18 bytes, cleared after reading), INQUIRY (36 B,
  removable - required: the volume is a superfloppy without MBR; EVPD -> 05/24), READ CAPACITY(10),
  READ FORMAT CAPACITIES, MODE SENSE(6) `03 00 80 00` and (10) `00 06 00 80 00 00 00 00` (WP bit
  in the device-specific byte; the stock reply puts it at the MODE SENSE(10) position for both),
  MODE SELECT(6/10) (drained, ignored), PREVENT/ALLOW, VERIFY(10), SYNCHRONIZE CACHE (OK), START STOP
  UNIT (LoEj: "ejected" -> NOT READY 02/3A until unplug/disconnect), READ(10) (ping-pong, section 2),
  **WRITE(10) (default, writes off): data drained, CSW status 1 with residue = dCBWDataTransferLength,
  sense DATA PROTECT 07/27** (lsSendCSWInWriteFlash b21:1cfa + sFillErrCSW b21:1f86).
  **Writes on** (console `wr on`, refused at 48 MHz; VOL to 48 MHz turns them off; the menu shows
  "MASS STORAGE (RW)", INQUIRY "USB DISK RW", MODE SENSE without WP): WRITE(10) receives up to 4
  blocks per arm into B1, copies each to `ZR->sec` (ZRAM1, the DMA source) and writes it with
  `ftlw_write` (`modules/common/ftlw.inc`, nand_ftl.md 3.1); a failure -> MEDIUM ERROR 03/0C (all
  data still received), the error is sticky until `wr on` again. The open FTL unit is closed by
  SYNCHRONIZE CACHE (03/0C on failure), START STOP UNIT, 1 s without writes (msc_poll), a READ(10)
  touching it (`ftlw_sync`) and when the function is disconnected (PLAY/MENU). Windows reads the WP
  bit when the disk appears: enable writes before connecting MASS STORAGE. Others: zero data / drain,
  ILLEGAL REQUEST 05/20. READ out of range 05/21, block read error MEDIUM ERROR 03/11.
* Invalid CBW: stall EP 0x01 and 0x82 until the host's reset recovery (BOT reset `21 FF`, CLEAR_FEATURE
  on both) [stock]. GET_MAX_LUN = 0.
* Not implemented (as in the stock): READ(6), READ(12)/(16), REPORT LUNS, Actions vendor CDBs
  0xCB/0xCC (on purpose: no ADFU switch from zos), the "ActUSBC" LBA 0/1 protocol.

## 6. CDC-ACM console (cdc.c)

* Class requests: SET_LINE_CODING (control OUT data stage, [inferred] sequence, stored only),
  GET_LINE_CODING (default 115200 8N1), SET_CONTROL_LINE_STATE (DTR rising -> greeting + prompt;
  shown on the LCD), SEND_BREAK (accepted).
* Half duplex on purpose: one 64-byte OUT packet is armed at a time; after it arrives the line
  editor runs, the output is sent, then OUT is re-armed (the host's OUT tokens are NAKed meanwhile).
  Output transfers are never a multiple of 64 (no ZLP needed); a transfer the host does not pick up
  within 1 s is dropped (no terminal open) and counted. SETUPs are served while output waits.
* **Received length [inferred workaround]**: no stock code receives short bulk packets of unknown
  length (CBWs are fixed 31 bytes), and the meaning of reading `P_USB_CNTH/CNTL` after a transfer is
  not established. The console therefore zero-fills the receive area before arming and takes the
  text up to the first NUL. The 64h/65h read-back after each packet is kept and printed by `usb`
  ("last cbc") so it can be determined on the device (needed for any binary protocol).
* Commands (numbers hex, `0x` optional; input is lower-cased; BS/DEL edit; CR, LF or CRLF = Enter):

  | command | does |
  |---|---|
  | `help` / `?` | list |
  | `ver` | module version, VID:PID, disk last LBA |
  | `usb` | ports 0x50-0x6A, 0x3E, 0x70, 0x42; state, SETUP/reset counters, control-OUT ok/timeouts, rx packets, last CBC read-back, tx drops, line coding, DTR |
  | `peek ADDR [LEN [PAGE]]` | hex dump (LEN <= 0x200). Without PAGE: the CPU's view (0x4000-0x7FFF = this module's page; 0x8000+ = the external bus with the LCD selected - avoid). With PAGE: 0x4000-0x7FFF of that port-05 page via `page_read` |
  | `poke ADDR VAL` | write a byte |
  | `in PORT`, `out PORT VAL` | I/O ports (`out 05` refused: it would unmap the module) |
  | `sec LBA` | hex dump of a user-disk block (`hal_blk_read`) |
  | `fw N` | hex dump of firmware-area sector N (`hal_fw_read`, FWIMAGE.FW sector N) |
  | `reset` | reboot through the watchdog (`WDT_REBOOT`) |
  | `wr [on\|off]` | mass-storage writes (default off; on only at 24 MHz; clears a sticky write error); shows state + error code (ftlw.h) |

## 7. Adding a DFU-style firmware-area function later (design only, no flash writes implemented)

* **Read** is already there in text form (`fw N`). A binary path: add a third function (PID 0x0003)
  or a second CDC-like data interface and a vendor control request `bmRequestType 0xC1`,
  `bRequest 0x01 READ_FW`, wValue = sector, wLength <= 512 -> `hal_fw_read` into `ZR->sec`, then the
  EP0 IN path (one control read of up to 512 bytes = 64 packets; check that a single CR arm can be
  that long, else loop in 64-byte arms using `SETUPST_LOCK`). Bulk would be faster: a 31-byte request
  block on EP1 OUT + data on EP2 IN, i.e. the MSC BOT engine with vendor CDBs (as the Actions ADFU
  does over the same endpoints, usb_msc.md 4.1).
* **Write** (later, needs the NAND program path of `nand_ftl.md` 3): DFU 1.1 (interface class FE/01/02,
  functional descriptor, DFU_DNLOAD/GETSTATUS/MANIFEST) is the standard choice (Windows: WinUSB via an
  MS OS 2.0 descriptor, dfu-util). DFU_DNLOAD blocks arrive as EP0 OUT data stages, so first confirm
  the [inferred] control-write sequence (the console's SET_LINE_CODING already exercises it: `usb`
  shows "ctrl-out ok"). Stage the image in DSP RAM pages (page_write), verify a checksum, only then
  program; never touch BREC blocks / the first blocks of CE1 (usb_msc.md 4.2 warning). The bulk
  receive length must come from the CBC read-back (section 6) or a length field in the protocol.

## 8. Testing

* **Emulator, with a host model** (new, does not change existing emulator code):
  `atj2085emu/emu/usbhost.py` models the SIE at the "arm" level (BR/EPIRQ/SETUP registers, bank
  ownership incl. the CPU-visible bank swap of port 0x70, DMA address space = USB-owned banks) plus a
  scripted host. `zos/tools/emu_usb_test.py` (run with `lisong_rebuild/.venv/Scripts/python.exe`)
  injects USB.ZM into the NAND dump (nandinject overlay), boots stock -> ZOS.AP, opens MODULES -> USB,
  connects and runs 106 checks (after device test 1, see 10.1): enumeration of both functions (descriptors with wLength 64/18/9/255,
  SET_ADDRESS timing, strings, qualifier stall, GET_STATUS/CONFIGURATION), MSC (INQUIRY, TUR,
  READ CAPACITY = stock formula = boot-sector count - 1, MODE SENSE 6/10 WP, READ FORMAT
  CAPACITIES, READ(10) LBA 0 and 9 blocks through the 4+4+1 ping-pong compared byte-for-byte with
  `ftlread.py`, WRITE(10) -> drained + 07/27, out-of-range read 05/21, unknown opcode 05/20, invalid
  CBW -> stall -> reset recovery, eject -> 02/3A), function switch, CDC (SET/GET_LINE_CODING
  round trip, DTR greeting, `help`, `ver`, `in 50`, `peek 803 6`, `sec 0` dump = boot sector,
  backspace editing, unknown command). Result: all passed (about 18 s wall time).
  **The model implements the same inferences as the firmware**; it proves the firmware logic and
  buffer discipline, not the silicon behaviour.
* **On the device**: not tested yet (section 10 is the checklist).

## 9. Verified vs inferred

| item | status |
|---|---|
| ports 0x50-0x6A meaning as used: VBUS, address, 52h/53h W1C status, SETUP status/bytes, EPI, mode values 0/1/2/3 for the stock uses, SA, count order, MPS/toggle values, BR | stock code (works on this player) |
| polling 52h/53h with the USB interrupt disabled (status bits latch without `P_IRQEN` bit 2) | **inferred** (the stock reads them inside its ISR; giveio polls only 68h) - risk 1 |
| SIE DMA offsets with both banks USB-owned = linear B1\|B2; with one bank USB-owned offset 0 = that bank; CPU sees a single MCU bank at 0x4000 | giveio (both) / stock READ ping-pong (single) |
| EP0 data at offset 0x700 (stock: offset 0) | inferred from the above |
| EP0 OUT data stage (EPI 1, mode 2, count n) | **inferred** - risk 2 (only the console's SET_LINE_CODING) |
| short bulk OUT packet ends the arm (53h bit 3 set) | stock (CBW of 31 bytes on a 64-byte arm triggers the ISR) |
| CBC read-back meaning, received length | unknown, worked around (zero fill) |
| EP1 IN (EPI 2) as an 8-byte interrupt endpoint that NAKs while unarmed | inferred (EPI 2 exists: 53h/68h bit 2; never armed by any known code) |
| SIE DMA independent of the MCU window (port 05 = module page 0xC1 while transfers run) | inferred (the stock AP main loop runs with other pages while the ISR's transfers proceed) |
| capacity formula, LBA mapping, WP behaviour | stock code + boot-sector cross-check |
| CPU at 48 MHz during USB | untested (VOL toggle; default 24.576 MHz = stock) |

## 10. On the device

1. Copy `zos/out/atj2085/USB.ZM` (and the new `ZOS.AP`: the API table grew by one entry; older
   ZOS.AP builds refuse USB.ZM with "API" error) to the disk root; start zos -> MODULES -> USB.
2. **Mass storage**: MASS STORAGE (RO) selected -> PLAY -> plug the cable. Screen: USB CONFIGURED,
   CMDS/SECT counting, MB 967. Windows: a new **removable drive** "zos USB DISK RO" (Device Manager:
   USB Mass Storage Device, VID_1209&PID_0001), ~967 MB FAT, files readable; copying a file **to** it
   fails with "The disk is write-protected". Eject in Explorer -> screen DISK EJECTED.
   Optional speed check: copy a large MP3 from the drive (expect several hundred KB/s; VOL=48 MHz
   before connecting may be faster).
3. **Console**: PLAY (off) -> RIGHT -> PLAY. Windows: "USB Serial Device (COMn)" under Ports
   (VID_1209&PID_0002). Open it with PuTTY (Serial, COMn, any speed) or `Windows Terminal`/TeraTerm;
   the screen shows PORT OPEN (DTR) and the terminal prints `zos USB console - type help` + `zos> `
   (if the terminal does not set DTR: press Enter). Try: `help`, `ver`, `usb`, `in 50` (expect 4x or
   6x: VBUS), `peek 803 30`, `sec 0` (boot sector, ends with `55 AA`), `fw 0`, `peek 4000 20 c2`.
   Report the `usb` output (ctrl-out ok / timeout, last cbc after typing one character).
4. If nothing enumerates: note the LCD state and the SETUP / RST counters:
   * stays ATTACHED, RST 0: the bus reset is not seen by polling (risk 1);
   * RST > 0, SETUP 0: SETUPs are not seen by polling (risk 1);
   * SETUP counts but no CONFIGURED: an EP0 detail (Windows: "Device descriptor request failed").
   * console enumerates but the port cannot be opened / PuTTY "unable to configure": risk 2 (`usb`
     shows timeout > 0).

### 10.1 Device test 1 (coordinator) and what changed

Result: SERIAL CONSOLE fully works on Windows (enumeration, SET_LINE_CODING control-OUT data, bulk
both ways) -> risks 1 and 2 below are retired. MASS STORAGE: Windows loads the USB storage driver
but no disk / drive letter appears.

Analysis: every reply before the first READ(10) (GET_MAX_LUN, INQUIRY, READ FORMAT CAPACITIES, READ
CAPACITY, MODE SENSE, TEST UNIT READY, REQUEST SENSE, CSW) uses exactly the primitives the console
proved (EP1 OUT at 0x000, EP2 IN from 0x800, both banks USB-owned, CPU copies with both banks MCU-
owned, control reads of 1-255 bytes) and their contents match what is known to work with Windows
(TinyUSB-style header-only MODE SENSE, fixed sense, 36-byte INQUIRY with RMB). The only device-
unproven code in mass storage was READ(10): the single-bank modes 01/10 of the stock ping-pong (CPU
sees one bank at 0x4000, USB offset 0 = the other bank) and the ZRAM1 helper writing into ZRAM2. A
READ(10) that never completes makes usbstor time out, reset, retry and finally give the device up,
which matches "driver loaded, no disk". Changes:

* READ(10) default = the "flat" path (section 2), built only from proven primitives; the stock
  ping-pong stays available with the console command `rdmode 1` (the setting survives switching
  functions inside the module) and the LCD shows "RD FLAT" / "RD PP".
* Diagnostics: a 128-entry mass-storage event log (SETUPs, every SCSI command with status / sense /
  LBA / length, invalid CBWs, host halt/clear, bus resets, unplugs, and **commands aborted** by a reset
  or unplug with the stage, data or CSW). Read it after a mass-storage attempt: PLAY (off) -> RIGHT ->
  PLAY -> terminal -> `log` (`log clear` resets it). The LCD shows the last opcode and OK/FL.
* Capacity cross-check: the boot sector's sector count is read at start; `ver` prints RAM 0x008A,
  the boot-sector total and the reported last LBA (the boot value wins if it is larger; it equals
  the stock value on this player).
* Emulator test now replays the Windows attach sequence (GET_MAX_LUN, INQUIRY, READ FORMAT
  CAPACITIES, INQUIRY, READ CAPACITY, MODE SENSE(6) 3F/C0, 1C/C0, 08/04, 3F/04, TUR, READ(10) 1 and
  8 blocks at 0, INQUIRY VPD 80 -> fail + REQUEST SENSE, PREVENT, READ(10) of 128 blocks, of the last
  8 blocks, 9 blocks at 300000) once with the flat path and once with the ping-pong, and checks `log`.

What to test: MASS STORAGE again (LCD must say RD FLAT). If the drive appears, also try `rdmode 1`
(console) -> MASS STORAGE to see whether the stock ping-pong works on this silicon (faster). If it
still fails, send the console `log` output and the LCD "LAST" line.

## 11. Open risks

1. ~~Polling 52h/53h without the USB interrupt~~ - works (device test 1, console).
2. ~~EP0 OUT data stage~~ - works (device test 1: SET_LINE_CODING).
3. CPU taking both banks (bits 11) for a few microseconds while an endpoint is armed (stock does
   the same with one bank); a packet arriving exactly then is undefined.
4. EP 0x81 (EPI 2) as interrupt endpoint: assumed to NAK while unarmed.
5. Received console packet length from NUL scan (text only).
6. 48 MHz CPU with USB untested; NAND read timing at 48 MHz is the one of the FILES module.
7. Throughput not measured (default READ(10) = flat path without overlap; per block one `hal_blk_read`
   with PIO NAND read).
