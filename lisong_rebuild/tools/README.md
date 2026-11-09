# Firmware, NAND and helper tools

Python 3 tools for the ATJ2085 firmware containers and NAND dumps. They are used by `zos/build.py`,
the homebrew builder and the emulator scripts. Unless noted, they need only the standard library.
None of them writes to a device. Inputs are your own files (see the top-level README).

| Tool | What |
|---|---|
| `fwtool.py` | AFI (`.fw` as s1res reads/writes it) and the inner FWIMAGE.FW file system: `unpack` / `pack`, all checksums recomputed |
| `fwcheck.py` | print an AFI image's entries and verify its checksums |
| `aptool.py` | ACTOS `.AP` application container: `info` / `unpack` / `pack` (used to package `ZOS.AP` and homebrew apps) |
| `drvtool.py` | ACTOS `.DRV` driver container parser |
| `restool.py` | decode `.RES` resource files (images to PNG, strings) |
| `asm_util.py` | sdasz80/sdldz80 wrapper + Intel-HEX parser (**SDCC path hardcoded at the top**) |
| `profile.py` | firmware profile for the homebrew builder (`LISONG_PROFILE=v04` uses `../my-ximpro.fw`) |
| `s1clone.py` | reader for s1clone NAND dumps (header, boot ROM, per-page data + spare) |
| `ftlread.py` | reference implementation of the stock "641" FTL read path over a dump: `info`, `ls`, `tree`, `get`, `export-disk`, `export-fw`, `export-vm` (spec: `../analysis/nand_ftl.md`) |
| `nandinject.py` | add files/one-level folders to the dump's FAT16 user disk as an **emulator overlay** (`--nand-overlay`). The dump is only read |
| `fwinject.py` | put a firmware image into the dump's firmware area as an emulator overlay (copies A/B), to boot a modified `.fw` in the emulator |
| `mkfatimg.py` | build a raw FAT16/FAT32 image from a folder for `headless.py --disk` (needs `pyfatfs`, `fs`) |
| `z80dis.py` | Z80 disassembler (sdasz80 syntax, control-flow info); `test_z80dis.py` round-trips every opcode through sdasz80 |
| `emurun.py`, `fakefs.py` | older scripted emulator runner with a host-folder "fake FS" for the homebrew launcher. Expects a copy of `atj2085emu` in `../work/emu_snapshot` (copy it there first). New scripts use `atj2085emu/headless.py` or `zos/tools/emu_nand_zos.py` |
| `fwpatch/` | firmware patcher (work in progress) |

Examples:
```
python ftlread.py ../../readback-my-ximpro.s1clone tree 2
python fwtool.py unpack ../../my-ximpro.fw out_dir
python nandinject.py ../../readback-my-ximpro.s1clone zos.ovl ../../zos/out/atj2085/SHELL.ZM
```
