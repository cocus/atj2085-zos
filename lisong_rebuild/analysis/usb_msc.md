# USB device controller and mass-storage (MSC) on the ATJ2085X / LISONG LSD_501 (v04)

Research note for zos (the new OS). No zos code, no firmware changes. Everything below is from:

* `USBDISK.AP` v04 disassembly (`lisong_rebuild/disasm_v04/USBDISK_AP/`, banks 0x20/0x21/0x22 and `seg_text.s`), `ADFUF641.AP` (`disasm_v04/ADFUF641_AP/`)
* the **mask ROM (BROM)**, 8 KB, which is stored in the header of `readback-my-ximpro.s1clone` (first 16408 bytes: 24-byte header + 16 KB = the 8 KB ROM mirrored twice). Extracted read-only to `analysis/brom_8k.bin` (md5 f2f7122a86ef3a21295fdba82528b216). Disassembled with `tools/z80dis.py`.
* `SYSCFG.SYS` kernel (VBUS poll), `BRECF641.BIN`, `MAIN.AP`
* wiRe's s1mp3 sources (`s1mp3_github/`: `AdfuSession.cpp`, `AdfuDeviceIoctl.h`, `giveio.asm`, `s1reg.inc`), s1flash.py
* ATJ2085X datasheet v1.8 (text extracted: section 4.15) and `analysis/atj2085_hw.md` (Programming Guide summary, section 2.4)
* Actions SDK (ATJ2085/62720720AK1011.../case_i_d913f/ap/ap_udisk: `usbdisk.c`, `usbdisk_2l.c`, `apadfu_3l.c`)

Marks: **(fact)** = read in code / ROM / sources. **(inferred)** = deduced, not proven on hardware.
Addresses `bank21:XXXX` are AP-image addresses in the USBDISK.AP bank 0x21 (load 0x1600).

---

## 1. Summary

| Question | Answer |
|---|---|
| USB block | Actions "USB 2.0 FS" SIE, **full speed only** (datasheet 4.15: "USB 2.0 (FS)"; descriptors bcdUSB 1.10). Datasheet claims MSC throughput max **read 1033 KB/s, write 955 KB/s**. |
| Registers | **0x50..0x6A**, plus 0x3E (pull-up), 0x27 bit2 (IRQ enable), 0x26 bit2 (IRQ status), 0x70 bits 5:4 (buffer ownership), 0x42 (48 MHz clock). |
| Endpoints | EP0 (8-byte packets, control read/write as two separate hardware endpoints), EP1/EP2/EP3 each IN+OUT. Datasheet: EP1/EP2 bulk or ISO (64 / up to 1023 B), EP3 8 bytes. The stock MSC uses **EP1 OUT (0x01) and EP2 IN (0x82), both 64-byte bulk**. |
| Data movement | **Not PIO, not DMA1/DMA2.** The SIE has its own bus-master DMA that reads/writes **ZRAM2** (B1 + B2, 2 KB each, MCU window 0x4000-0x4FFF). The CPU programs start offset (0x62/0x63) and byte count (0x64/0x65) and polls buffer-ready (0x68). Ownership of B1/B2 (CPU or USB) is switched with port 0x70 bits 5:4, giving 2 KB ping-pong. USB DMA does **not** halt the CPU. |
| DMA1 use | Only memory-to-memory copies (ADFU memory commands, NAND page read in the UD/SD drivers). DMA2 is used by the BROM for NAND. Neither is used for USB data. |
| Connect detect | **0x50 bit6 = VBUS** (RO). Kernel polls it from its tick routine (SYSCFG 0x2F38) and posts message 0x91 (plug) / 0x92 (unplug). USB IRQ 0x52 bit6 = connect/disconnect. |
| Soft connect | **0x3E bit7 = internal D+ pull-up enable** (bits 2:0 select 1.1..1.8 kohm, default 4 = 1.5 kohm). |
| Clock | Needs the 48 MHz PLL output: **0x42 |= 0xE0** (HOSCEN, PLL1EN, M48EN), then 0x00 = 0x10 (MCU from HOSC 24.576 MHz, /1). Suspend: 0x42 = 0, 0x00 = 0. |
| IRQ | 0x26 bit2 / 0x27 bit2 (USB). Sources: 0x52 (bus reset, connect, SETUP, wakeup) and 0x53 (per-endpoint). The stock ISR does *all* SCSI work inside the interrupt. |
| MSC stack | Bulk-Only Transport inside the ISR (`bank21:16c2`). 15 SCSI opcodes + 2 Actions vendor opcodes (0xCB, 0xCC) + a vendor "ActUSBC" protocol in writes to LBA 0/1. |
| Host-visible disk | LUN0 = NAND user disk; **host LBA n = UD logical sector n + 0x200**, so the VM settings area (UD sectors 0..511) and the firmware area (system-disk driver) are **hidden**. |
| ADFU / upgrade | Two different things: (a) **firmware ADFU**: entered from the running firmware by vendor SCSI opcode 0xCB over the MSC connection (this is what s1res / s1flash / vendor updater use). (b) **ROM recovery ADFU** (VID 10D6 PID FF51, class FF/FF/FF): BROM runs it only when it cannot load a valid BREC from NAND. **No key, no GPIO, no pin is read by the BROM.** |

---

## 2. USB controller hardware

### 2.1 Register map (all (fact) unless marked; names from s1reg.inc / PG, semantics confirmed by code)

| Port | Name | Evidence / semantics |
|---|---|---|
| 0x3E | USB pull-up | bit7 = pull-up enable (soft connect). `bank21:21ef` UDiskRun sets it (`or 0x80`); `UDiskExit` (text 0x0ee6) clears it **except** when switching to ADFU (0x2801 == 0xFF), so the host never sees a disconnect. BROM sets it at ROM 0x02b3. |
| 0x50 | status/control | RO bits: **6 = VBUS**, 5 = D+, 4 = D-. bit3 = bus-active, **W1C** (write 0x08 clears it: `text 0x0fc6`.. `UsbActive_c`). bit7 = suspend (RW): `UsbSuspendUsb` writes 0x80, then polls until VBUS gone or bit7 cleared by the hardware (resume). |
| 0x51 | device address | bit7 = software enable (must be 1), bits 6:0 = address. Bus reset ISR writes **0x80** (addr 0, enabled). SET_ADDRESS writes `addr | 0x80` **after** the status stage (`bank21:1908`). |
| 0x52 | global IRQ status, W1C | bit7 bus reset, bit6 connect/disconnect, bit5 SETUP, bit4 wakeup/resume. ISR order: 7, 5, 6, 4 (`bank21:1600`). `0xFF` written clears all. |
| 0x53 | endpoint IRQ status, W1C | bit0 control-read done, bit1 control-write done, bit2 EP1 IN, **bit3 EP1 OUT**, bit4 EP2 IN, bit5 EP2 OUT, bit6 EP3 IN, bit7 EP3 OUT. Stock code acks with `0x01`, `0x02`, `0x08` (EP1 OUT), `0x10` (EP2 IN), `0x18` (both). |
| 0x54/0x55 | frame number | not used by stock firmware. |
| 0x57 | SETUP status | `in a,(0x57); and 0xDF; cp 0x58` = valid(6) + ack-sent(4) + count 8. Anything else -> stall EP0. bit5 (SETUP lock) is masked, and is read again after control IN data to decide on the extra zero-length packet. |
| 0x58..0x5F | SETUP packet bytes 0..7 | bmRequestType, bRequest, wValueL, wValueH, wIndexL, wIndexH, wLengthL, wLengthH. |
| 0x60 | endpoint index (EPI) | 0 = control-read (CR, IN data stage), 1 = control-write (CW, status stage / OUT data), 2 = EP1 IN, **3 = EP1 OUT**, **4 = EP2 IN**, 5 = EP2 OUT, 6/7 = EP3 IN/OUT. All of 0x61..0x66 act on the selected EPI. |
| 0x61 | endpoint mode | Values used: bulk = 0x00 (normal); control status stage (EPI 1) = 0x02; **stall = 0x01** (EPI 1 for control stall, EPI 3/4 for halt); control IN data (EPI 0) = 0x03 when the transfer ends with a short packet, 0x00 when more packets/ZLP follow. (The PG summary lists bit0 as "complete" for CR/CW; the code uses 0x01 as the stall value on every endpoint, so treat bit0 = STALL.) |
| 0x62/0x63 | start address H/L | Offset of the transfer inside the **USB-owned part of ZRAM2**. Always 0 in the stock MSC code. |
| 0x64/0x65 | byte counter H/L | Write 0x64 first (also **clears BR**), then **0x65 last = loads count and sets BR (arms the endpoint)**. Max 0x0800 in the stock MSC. |
| 0x66 | MPS / data toggle | write: bit7 = force DT, bit6 = DT value, bits 5:0 = MPS-1. EP0: **0xC7** (force DATA1, 8 bytes). Bulk: 0x3F (64 B, toggle untouched), **0xBF** (force DATA0, 64 B) at SET_CONFIGURATION / clear-halt. |
| 0x68 | buffer-ready status (RO) | bit0 CR, bit1 CW, bit2 EP1 IN, **bit3 EP1 OUT**, **bit4 EP2 IN**. BR=1 means armed / busy; it falls to 0 when the SIE has finished the transfer. All completion waits are "wait BR=1, then wait BR=0" (`sWait_Ep2_Send_Out`, `bank21:204f`). |
| 0x69/0x6A | NAK status / STALL status | W1C; only cleared with 0xFF at bus reset. |
| 0x70 | MCU clock/B1,B2 map | bit5: B1 -> MCU(1)/USB(0), bit4: B2 -> MCU(1)/USB(0). See 2.3. |
| 0x3E, 0x42, 0x00 | clocks | see section 1. |
| 0x27 / 0x26 | master IRQ enable / status | bit2 = USB. Stock ISR is entered via the kernel IRQ table (`IRQ_Intercept`, see 2.5). |
| 0x22/0x23/0x24 | CTC1 | used as a bus-idle timer for suspend, see 2.5. |

Do **not** use the SDK header `sdk_i_913f/inc/UsbReg.h` for this chip: it describes a different, high-speed capable USB block (registers 0x3E, 0x8C, 0xEC, 0x50..0x6F with other meaning; GL3961 / "v9" devices). The same applies to the `v9` branch in `giveio.asm` (ports 0x58.., 0x89/0x8A). The LISONG board has the older block (the one in s1reg.inc and in the ROM).

### 2.2 Endpoints and descriptors served by the firmware

Everything is answered by the AP, in the SETUP ISR (`EP0_SETUP_ISR bank21:16a4` -> `parsing bank21:17f6`). No hardware auto-response.

* Device descriptor (18 B) at AP address **0x1130**: `12 01 10 01 00 00 00 08 | D6 10 | 00 10 | 00 01 | 00 01 02 01`; i.e. bcdUSB 1.10, class 0 (per interface), **bMaxPacketSize0 = 8**, VID 0x10D6, PID 0x1000, iManufacturer 0, iProduct 1, iSerial 2, 1 configuration. **VID/PID are overwritten at init** with the 4 bytes from the firmware header (`UDiskInit` copies `vidpid` to 0x1138): this device = **10D6:1100** (FWIMAGE header offset 0x0C).
* Configuration (32 B) at **0x1142**: `09 02 20 00 01 01 00 80 32` (bus powered, 100 mA), interface `09 04 00 00 02 08 05 50 00` (**class 8, subclass 5 (SFF-8070i), protocol 0x50 = Bulk-Only**, 2 endpoints), endpoint `07 05 01 02 40 00 00` (EP1 OUT bulk 64) and `07 05 82 02 40 00 00` (EP2 IN bulk 64).
* Strings: index 0 = LANGID `04 03 09 04` (0x28a1); index 1 **and** 2 = the 48-byte UTF-16 string descriptor from the firmware header (here "GENERIC USB DISK DEVICE"), pointer in 0x11db (copied from FWIMAGE header offset 0x120, `GetUsbSetupInfo` in `usbdisk.c`). Others -> stall.
* Std requests handled: GET_DESCRIPTOR device/config/string (types 1,2,3), GET_STATUS (dev/if/ep), GET_CONFIGURATION, GET_INTERFACE, SET_ADDRESS, SET_CONFIGURATION (0/1), SET_INTERFACE (0), SET_FEATURE/CLEAR_FEATURE(ENDPOINT_HALT) for 0x01 and 0x82. **Class**: `0x21/0xFF` Bulk-Only Mass Storage Reset (just flags, no real reset), `0xA1/0xFE` GET_MAX_LUN (returns byte at 0x122b). Anything else: EP0 stall (`syntax_err`, bank21:1ac5). No device-qualifier, no BOS, no OTG.
* GET_DESCRIPTOR length: `get_descriptor_length` clips to min(wLengthL, table length) (wLengthH must be 0).
* The first GET_DESCRIPTOR(device) also sets port 0x22 bit7 (`get_dev_dcp`, bank21:1b59): starts the CTC1 idle timer (inferred purpose).

EP0 sequences (all (fact)):

* IN data stage: copy bytes to 0x4000 (CPU sees B1+B2 with 0x70|=0x30 and 0x05=0xF7), give B1 to USB (0x70 = (0x70 & 0xCF) | 0x10), `60=0, 61=mode, 66=0xC7, 62=63=64=0, 65=len`, wait 0x68.0 =1 then =0, `53=0x01`. If len is a multiple of 8 the code sends an additional zero-length packet when 0x57.5 says so (`control_read`, bank21:1acf).
* No-data request / status stage: `60=1, 61=0x02, 66=0xC7, 62..64=0, 65=0`, wait 0x68.1, `53=0x02` (`no_data_control`, bank21:1b46).
* Error: `60=1, 61=0x01, 66=0xC7, 65=0` = STALL.

### 2.3 How data moves (the 2 KB ping-pong)

* The CPU window 0x4000-0x4FFF shows ZRAM2 only when port 0x05 = 0xF7 (lane 111) **and** 0x70 bits 5:4 give the CPU the bank (01: B2 at 0x4000-0x47FF, 10: B1 at 0x4000-0x47FF, 11: both at 0x4000-0x4FFF, 00: none). (PG, atj2085_hw.md 1.1.)
* The SIE DMA offset (0x62/0x63) is relative to the part owned by USB (inferred from the code: with both banks USB-owned giveio uses offsets 0..0xFFF over B1+B2, i.e. 0x4000.. in CPU terms; with only one bank USB-owned the stock MSC code always uses offset 0 for "the bank USB owns").
* Stock layout: **B1 (CPU 0x4000) = host-to-device buffer** (CBW, WRITE data), **B2 (CPU 0x4800 when both mapped) = device-to-host buffer** (CSW, READ data, INQUIRY...). Receive CBW: `0x70 = (..&CF)|0x10` (USB owns B1), EPI 3, count 0x40 (arm for 64), later `0x70 |= 0x30` and read the CBW at 0x4000 (`bank21:16c2`). Reply: `0x70 |= 0x30`, copy to 0x4800, `0x70 = (..&CF)|0x20` (USB owns B2), EPI 4, `66=0x3F`, `62=63=0`, `64:65 = length` (`bank21:1b8d`, `1f5c`).
* READ(10) ping-pong: CPU reads **4 sectors (2 KB)** from the sector backend into the CPU-owned bank, kicks the watchdog, waits for the previous IN transfer to finish (0x68.4 falls), **swaps ownership** (`sInvXYMemMapping`, bank21:1c36: toggles 0x70 bits 5:4), arms EP2 IN with 0x0800 and immediately starts reading the next 2 KB (`f_b21_1bbc`, bank21:1bbc-1c35). WRITE(10) is the mirror image (`CMD_Write10`, bank21:1c73): arm EP1 OUT for 2 KB, while the previous 2 KB is programmed to NAND (`f_b21_1d33` -> `UD_SectorWrite` per sector).
* DMA channel: **the USB SIE internal DMA** (no DMA1/DMA2). The datasheet says nothing more; the PG says USB DMA does not stop the MCU while DMA1/DMA2 do (atj2085_hw.md 1.1/2.1).
* DMA1 (ports 0x06..0x13) in the USB code: only in the ADFU memory-copy command (`sDma1MoveDataToB2Mem`, `f_b22_1bd9`: src/dst lane 7 = ZRAM2, mode 0x12 = 0, start `0x13 = 1`, poll bit0) and in the BROM download (ROM 0x08cb). The NAND UD/SD drivers also use DMA1 (+ ECC 0xCC-0xCF) for page reads (port scan of `UD_SF641.DRV`).

### 2.4 Interrupt use and connect/disconnect

* ISR `ADFUISR_b21` at **bank21:1600** (name from the SDK/ADFU heritage): saves port 0x05 and SP (0x28be/0x28bc), dispatches on 0x52 / 0x53, and on exit restores SP and port 0x05. Every blocking wait inside also tests 0x52.7 (bus reset) and 0x50.6 (VBUS): on reset it **unwinds the stack to the ISR exit** (`jp sISREnd`), on VBUS loss it jumps to `Connect_ISR` (bank21:17e4) which sets flag 0x28de (-> AP state 0x40 = unplug exit).
* Bus reset (`USB_Reset_ISR`, bank21:1637): clears the software state, `0x51=0x80`, clears BR of all 8 endpoints (loop `60=n; 64=0` until 0x68 == 0), then `0x52=0x53=0x69=0x6A=0xFF`.
* IRQ install: `UDiskRun` (bank21:21ef) calls `IRQ_Intercept` (RST 10h, HL=0x0000, DE=0x1600, C=2) -> USB IRQ index 2; init then `0x52=0x53=0xFF`, `0x27|=0x04`, `0x3E|=0x80`. Release: `IRQ_Release` (HL=0x0100) in `UDiskExit` (text 0x0ee6).
* Disconnect before connect: `UDiskRun` refuses to arm if VBUS (0x50.6) is low, or if D+ and D- are both high (0x50 & 0x30 == 0x30) while the pull-up is off (inferred: host/charger fault guard).
* **Idle/suspend timer**: `f_c_0fc6` installs a second intercept (C=4: DMA/CTC) on `f_c_0faa` and programs CTC1 (`0x22=0, 0x23=0, 0x24=0x30`, enable `0x27 bit4`). Handler: ack `0x25=0x80`; if 0x50.3 (bus active) is set or D+/D- are both low, clear it (`0x50=0x08`) and reset the counter (0x1249); else increment. When 0x1249 >= 0xB4 `UDiskHandle` flushes (`UD_Update`), masks all IRQs (0x27=0) and enters suspend (state 0x80): `0x50=0x80; 0x00=0; 0x42=0;` wait, then `0x42=0xE0; 0x00=0x10`.
* **VBUS plug/unplug for the OS** (not the AP): `SYSCFG.SYS` kernel tick routine at 0x2F38: `in a,(0x50) & 0x40` compared with the saved bit in 0x0134; on change it posts system message **0x91 (plug)** or **0x92 (unplug)** into the queue 0x0115 (`ld a,0x91/0x92` at 0x2F86-0x2F8A). Same routine posts 0x82 (low battery) from port 0xD0 when VBUS is absent. MAIN.AP then starts USBDISK.AP. zos can do the same by polling 0x50.6.

### 2.5 Clock / power

* USB needs 48 MHz: `0x42 |= 0xE0` before anything (UDiskInitInternal bank20:163d, ROM 0x027e, ADFU exit also writes 0x80/0x00). 0x42 bits 7:5 = HOSCEN, PLL1EN, M48EN (PG). Datasheet 4.7: "the clock used in serial communications is 48 MHz".
* Stock sets 0x00 = 0x10 (MCU clock source = HOSC; with 0x70.3 = 0 that is 24.576 MHz /1) while USB is active. After 0x42/0x00 changes it waits `djnz` x 0x80 (`f_c_0ee1`, text 0x0ee1) / 0x50 (ROM) loops.
* `0x4E |= 0x08` (clear watchdog) is executed in the AP main loop and after every 2 KB chunk in the transfer loops. The ROM and giveio start by writing 0xA2/0x00 to port 0x4E.

---

## 3. MSC stack (USBDISK.AP bank 0x21)

### 3.1 Code map (v04)

| Where | What |
|---|---|
| bank20 (0x1600-0x1a51, 1106 B) | `UDiskInit` (b20:1600): stores callback, string descriptor ptr (0x11db), 4 B VID/PID (0x1138), 28 B vendor/product/rev (0x10fc); `UDiskInitInternal` (1x3d): clocks, 0x70, capacity, partition scan, LUN setup, CTC/IRQ install. `UDiskSetDiskNum/GetDiskInfo/SetWP/CheckWP` APIs. Contains an embedded DOS-style MBR boot stub used for signature compare (b20:1972, 0xE0 bytes). |
| **bank21** (0x1600-0x28f2, 4851 B) | **The MSC engine**: ISR, SETUP parser, CBW decoder, SCSI commands, sector loops, LBA computation, state machine (`UDiskHandle` b21:2257), "Actions" vendor password / hidden-disk protocol. |
| bank22 (0x1600-0x1fe3, 2532 B) | The ADFU engine **inside the same AP**: `ADFUHandle`, `ADFUISR_b22` (b22:1782), memory/flash commands, DMA1 copies. The standalone `ADFUF641.AP` (11264 B) contains the same code. |
| text (0x0900-0x1566) | glue: sector read/write wrappers (`RST 18h`), backup/restore of ZRAM2 (`BackupZram2Code`, text 0x0f20: copies 0x4000-0x4FFF through page 0xF6/0xF5, because stock OS drivers keep their bank-B code in ZRAM2), `UsbSuspendUsb` 0x0ffd, `UDiskExit` 0x0ee6, data/descriptors (0x1039-0x1249). |

SDK source (`usbdisk.c`/`usbdisk_2l.c`) shows the AP wrapper: `UDiskInitAll`: `DRV_DetectUD(0)`/`DRV_ActiveUD(0, MODE_FAST)`, read VM `comval`, `UDiskSetDiskNum(comval.OnlineMode)`, `UDiskInit(cb, setup, attr, vidpid)`, `GetSysInfo`, `UDiskRun`. Main loop: kick WD, `MsgManager` (PlayPause = "Menu" key exits, SD-out message), `UDiskHandle`, exit when status is 0x30/0x40/0x50/0x70.

### 3.2 Bulk-Only Transport

* CBW check at `bank21:16c2`: select ZRAM2 (0x05=0xF7), give both banks to the CPU, require signature `55 53 42 43` ("USBC" LE) at 0x4000; otherwise stall both bulk endpoints and clear all IRQs (`not_a_valid_CBW`, bank21:17c9). No check of dCBWFlags / bCBWCBLength (inferred: trusts the host).
* Saved fields: tag (0x4004-7 -> 0x28a9), dCBWDataTransferLength (0x4008-B -> 0x28c6), flags/LUN (0x400d -> 0x28ca), CDB opcode at 0x400f; read/write LBA and length from 0x4011..0x4017 (big-endian SCSI fields, byte-swapped in `Compute_LBA`, bank21:205a).
* Data phase: the data (INQUIRY etc.) is always copied from a fixed table to 0x4800 with **length = dCBWDataTransferLength** (`ldir bc=(0x28c6)`), so the response is padded/truncated to what the host asked for. No phase-error handling, no residue tracking in the success case (residue = 0).
* CSW (`f_b21_1f5c`, 0x28a5): `55 53 42 53`, tag, residue, status. Failure (`sFillErrCSW`, bank21:1f86): status 1 and residue = the whole CBW length. The next REQUEST SENSE returns the stored sense (below). Unsupported opcodes also get "dummy zero data of the requested length + CSW failed" (bank21:1782..178a). There is no phase-error (status 2) path.
* After the CSW (`bulk_out_back`, bank21:178d): `0x53 = 0x18` (ack EP1 OUT + EP2 IN), flash-LED callback if set, then: if the command class (0x28d7: 0 none / 1 read / 2 write) is the same as for the previous command, EP1 OUT is re-armed immediately inside the ISR (`sPrepare_EP1_Out`, bank21:21d7: `0x70` -> B1 to USB, EPI 3, `66=0x3F`, `64=0`, `65=0x40`); if the class changed, the ISR sets flag 0x28d4 and **masks the USB IRQ (`0x27 &= ~4`)**, and the AP main loop (`UDiskHandle` -> `UdiskReady`, bank21:21c2) performs the state change, re-arms EP1 OUT and unmasks. So **only one command is in flight at a time**; the CBW buffer is re-armed only after the CSW was sent.

### 3.3 SCSI commands implemented (dispatch bank21:1727-1782)

| Opcode | Command | Handling |
|---|---|---|
| 0x00 | TEST UNIT READY | OK, except after a "card/password change" flag (0x2800) -> UNIT ATTENTION (sense 3) once. |
| 0x03 | REQUEST SENSE | 18 bytes from one of 5 canned records (0x2847 + n*0x12): 0 = none, 1 = ABORTED COMMAND (0x0B / ASC 0x4E), 2 = **DATA PROTECT (0x07 / ASC 0x27)**, 3 = UNIT ATTENTION (0x06 / ASC 0x28), 4 = NOT READY (0x02 / ASC 0x3A medium not present). Index in 0x28d3, cleared after use. |
| 0x12 | INQUIRY | 36 bytes at 0x10f4: PDT 0, **RMB = 0x80 or 0** (0x80 = removable unless the card path says otherwise), version 0, format 1, additional length 0, then 28 bytes vendor(8)/product(16)/rev(4) taken from the firmware header ("GENERIC", "USB DISK DEVICE", "1.00"). |
| 0x1A / 0x5A | MODE SENSE(6/10) | Canned reply; **write-protect bit (0x80 in the device-specific byte at 0x2806/0x2816)** = flag 0x122d (LUN0) / 0x122e (card). Page 0x1C -> short reply (0x2803), 0x3F -> long reply (0x2813). |
| 0x55 | MODE SELECT(10) | Receives the data (0x68 handshake) and ignores it. |
| 0x1B | START STOP UNIT | Byte 0x4013 == 2 (**eject**): clears 0x22.7 (CTC off) and sets 0x28df -> AP state 0x30 -> exit. Start/stop otherwise OK, no action. |
| 0x1E | PREVENT/ALLOW MEDIUM REMOVAL | Allow (bit0 = 0) -> flag 0x28d9 = 1: shortens the idle-flush timer (see 3.6). Prevent -> 0x28d9 = 0. |
| 0x23 | READ FORMAT CAPACITIES | canned reply 0x1182/0x1196/0x11aa/0x11be (capacity patched in at init). |
| 0x25 | READ CAPACITY(10) | 8 bytes at 0x1162.. (last LBA / count (inferred), block size 0x00000200). Disk A / B / C / card variants at 0x1162, 0x116a, 0x1172, 0x117a. |
| 0x28 | READ(10) | `f_b21_1bbc`; see 2.3. |
| 0x2A | WRITE(10) | `CMD_Write10` (bank21:1c73). If LUN is write-protected (flag 0x122d, `UDiskSetWP('C')`) the command still receives the data, then reports CSW failed with sense 2 (DATA PROTECT) (`lsSendCSWInWriteFlash`, bank21:1cfa-1d2d). |
| 0x2F | VERIFY(10) | OK, no data. |
| 0xCB | **vendor: switch to ADFU** | see section 4. |
| 0xCC | **vendor: query** | returns the 11-byte string `"ACTIONSUSBD"` (0x28e0) and resets 0x28f2/0x11da/0x28f1 as described in `l_b21_1df1`. Used by s1res (`AdfuDeviceIoctl::preopen`) to recognise Actions players. |
| other | -> CSW failed + sense 1, zero-filled data | |

Not implemented: READ(6)/WRITE(6), SYNCHRONIZE CACHE, READ TOC, READ(12), LOG SENSE, REPORT LUNS, SEND DIAGNOSTIC.

### 3.4 LUNs and mapping to the NAND logical sectors

(fact, from `Compute_LBA` bank21:205a, `UDiskInitInternal`, `f_b20_1691`, `f_b20_172a`, `f_b20_1838`)

* The CBW LUN (0x28ca) is remapped by the "disk number" mode (0x1237 = comval.OnlineMode 0..3, `bank21:170b-1726`).
* **LUN 0 = internal NAND user disk.** Every host LBA is translated **UD sector = LBA + 0x200** (`l_b21_20b5`: `hl += 0x0200` with carry into the high word). The two sector-count tables are filled with `UD_GetCap` (RST 10h API 0x0082, in 128 KB units?) as `capacity_sectors = 256*units - 0x8000 - 0x210` (`f_b20_16e4`; the first term converts units to sectors, 0x8000 sectors = 16 MB = the firmware area, 0x210 = 0x200 VM sectors + 0x10 (reserve, inferred)).
* So the **VM settings area (UD sectors 0..511) and the firmware/system disk (separate `SD_*` driver) are hidden from the host.** Matches atj2085emu README "Storage model" (logical sectors 0..511 = VM, user FAT volume at 0x200).
* A second LUN is created from (a) an "Actions" hidden/encrypted partition ("disk B/C") described by an MBR-like sector in the **VM area, UD sector 0x1FF** (flag 0x4820 = 1 and a boot-code compare `sActionsMBRCheck`; partition start/size at +0x80/0x90/0xA0) — LUN 1 LBA is then offset by 0x11d2/0x11d6 — or (b) an SD/MMC card (`DRV_DetectUD(E=1)`, `UD_SDMMC`, ReadInit/WriteInit streaming, `f_c_0e71`). GET_MAX_LUN = (disk B present | card present). On the LISONG board neither exists (no card slot; emu README says the SDMMC driver is dead code on the 2085X) -> **one LUN**.
* Host LBA 0/1 of LUN0 (UD sectors 0x200/0x201) are specially handled (`Compute_LBA` sets flags 0x28d5/0x28d6; `sEncrypParseInRD/WR`, bank21:2458..2792): reads can be answered from a saved copy, writes with signature `"ActUSBC\0"` (0x27e0) and command byte at offset 7 are interpreted: 0x05 query, 0x08 set password, 0x15 start hidden-disk write window, 0x18 define disk B/C, 0x20 **eject/exit** (sets 0x2802 -> state 0x60), 0x80 login, 0xA0 **switch to "Linux OS" mode** (sets 0x2801 = 1 -> state 0x51 = switch to ADFU). Replies are "ActUSBS" (0x27e8). This is the Actions encrypted-disk / online-device protocol. zos does not need any of it.
* Sector backend contract (RST 18h, DE -> 8-byte block at 0x11dd): `{u32 sector, u8 flag, u16 buffer ptr, u8 page (7 = ZRAM2)}`; A = 0 SectorRead, 1 SectorWrite, 4 **Update (flush)**, 5 WriteInit, 6 ReadInit (card streaming only). One sector (512 B) at a time; the NAND FTL/cache is inside the UD driver (atj2085emu README: driver caches a sector at 0x3A80).
* Active drive selection: `DRV_ActiveUD(C, E)` (RST 10h HL=0x1300) is switched with port 0x05 = 0xF6 when changing between flash (E=0) and card (E=1).

### 3.5 Multiple LUNs / stock quirks worth knowing

* LUN mode 0x1237: 0 = multi, 1 = normal, 2..3 = encrypted variants; remap code increments the LUN once or twice.
* The ISR **runs the whole SCSI command, including the NAND reads/writes, inside the interrupt** (IM 1 accepts with interrupts disabled and the kernel handler only re-enables them on exit, so there is no nesting; (inferred) the 10 ms kernel tick is therefore starved during a transfer). The AP main loop only runs the state machine, timers and message queue.

### 3.6 Write caching / flush (state machine `UDiskHandle`, bank21:2257)

States in 0x11da (SDK comment in `usbdisk_2l.c`): 0 idle, 0x10 read active, 0x20 write active, 0x30 eject requested, 0x40 cable pulled, 0x50/0x51 switch to ADFU / upgrade, 0x60 "partition protocol" exit, 0x70 error, 0x80 suspend, 0x90 resume.

* Every READ/WRITE puts the machine in 0x10 / 0x20; each CBW resets idle counters 0x28da/0x28db.
* Idle timers use the RTC 2 Hz counter (port 0x43 bit3 latch, read 0x45; `CheckTimer`, bank21:23b7): **0x10 -> 0 after 2 ticks (about 1 s)**, **0x20 -> 0 after 0x3C ticks (about 30 s)** or after 2 ticks if PREVENT/ALLOW said "allow" (0x28d9). Going to state 0 executes **`UD_Update` (RST 18h A=4)** = flush the driver's write cache / FTL map (`sGoInState0`, bank21:2397).
* `UD_Update` is also issued at suspend (0x1249 >= 0xB4), at `UDiskExit`, and before the vendor "switch to ADFU".
* So: **write caching exists in the UD driver; the host is never told to flush** (no SYNCHRONIZE CACHE). A unplug right after a write window loses data.

---

## 4. ADFU / firmware upgrade and how to enter it

### 4.1 Firmware ADFU ("ADFU firmware", the mode s1res / s1flash / vendor updater use)

Sequence (fact, `AdfuDeviceIoctl.h` `preopen`/`open` + `CMD_SwitchToADFU` bank21:1dbc/1df1):

1. Player runs normally; plug USB. Kernel posts 0x91; MAIN starts `USBDISK.AP`; the PC sees **10D6:1100 mass storage "GENERIC USB DISK DEVICE"** and assigns a drive letter. (The user has been doing this implicitly.)
2. Host sends SCSI CDB `CC 00 00 ...` (read 11 bytes). Reply must be `"ACTIONSUSBD"` -> tool knows it is an Actions device. (`AdfuDeviceIoctl::preopen` also checks the drive geometry media type 0x0B.)
3. Host repeats CDB `CB 00 00 ...` (read 1 byte) once per second up to 10 times. The device answers 0xFF when it accepts the switch (`CMD_SwitchToADFU`: refuses with 0x00 if the LUN is write-protected or busy, `0x122d`/`0x11da`/`0x28f2`); on accept it saves the MBR (`f_b21_2418`: reads UD sector 0x200, `BackupMBR2F5`) and sets **0x2801 = 0xFF**.
4. `UDiskHandle` sees 0x2801: state **0x51**, USB IRQ masked, AP exits with RESULT_UPGRADE (SDK comment "0x50: switch to firmware upgrade AP"). `UDiskExit` leaves the D+ pull-up on, so the host sees **no disconnect/re-enumeration**.
5. `MAIN.AP` (bank 0x40, 0x1b21-0x1b48) runs **`adfuf644.ap` / `adfuf321.ap` / `adfuf641.ap`** by flash family (names at MAIN data 0x0aa0/0x0aac/0x0ab8). The ADFU AP re-installs its own USB ISR (`ADFUISR_c`) and uses the **same MSC endpoints and BOT framing, with ADFU commands in the CDB**, exactly as `s1flash.py` does through the drive letter (`ioctl` backend), or with libusb after a re-enumeration with PID FF51 (see 4.2).
6. Session end: either CDB 0x20 with state code 0x91 (download end; the AP exits after ~3 idle ticks and the watchdog resets the player) or CDB 0x16 (flag 0x1e2a = 0x77, AP exits; (inferred) the OS then restarts).

ADFU CDB opcodes implemented by the device (`EP1_OUT_ISR_b22_17dc` bank22:17dc; ADFUF641.AP identical):

| CDB[0] | Meaning (device side) | Host side (`AdfuSession`, s1flash.py) |
|---|---|---|
| 0x00 | no-op, CSW OK | |
| 0x05 | **memory read/write**; CDB[1] bit7 = read; CDB[2:3] address; CDB[6] memory select (0..7 IPM/IDM/ZRAM2 lane, 8 = ZRAM1); CDB[7:8] length. Moves up to 2 KB per round via ZRAM2 and **DMA1** | `upload/download` |
| 0x08 | **system-disk (firmware area) sector read/write**: CDB[2:5] = 32-bit sector, CDB[7:8] = sector count; one `SD_*` driver call (RST 18h) per sector | `read_fw_block`, `write_fw` |
| 0x09 | boot record (BREC) read (`UD_WordRead/DwordRead` path, inferred) | `read_brec` |
| 0x10 | **BROM recovery ADFU: call address** (CDB[1:2] -> ROM var 0x010e, executed from the main loop after the CSW). In the *firmware* ADFU 0x10 only stores the 16-bit value in 0x1e38 and sets flag 0x1e2b (b22:19fe). | `exec` (recovery) |
| 0x20 | **firmware ADFU: call address / state code**; address in CDB[1:2] (`hl = (0x4010)`). If the high byte (CDB[2]) is < 0x90 it is stored as a call target (0x1e3b) and executed by `ADFUHandle`; >= 0x90 are magic *state codes* (b22:1ad6): 0x90 -> state 0x20, 0x91 / 0x93 / 0x94 -> "download end" / "upload end" (flag 0x1e57 = 1 / 2, state 0x40), 0x92 -> state 0x30, 0x95 -> 0x1e41 = 1 and state 0x10 (final step: writes 0x80 sectors of 0xFF at system-disk sector 0x4000 (b22:16a1), purpose unknown), >= 0x96 -> state 0. After download-end plus 2-3 idle ticks `ADFUHandle` calls `ADFUExit` and then spins in `jr $` (b22:1673-1676) = **the device is reset by the watchdog**. s1flash sends 0x0096 first ("begin") and 0x0091 last ("end"). | `exec`, `write_fw` framing |
| 0x16 | reset: flag 0x1e2a = 0x77 -> state 0x60 -> AP exits (BROM version: `jp 0x0924`, same flag in ROM 0x0100) | `reset` |
| 0x18 | return 2 status bytes (0x1e3d) | |
| 0x1A | get status/RAM bytes | |
| 0x25 | READ CAPACITY | |
| 0x28 | read back the saved MBR | |
| 0x03 | REQUEST SENSE | |

Recovery detection by the PC: read 16 bytes at ZRAM 0x0800 (`download(0x0800)`); if they are not `"SYS INFO"` the device is in **recovery** (ROM) mode (`AdfuSession::detectRecoveryMode`). In recovery the exec command is 0x10, in firmware mode 0x20.

### 4.2 ROM recovery ADFU (BROM) — what it is, how it starts

BROM layout (8 KB at 0x8000, mirrored in the dump; header `C3 30 80 | 30 00 | 51 0C | 20 06 04 13 | D6 10 51 FF | "Actos@Actions" | "Gongee.Zhang"`; ROM date 2006-04-13):

* **0x8030** reset code (fact): `out 0x4E,0xA2` (watchdog), `out 0x04,0x01`, `di; im 1`, `out 0x27,0`, `out 0x42,0x80` (HOSC only), `0x70 |= 0x30`, `0x05 = 0xF7`, `sp = 0x4F00`, `out 0x00,0x10`, **copy 0xC00 bytes ROM 0x8100 -> ZRAM2 0x4000**, `call 0x4013` (NAND boot loader). If it returns 0: `jp 0x0000` (BREC now in ZRAM1). If it returns non-zero: **copy 0xC00 bytes ROM 0x9000 -> ZRAM1 0x0000, `sp=0x100`, `jp 0x0000`** = the recovery ADFU.
* **NAND boot loader** (ROM 0x8100, running at 0x4000): reset + read status on CE1 (`out 0x28/0x29` CE select, `out 0x2A` command: 0xFF reset, 0x70 status, 0x90/0x91 read ID, 0x37 stop), identifies the chip from ID byte 0 (0x98 Toshiba, 0xEC Samsung, 0x07, 0x20...), reads **16 KB BREC** from the first blocks (32 x 512-byte sectors of data, pages 0-7, by DMA2 ports 0x14-0x21; **no spare, no ECC check** - corrected 2026-11, see zos/NAND_AND_BOOT.md 5.1), blocks 0, 1, 2, 3 in turn, and verifies it with `0x436e`: **word at BREC+0x3FFC == 0x55AA and word at +0x3FFE == 16-bit sum of the words below 0x3FFE** (checked: `BRECF641.BIN` has 0x55AA at 0x3FFC and sum 0x5B00 = stored). Chip not answering at all twice -> tries an external ROM on CE1 (signature `55 AA 0F F0` at 0x8002..0x8005, `jp 0x8000`), otherwise returns "fail".
* **Recovery ADFU** (ROM 0x9000, entry `jp 0x0200`): waits in a loop until **VBUS** (0x50.6) is present, enables the 48 MHz clock, sets the IRQ vector 0x0056 -> ISR 0x02db, enables USB IRQ and the pull-up, then idles; descriptors are **VID 10D6, PID FF51, class FF/FF/FF** (same layout as the MSC one but vendor class; ROM 0x0ac8), bulk 0x01 OUT / 0x82 IN. Commands: 0x05 memory read/write (via DMA1), **0x10 jump to address**, 0x16 reset. That is enough to upload `ADFUS.BIN` (the "ADFU server", 9216 B in the AFI, type 'A') or `giveio` and run it; the vendor updater / s1clone use it to write BREC and the NAND.

**How to force it (answer to "how do I get into USB upgrade mode"):**

* From a working firmware: just use the MSC route (section 4.1). No key combination exists for it; the vendor tool does exactly this over the drive letter. s1flash.py in "ADFU firmware" mode needs libusb to open the device, which only works **after** it has been re-bound; since the firmware ADFU keeps the same MSC enumeration (10D6:1100, class 8) Windows keeps the mass-storage driver, so use the ioctl/SCSI pass-through route (as s1res does) — see the message in `s1flash.py open_session`.
* **ROM recovery (PID FF51) has no software or key trigger.** The BROM (fully disassembled for the reset path and the loader) reads **no GPIO and no key**; BREC reads no key either (BRECF641.BIN scanned: only 0xF3/F4/EE GPIO, 0xD0 and one 0x50 (VBUS) use inside its NAND-write guard at 0x097e). It is entered **only if the BREC cannot be loaded and verified from NAND CE1**. Practical ways: an erased/corrupt BREC area (what a broken flash looks like; s1clone's "recovery state"), or disturbing the NAND bus at power-up (hardware: keep CE1/R/B or a data line from working until the BROM has given up (about 4 failed block reads), then release it; pin-level details are not in any document here). The ICEEN-/ICERST- pins and the datasheet have no boot-mode strap.
* Warning for zos: **never touch the BREC blocks or the first blocks of NAND CE1.** With them intact the device always boots; if they are damaged, ROM recovery + s1clone is the only way back (this is why the constraint "no destructive flash" matters).

### 4.3 How zos could be loaded without the normal updater (for testing)

Recovery ADFU command 0x10 (jump) after 0x05 uploads is how `giveio` (v3.1, `giveio.asm`, 0x3400..0x37FF) takes over: it disables the watchdog (`out 0x4E,0`), `di; im 1`, `0x27=0`, own stack, and then runs a **polled BOT loop** over ZRAM2 with direct port access (see `usb_rx/usb_tx` in giveio.asm) — **a working minimal, interrupt-free, PIO-free bulk transport for this chip** that zos can use as the reference for its first MSC bring-up (it relies on the ROM/ADFU having already enumerated; zos has to do enumeration itself, see section 5).

---

## 5. Design note: minimal zos MSC (read-only first)

### 5.1 What zos needs

1. **Hardware bring-up** (all values from the stock code): kick/disable WD (0x4E); `0x42 |= 0xE0`, short delay (`djnz` 0x80 x), `0x00 = 0x10`; `0x05 = 0xF7`, `0x70 = (0x70 & 0xCF) | 0x10`; `0x52 = 0x53 = 0x69 = 0x6A = 0xFF`; leave `0x27.2` clear if polling (or install the 0x0038 handler on `0x26.2`); `0x3E |= 0x80` (connect) only after VBUS (0x50.6) is seen. On disconnect: `0x3E &= 0x7F`, `0x42 = 0x80`.
2. **Enumeration**: on 0x52.7: `0x51 = 0x80`, clear all BR (`for n in 0..7: 0x60=n; 0x64=0` until 0x68 == 0), ack. On 0x52.5: read 0x57 (== 0x58 after masking 0xDF) and the 8 bytes at 0x58..0x5F. Serve the requests in 2.2 (about 10 handlers); descriptors are ~110 bytes of const data (use VID/PID of your own; stock VID 10D6 is Actions' — do not reuse for a distributed product).
3. **Bulk endpoints**: after SET_CONFIGURATION(1): `0x70 = (0x70&0xCF)|0x10`, EP1 OUT (`60=3, 61=0, 66=0xBF, 62=63=0, 64=0, 65=0x40`), EP2 IN (`60=4, 61=0, 66=0xBF, 64=0`). Poll `0x68.3` for a CBW.
4. **BOT + SCSI**: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), READ(10), MODE SENSE(6) (return WP = 1), PREVENT/ALLOW, START STOP UNIT, READ FORMAT CAPACITIES (0x23), MODE SENSE(10)/VERIFY as no-ops, unsupported -> CSW failed + sense ILLEGAL REQUEST. 13-byte CSW at 0x4800 with `55 53 42 53`.
5. **Sector backend**: a function `read_sector(lba, buf)` supplied by zos's NAND/FTL driver (UD-compatible: logical sector = host LBA + (VM area if zos keeps it)). For a first version the **stock UD_SF641 driver via RST 18h** is not available to a fresh OS, so the backend will be zos's own NAND read (+ FTL logical-to-physical map) or, as an interim, a RAM/ROM image. Read-only means no FTL write path, no erase, no flush, **no risk to NAND**.
6. **Read-only first**: report WP in MODE SENSE, answer WRITE(10) with DATA PROTECT (sense 07/27) after draining the data — exactly as the stock does when `0x122d` is set (`bank21:1cfa`).

### 5.2 Estimated size

* Stock MSC engine in `USBDISK.AP`: text 2335 B code + bank20 882 + bank21 4555 + bank22 1757 = ~9.5 KB of code (14.8 KB AP file) because it contains encrypted-disk protocol, second LUN, card, ADFU, suspend and display hooks.
* Minimal zos (single LUN, no vendor protocol, polling, read-only): EP0 + descriptors ~0.9 KB, BOT/CSW ~0.3 KB, SCSI ~0.7 KB + tables 0.2 KB, read loop ~0.25 KB: **about 2.3 - 2.8 KB**, plus the NAND read/FTL it needs anyway. Write support: +0.4 KB (WRITE(10) with 2 KB ping-pong) plus the FTL write/flush path (the large part).
* RAM: ZRAM2 4 KB (fixed), ~40 bytes of state (cfg, halt flags, tag, LBA, count, sense index).

### 5.3 Pitfalls / timing

* **ZRAM2 is the only DMA-capable USB memory** and the stock OS keeps driver code in it (UI driver bank B at 0x4800, backed up by `BackupZram2Code`). zos must treat 0x4000-0x4FFF as USB-owned while connected, and the CPU can only see the bank(s) it owns (port 0x05 must stay 0xF7; if 0x70 bits 5:4 = 00 the MCU cannot access ZRAM2 at all).
* **Buffer = 2 KB per bank.** The most efficient loop is the stock one: read 4 sectors into the CPU bank while USB sends the other. The Z80 `ldir` of 512 B costs about 11 k T-states (~0.45 ms at 24.576 MHz); the FS-bus transfer of 2 KB is about 2 ms, so the CPU can keep up if the NAND page read is below ~1.5 ms. The datasheet quotes 1033 KB/s read / 955 KB/s write for the stock firmware.
* **Which DMA**: the USB SIE has its own; **do not use DMA1/DMA2 on ZRAM2 while the SIE owns the bank** and remember DMA1/DMA2 halt the Z80 (not the USB DMA). The UD/FTL layer should use DMA1 for the 2 KB NAND page, which stalls the CPU but not the SIE.
* **BR handshake**: the order is `0x62/0x63`, then `0x64`, **then 0x65 last** (0x65 arms the endpoint, 0x64 alone clears BR). To *finish* an IN transfer wait for 0x68 bit to go 1 -> 0 (arming has latency: the code first waits for 1). Forgetting to ack `0x53` leaves the IRQ pending and the next wait returns immediately.
* **Bus reset / unplug inside a transfer**: every wait must also test 0x52.7 and 0x50.6 (the stock code abandons the whole command via `sISREnd`/`Connect_ISR`). If zos polls from the main loop this is a simple "abort command" path.
* **EP0 is 8 bytes max**: send descriptors in 8-byte packets (hardware splits from the count), force DATA1 on the first IN packet (`66 = 0xC7`), SET_ADDRESS only takes effect after the status stage; the host expects the 18-byte device descriptor in time (do not run long NAND work between SETUP and the data stage; the stock ISR does everything in the ISR, so it is fast).
* **Watchdog**: default after the ROM is enabled (0xA2: about 1.4 s). Every long loop must do `0x4E |= 0x08` (the stock main loop and transfer loops do), or disable the watchdog like giveio (`0x4E = 0`) — in a production OS prefer to keep it and kick it.
* **Don't mount the FS while exported**: the stock AP has exclusive use of the UD driver (and FAT); after the cable is pulled MAIN re-initialises the FS. zos must do the same (flush, then re-read the FAT).
* **Write cache / flush** (when write is added): flush on unplug (0x50.6 or 0x52.6), on PREVENT-ALLOW(allow) after ~1 s, on 30 s idle, and on START STOP UNIT(eject); Windows will not send SYNCHRONIZE CACHE to this device model.
* **Host quirks the stock firmware already handles**: INQUIRY returns exactly the number of bytes the host asked for; READ CAPACITY and READ FORMAT CAPACITIES are both implemented (Windows 2000/XP ask for 0x23); `0xA1/0xFE` GET_MAX_LUN must be answered (stall is tolerated by most hosts, the stock returns a byte); MODE SENSE(6) page 0x1C and 0x3F are used by Windows.
* **Emulator**: `atj2085emu` has no USB host model (ports 0x50..0x6A are latches; README). To test MSC on the emulator, a host-side SIE model (SETUP queue + CBW feeder) would have to be written; the stock `USBDISK.AP` could serve as the regression oracle in the emulator.

---

## 6. Evidence index and open points

Evidence: `USBDISK.AP` bank21 (ISR 0x1600, SETUP parser 0x17f6, CBW decoder 0x16c2, READ 0x1bbc, WRITE 0x1c73, LBA 0x205a, state machine 0x2257, vendor protocol 0x24a2-0x2792), bank20 (init 0x1600-0x196a), text (0x0e07/0x0e32 drive activate, 0x0fc6 CTC, 0x0ffd suspend), ADFU bank22 (ISR 0x1782, DMA 0x1bd9), BROM `analysis/brom_8k.bin` (reset 0x8030, loader 0x8100->0x4000, recovery 0x9000->0x0000, ISR 0x02db), SYSCFG 0x2F38 (VBUS poll / 0x91, 0x92), MAIN.AP bank 0x40 0x1b21 (adfu AP selection), s1mp3 sources, datasheet 4.15.

Open points (not provable from the files here):

* Exact meaning of 0x61 bit1 (control-read mode value 0x03 vs 0x00) and whether SA 0x62/0x63 is relative to the USB-owned bank (inferred from usage).
* The `-0x210` term in the capacity computation (0x200 VM sectors + 0x10?) and whether `UD_GetCap` returns 128 KB units (inferred from the arithmetic).
* The meaning of `0x50` bit-pattern `0x30` guard in `UDiskRun`.
* What a hardware (pin) forced recovery should short; no schematic in the workspace marks it (`atj2085-mp4--schematic_compress.pdf` and the other schematics were not searched pixel by pixel; the ROM analysis shows the only requirement is "NAND CE1 boot fails").
* BROM NAND ID table (ID byte values 0x98, 0xEC, 0x07, 0x20 branches, 0x42c6/0x4274/0x430d) not fully analysed.
* `ADFUS.BIN` (9216 B "ADFU server", AFI type 'A') is not disassembled; its role is inferred from names and from the s1mp3 tools.
