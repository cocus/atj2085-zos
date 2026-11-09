# zospatch - try zos on an ATJ2085 player without risking the stock firmware

`zospatch.py` takes a stock firmware file for an Actions ATJ2085 ("s1mp3") player, the `.fw`
you would flash with s1res, and writes a patched `.fw`. The patched firmware:

* **boots the unchanged stock OS by default.** All stock files stay in the firmware, byte for
  byte. The stock kernel is kept as `SYSCFG.STK` and started through the boot record's own
  loader, so the stock boot path is the same as before.
* **starts zos only if zos is installed on the player's USB disk.** That means `ZOS.AP` is in the
  root directory and is intact. zos itself (the core plus its `.ZM` modules, `ZOS.CFG`, skins) lives
  on the disk, so you install it by copying files over USB and **uninstall it by deleting
  `ZOS.AP`** using the stock firmware's USB disk mode. No key presses are needed.
* has escapes that need no zos at all:
  * **USB cable connected at power-on: stock.** This is on by default; turn it off with
    `--no-vbus-escape`. It means the stock USB disk and the stock firmware-update path
    (s1res / ADFU) can always be reached, even if zos misbehaves.
  * **MENU or VOL held at power-on: stock**, until the stock boot animation appears.
* falls back to stock whenever anything is wrong: `ZOS.AP` missing, truncated, garbage or
  fragmented beyond repair, an unsupported disk format, or a boot record it does not know.

Nothing in this tool writes to a device. Flash the output with s1res as usual (normal mode,
**not** GiveIO).

## Usage

```
python zospatch.py STOCK.fw -o PATCHED.fw            # patch (USB-cable escape ON)
python zospatch.py STOCK.fw -o PATCHED.fw --no-vbus-escape
python zospatch.py STOCK.fw -o PATCHED.fw --zos-file ZOSDEV.AP   # use another core file name
python zospatch.py --check FILE.fw [...]             # show what is inside, verify every checksum
python zospatch.py --unpatch PATCHED.fw -o STOCK.fw  # restore the original, byte for byte
python zospatch.py PATCHED.fw -o NEW.fw --repatch    # re-patch (e.g. with a newer stub)
```

Python 3 standard library only. `zosboot.bin` (the boot selector, our own code) must sit next
to the script; rebuild it from `stub/zosboot.s` with `python build_stub.py` (needs SDCC's
`sdasz80`/`sdldz80`).

Installing zos on the player (with the stock firmware's USB disk): copy `ZOS.AP`, the `*.ZM`
modules, the DSP programs (`MP3DEC.DSP`, `OGGDEC.DSP`, `PCMDEC.DSP`, `ADENCM.DSP`, taken from your
own firmware by `zos/build.py`), `ZOS.CFG` and the `SKIN` folder to the root of the disk, then unplug and
power on. To go back to stock without reflashing, delete `ZOS.AP` (or rename it). To go back to
the original firmware, flash the original `.fw` again (or the output of `--unpatch`).

## What it checks before writing anything

The tool refuses, with a message, when any of these fails:

1. **The container.** AFI magic; every AFI part's checksum and the AFI header sum (@0xFFC);
   FWIMAGE header `55 AA F0 0F`; header sum16 (@0x1FE); directory sum32 (@0x10); every file's
   32-bit word sum, as the boot record verifies them; no overlapping files. A firmware that
   fails its own checksums is not patched.
2. **The kernel.** `SYSCFG.SYS` exists, is 0x3800 bytes, shares no sectors with another file,
   and its vector page (file 0x3700) jumps into the 0x0600 load area.
3. **The boot record(s)** (`BREC*.BIN` in the AFI). Each one is identified by 16-bit sums over
   its code. Both are recorded on the stub's side too, and the stub re-checks them in RAM at
   every boot:
   * *loader* (the code that loads `SYSCFG.SYS` to 0x0600 and the vector page to 0x0000, and
     looks up the name with 11 `cp` instructions). This is what makes the stock fallback work.
     If **any** BREC in the file has an unknown loader, the tool refuses: on such a device
     stock could not be reached.
   * *flash reader* (sector read, address build and select routines of the "F641" boot record).
     At least one BREC must have it. Devices whose BREC has another reader (F644, F321) **always
     boot stock**. `--check` lists this per BREC.
4. **Space.** The FWIMAGE must stay below 8 MB (the boot record keeps copy B at 8 MB), and the
   directory needs a free slot.
5. **Not already patched.** Use `--unpatch`/`--repatch`. Old zkern native builds (they contain
   `SYSCFG.STK` without an undo record) are refused. Patch the stock file instead.

After patching, the tool re-parses its own output and checks: every checksum; every original
file unchanged at its original sector; `SYSCFG.STK` identical to the stock kernel; every other
AFI part unchanged (moved by the same offset if it lay after FWIMAGE); the stub parameters; and
that `--unpatch` gives back the input byte for byte.

## What the patch changes

* `SYSCFG.SYS` (same size, same sectors) is replaced by `zosboot.bin` plus parameters and an
  undo record (file 0x2D00, never loaded).
* `SYSCFG.STK` (a copy of the stock kernel) is appended after the last file. One directory entry
  is added. The FWIMAGE grows by 14 KB and later AFI parts (e.g. `SYSINFO.BIN`) move by the same
  amount.
* Checksums are recomputed: the two changed files, the directory, the header, the FWIMAGE part
  and the AFI header.
* Unchanged: BREC, BROM, ADFUS, FWSC and every other file.

The boot record also checksums `SYSCFG.STK` at boot (~0.02 s).

## How the boot selector decides (stub/zosboot.s)

The boot record loads the stub exactly as it would the stock kernel. The stub then:

1. checks the boot record's loader (sums plus the name test). If it is unknown, stock cannot be
   reached: the stub tries zos, and resets via the watchdog if that also fails. The patcher
   refuses firmware where this could happen.
2. sets the key GPIOs up the way the stock key driver does. **The keys can't be read before
   this step.**
3. USB cable present (option) -> stock. MENU or VOL held in at least 12 of 16 samples over
   ~25 ms -> stock.
4. checks the boot record's flash reader (sums, units per zone, geometry). Unknown -> stock.
5. reads the FAT16 boot sector of the user disk (user-disk sector 0x200). It needs the 55AA
   signature, 512-byte sectors, the "FAT16" type string and a power-of-two cluster size.
   Otherwise -> stock.
6. searches the root directory for `ZOS     AP` (skipping long-name entries, volume labels and
   directories). Not found -> stock.
7. reads the AP header: the ACTOS AP magic, a text segment at file 0x800 linked and entered at
   0x0800, 1..0x2400 bytes, and a file size that covers the text (otherwise the file is
   truncated). Then it follows the FAT16 cluster chain and loads the text straight to 0x0800.
   Any problem -> stock: a chain that ends early, a free or out-of-range cluster, an unmapped
   flash unit, a sector beyond the flash, or text that does not start with `jp`.
8. starts zos with the same port setup the stock OS leaves at its main menu (RTC timers,
   watchdog period, DC-DC, keys, I2C pins released), `SP = 0x0800`, `jp 0x0800`.

"Stock" means: patch the 3 extension letters the boot record's loader compares to `STK`, then
re-enter its loader. The stock kernel is then loaded and started by the same code as on an
unpatched player.

Sector reads use the boot record's own (checked) NAND address and transfer routines. The
logical-to-physical table lookup is done in the stub, for both FTL zones. The boot record's own
lookup only covers the firmware area (units < 0x400 of zone 0), but a full disk can place
`ZOS.AP` anywhere.

zos is loaded from the disk on every boot, so the core always matches the modules next to it.
Updating zos means copying newer files; no reflash is needed.

## Tested (emulator only)

`test/roundtrip.py` (offline) patches, verifies and unpatches every `.fw` found. `test/emu_matrix.py`
boots the patched firmware in atj2085emu with the real boot record and the user's NAND dump.
`test/synth.py` runs the stub's FAT/FTL logic against crafted disks (cluster sizes 1-64,
fragmented and backwards chains, zone 0 above unit 0x400, zone 1, beyond the flash, FAT12, a
missing signature, directory and deleted entries, sizes too big or too short, cluster 0).

| firmware | result |
|---|---|
| LSD_501_04 (my-ximpro), LSD_501_05 (with SYSINFO part after FWIMAGE), the 2007 Ximpro dumps, s1fwx UNNAMED, homebrew builds of these | patched; unpatch byte-identical; F641 = zos-capable |
| B002 t29_h66_j1, LSD_504_03 (BRECs F644 + F641 + F321) | patched; F641 devices zos-capable, F644/F321 devices always stock |
| 2rom22.bin | refused: its own FWIMAGE part checksum is wrong |
| 481C "moonlight treasure" (older generation, different boot record loader) | refused |
| XC_SN320.bin (no FWIMAGE part) / raw FWIMAGE.FW | refused (not a flashable AFI with FWIMAGE) |

## Limitations

* **Emulator-tested only.** The emulator does not model the key matrix before the GPIO setup,
  NAND timing, or watchdog resets. Before giving the firmware to others, test both escapes
  (MENU held, USB cable) on a real device, with `ZOS.AP` present.
* Only boot records with the known F641 loader and reader are supported for starting zos. Other
  flash families boot stock. Firmware with an unknown loader in any BREC is refused.
* The device's boot record is not written by s1res. If a player runs a BREC that is not the one in
  the patched `.fw`, the stub's runtime checks decide: an unknown reader means stock. An unknown
  loader means zos is tried, and if that fails the player resets in a loop. ROM recovery is then
  the way out. This cannot happen when the player was set up with firmware from the same family.
* The user disk must be FAT16 with its boot sector at user-disk sector 0x200 (the stock layout);
  change the location with `--volume-offset`. FAT12/FAT32 -> stock.
* `ZOS.AP` has no checksum of its own. Truncation, garbage, bad headers and broken chains are
  detected; a flipped bit inside the code is not.
* The zos core image must be at most 0x2400 bytes (it ends below 0x2C00, where the stub runs).
