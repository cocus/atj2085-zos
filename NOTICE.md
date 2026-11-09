# Notice and disclaimer

## Not affiliated

zos is an independent hobby project. It is not affiliated with, endorsed by or supported by Actions
Semiconductor, LISONG, Ximpro or any player manufacturer. Product and chip names are used only to
say which hardware this software targets.

## What is NOT in this repository

On purpose, this repository contains none of the following. You need your own copies, taken from
your own device (see README.md, "What you need"):

* stock firmware images (`.fw`/AFI, `FWIMAGE.FW`) or any file extracted from them (`.AP`, `.DRV`,
  `.SYS`, `.DSP` such as `MP3DEC.DSP`, `.RES`, fonts), and firmware images built from them
* NAND dumps (`.s1clone`), boot-ROM / TROM / BREC images, and the mask-ROM runtime library the
  emulator needs (`atj2085emu/rom/comlib.s28`, generated locally by `atj2085emu/tools/mkrom.py`
  from your own dump)
* the Actions SDKs or any code, header, library or listing copied from them
* disassemblies or rebuilt sources of the stock firmware
* vendor datasheets and programming guides
* third-party projects such as wiRe's s1mp3 tools (s1res, s1clone, s1fwx, ...); the tools here only
  read the s1clone dump format and the AFI/FWIMAGE container format

The research notes in `lisong_rebuild/analysis/` describe hardware behaviour and the stock
firmware's interfaces (port sequences, addresses, protocols) in our own words. They cite
documents and files from the author's work tree that are not distributed here.

`zos/build.py` copies `MP3DEC.DSP` out of **your** firmware image at build time. The built output
(`zos/out/`) therefore contains that stock file and must not be redistributed.

## USB identifiers

The zos USB modules (`zos/modules/usb/desc.c`, `zos/modules/usbaudio/desc.c`,
`zos/modules/fm/usbdesc.c`) use VID **0x1209** (pid.codes) with PIDs 0x0001-0x0004. These are
pid.codes **test PIDs**, for private testing only. They are not Actions' VID 0x10D6. Anyone who
distributes binaries or devices running zos must request their own PIDs from pid.codes (free for
open-source hardware/software) or use a VID/PID they own.

## Third-party material included

* `zos/skins/ilike/rockbox/`: the Rockbox theme **iLike** by Hayden Pearce ("St."), licensed
  **CC-BY-SA 3.0** (https://creativecommons.org/licenses/by-sa/3.0/). The files are unchanged.
  The zos skin generated from them (`SKIN/*.ZI`, `COLORS.ZT`, by `zos/tools/rbskin.py`) is a
  derivative under the same license. See `zos/skins/ilike/README.md`.

## No warranty, risk of bricking

Writing modified firmware to a player can make it unusable. Recovery may need the chip's
boot-ROM recovery mode and a full NAND backup. You use everything here at your own risk (see the
LICENSE and the safety section of README.md).
