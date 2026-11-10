"""Helpers to boot a newbrec image in atj2085emu through the BROM model (soc.py brom_load_brec).

Prerequisites that are NOT in the repository (untracked): my-ximpro.fw, readback-my-ximpro.s1clone and
the atj2085emu package with rom/. Set NEWBREC_PREREQ to the folder holding them (default: the s1mp3
checkout next to this repository, or the main working copy given in the environment).
"""
import os, sys, struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
PREREQ = os.environ.get('NEWBREC_PREREQ') or (
    REPO if os.path.exists(os.path.join(REPO, 'atj2085emu', 'rom')) else r'C:\Users\cocus\Downloads\s1mp3')
EMU = os.path.join(PREREQ, 'atj2085emu')
FW = os.path.join(PREREQ, 'my-ximpro.fw')
DUMP = os.path.join(PREREQ, 'readback-my-ximpro.s1clone')
sys.path.insert(0, EMU)
sys.path.insert(0, os.path.join(PREREQ, 'lisong_rebuild', 'tools'))

from emu.fwimage import FwImage
from emu.soc import ATJ2085
from emu.nand import ecc512
from emu import pngout


def brec_sum(b):
    return sum(struct.unpack_from('<8191H', b, 0)) & 0xFFFF


def make_overlay(brec, path, blocks=(0, 1), extra=()):
    """NAND overlay with `brec` (16384 bytes) in the given blocks, stock spare format per sector
    (FF x8, ECC x4, FF FF 55 FF), pages 8-63 erased: what the zos FIRMWARE module writes.
    extra = [(brec2, blocks2), ...]: more boot record copies (e.g. the stock one in blocks 2/3)."""
    ov, blks = {}, []
    for rec, bl in [(brec, blocks)] + list(extra):
        assert len(rec) == 0x4000
        for blk in bl:
            blks.append(blk)
            for p in range(64):
                page = bytearray(b'\xff' * 0x840)
                if p < 8:
                    page[:0x800] = rec[p * 0x800:(p + 1) * 0x800]
                    for g in range(4):
                        page[0x800 + 16 * g + 8:0x800 + 16 * g + 12] = ecc512(page[g * 512:(g + 1) * 512])
                        page[0x800 + 16 * g + 14] = 0x55
                ov[blk * 64 + p] = page
    with open(path, 'wb') as f:
        f.write(b'NANDOVL1')
        f.write(struct.pack('<I', len(blks)))
        for blk in blks:
            f.write(struct.pack('<I', blk))
        f.write(struct.pack('<I', len(ov)))
        for p in sorted(ov):
            f.write(struct.pack('<I', p) + bytes(ov[p]))


def stock_brec():
    """the stock boot record of the player (from the untracked my-ximpro.fw), via tools/brecpatch"""
    sys.path.insert(0, os.path.join(REPO, 'tools', 'brecpatch'))
    import brecpatch
    return bytes(brecpatch.load(FW)[0])


def boot(brec_path, overlay, log=lambda s: None, extra=()):
    """Boot the image from NAND blocks 0/1 (overlay) in the emulator. Returns the emulator."""
    brec = open(brec_path, 'rb').read()
    make_overlay(brec, overlay, extra=extra)
    emu = ATJ2085(FwImage(FW), log=log, nand=DUMP, nand_overlay=overlay,
                  nand_cache=os.path.join(EMU, 'work', 'nandcache'))
    return emu


def shot(emu, path):
    pngout.save_lcd(emu.lcd, path)
