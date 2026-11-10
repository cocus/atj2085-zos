# brecpatch - your own power-on screen for an ATJ2085 player

The first thing an ATJ2085 player shows is drawn by its **boot record** (BREC, 16 KB, NAND
blocks 0 and 1, `BRECF641.BIN` inside the `.fw`): a full-screen fill (stock: blue) and a 20 x 36
picture (stock: an hourglass), visible for a few seconds while the boot record mounts the NAND
and checks the firmware. `brecpatch.py` changes those two things and nothing else:

* the fill colour (default **black**),
* the picture: a built-in "ZOS" emblem (light blue on black, with a row of "busy" dots), or your
  own picture with `--image`.

![preview](preview.png)

The tool finds the paint routine by its instruction pattern (the fill loop and the 20 x 36 window +
`ldir` that streams the bitmap), checks the window geometry, the bitmap size and location and the
image checksum, and refuses anything it does not recognise. It recomputes the boot ROM's checksum
(16-bit sum of the words 0x0000-0x3FFC at 0x3FFE, `0x55AA` at 0x3FFC), verifies the result again
and writes a preview of the screen as the user will see it. No stock bytes are inside the tool; it
only patches the image you give it. Pure Python 3; Pillow only for `--image` with a PNG (a 20 x 36
24/32-bit BMP needs nothing).

## Usage

```
python brecpatch.py BREC.BIN --check [--preview now.png]   # what is there (nothing written)
python brecpatch.py BREC.BIN -o NEWBREC.BIN --preview new.png             # black + ZOS emblem
python brecpatch.py BREC.BIN -o NEWBREC.BIN --image logo.png --fill 000000
python brecpatch.py NEWBREC.BIN --restore-check BREC.BIN   # only fill/picture/sum may differ
python brecpatch.py my.fw -o NEWBREC.BIN --fw-out my-newbrec.fw          # also a .fw copy
```
Input can be a raw 16 KB BREC (e.g. `BREC.BIN` saved by zos), an AFI `.fw` (its `BREC*.BIN`
part) or an s1clone NAND dump (block 0; block 1 is compared). Pictures of another size are
fitted into 20 x 36 on the fill colour. The picture is stored rotated by 180 degrees, as the
stock one is (the glass is mounted upside down relative to the GRAM scan BREC sets up).

## Faster boot (default; `--stock-boot` turns it off, `--no-fast-boot` / `--no-fast-lcd` one part)

Times from power-on code start (BREC entry) to the OS (`SYSCFG.SYS`) start, emulator T-states,
seconds at 24.576 MHz (the boot clock; the emulator does not model NAND busy time):

| boot record | picture visible (backlight on) | OS starts | zos-native shell on screen |
|---|---|---|---|
| stock | 30.9 M = 1.26 s | 105.6 M = 4.30 s | ~106 M + zos init |
| fast boot only (`--no-fast-lcd`) | 30.9 M = 1.26 s | 52.6 M = 2.14 s | |
| default (both) | 23.7 M = 0.96 s | 45.3 M = 1.84 s | before 60 M = 2.4 s |

**Fast boot (firmware sums).** At every power-on BREC checks FWIMAGE copy A (the firmware): magic, header
16-bit sum, directory 32-bit sum, then the 32-bit sum of every file except fonts/pictures (1.6 MB,
about half of the boot time). Then it compares A's header with copy B's and copies A to B after a
flash, or B to A if A is bad. The fast boot keeps all of that except the per-file sums, and skips
those **only on a normal boot**. A run-time flag (in a few free bytes after the picture) switches
back to the stock full checks whenever something is off:
* A and B differ (first boot after s1res or a zos FIRMWARE update, or an interrupted update): A is
  verified **fully** before the stock "erase settings + copy A to B". If A's files are bad, the
  stock "verify B fully + copy B to A" runs instead. A half-written update is therefore still
  undone, as with the stock boot record.
* A's header or directory is bad: B is verified fully before it is copied over A (stock).

**What you lose:** if a file inside copy A gets corrupted while A's header and directory stay
intact and equal to B's (e.g. a NAND bit error the ECC cannot fix), the stock boot record notices
and heals A from B. The patched one boots the damaged file, which can crash or hang. Reflashing
with s1res fixes it if the stock OS still gets to its USB mode, otherwise ROM recovery.

**Fast LCD** sets the two waits after the picture is drawn and before the backlight goes on
(200 + 100 units of about 1 ms) to 1 + 1. They only delay the backlight; the LCD reset waits
(10, 10, 100) and the four 200-unit waits inside the controller init table (power-up steps) are
kept, because there is no evidence they can be shortened safely. This saves 7.3 M T = 0.30 s.

The zos FIRMWARE module accepts these boot records: the header and entry code it compares are
untouched. Its summary says "CODE DIFFERS!" because the firmware-check calls change.

## Installing it

**s1res / the normal firmware update does NOT write the boot record** (it only writes FWIMAGE
copy A), so `--fw-out` is only useful for tools that do (vendor recovery tools, s1clone).
The intended way is the zos **FIRMWARE** module:

1. `FIRMWARE > BREC BACKUP` saves the installed boot record to `BREC.BIN` (read back and
   compared; it also reports whether the copy in block 1 is identical). Copy it to the PC.
2. On the PC: `brecpatch.py BREC.BIN -o NEWBREC.BIN --preview p.png`, look at `p.png`, copy
   `NEWBREC.BIN` to the player's disk root.
3. `FIRMWARE > BREC UPDATE` checks `NEWBREC.BIN` (size, `0x55AA`, checksum, same boot-record
   family as the installed one: identical header and entry code), shows how many bytes differ
   and whether only the boot-screen area changes, and writes after `>>` is held for 2 s:
   block 1 first, read back (data, spare and ECC), then block 0, read back. If block 1 fails,
   block 0 is not touched. It makes `BREC.BIN` first if it is missing.
4. `FIRMWARE > BREC RESTORE` writes `BREC.BIN` back the same way.

Status: **emulator-verified only** (real dump, ROM-loader model, real boot record and stock OS
after the update; see `zos/NAND_AND_BOOT.md` section 5).

## If it goes wrong

A boot record that passes the checksum but does not run (no picture, hangs) leaves the player
dead until **ROM recovery**: the mask ROM only enters its USB recovery mode (ADFU, VID 10D6 PID
FF51) when none of NAND blocks 0-3 holds a boot record with a correct checksum. Recover by keeping
the NAND from answering at power-up (e.g. a switch in series with the NAND's CE# line, or holding
CE# inactive, until the ROM has given up, about four block reads), then release it and write the
full dump back with s1clone. Keep your `readback-*.s1clone` dump and `BREC.BIN`.
A patched boot record only changes data that the paint routine copies to the LCD, so the code
path is the stock one (emulator: identical port I/O and timing up to the OS start).
