"""Hardware-level model of the ATJ2085 NAND interface + the NAND chip(s) behind it.

Backing store: an s1clone dump (wiRe's s1clone container, see lisong_rebuild/tools/s1clone.py),
opened read-only. Everything the firmware programs or erases goes to an in-memory overlay
(optionally loaded from / saved to an overlay file); the dump is never written.

What is modelled (evidence: BREC 0x3D3A/0x3D5F/0x3D9A/0x3E30/0x1ACA/0x18F7, UD_SF641.DRV
0x37AF/0x37C9/0x39FB, Actions BREC sources 641Read.asm/641WordProgram.asm, wiRe FlashIO.cpp,
ATJ2085X programming guide sec. 8; full write-up in lisong_rebuild/analysis/nand_ftl.md):

  external window 0x8000-0xFFFF while port 0x02 bits 5-3 (EMA28:26) select CE1/2/4..7:
      port 0x01 = 1 -> every write is a CLE (command) cycle
      port 0x01 = 2 -> every write is an ALE (address) cycle
      port 0x01 = 0 -> data cycles (reads = RE#, writes = WE#); the address inside the window
                       is irrelevant (firmware uses 0x8000/0x8002/0x8123/0x81xx)
  port 0x2A write: hardware CMD sequencer = one CLE cycle with the value; 0x90 (read ID) also
      issues one ALE cycle (column register 0x2B); 0x37 = stop sequencer / release CE (no cycle)
  port 0x28: bit n = enable CEn (logged only), bit 0 read = sequencer busy (always 0 here)
  port 0x29: CE mode / flash type (latch)
  ports 0xCC-0xCF: hardware ECC. Writing 0xCC bit 4 starts it; it hashes the next 512 data
      bytes that cross the NAND data port (CPU or DMA), then clears bit 4 and presents the
      24-bit Hamming code in the same byte format the firmware stores in the spare area:
      CD = ECC[10:3], CC = ECC[2:0]<<5 | ECC[13:11], CF = ECC'[10:3],
      CE = ECC'[2:0]<<5 | 0x10 (block type 512 B) | ECC'[13:11]
  DMA1 with SA3/DA3 bit 7 = external: CE = (SA3 >> 2) & 7, bytes = data cycles (soc.py)

Chip (Samsung K9K8G08-class, ID EC D3 51 95, 2048+64 byte pages, 64 pages/block):
  FF reset, 70 status (0xC0 = ready, not protected; bit 0 = fail), 90 read ID,
  00 a a a a a 30 read page, 05 a a E0 random data out, 00 a.. 35 copy-back read,
  80 a.. d.. 10 program, 85 a.. d.. (10|11) random data in / copy-back program,
  81 (2nd plane) / 11 (dummy program) treated as normal program, 60 a a a D0 block erase.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'lisong_rebuild', 'tools'))
from s1clone import S1Clone  # noqa: E402

RAW = 0x840


def ecc512(data):
    """Hardware ECC bytes (CD, CC, CF, CE order = spare bytes +8..+11) for 512 data bytes.
    Same algorithm as wiRe's IFlashIO::calcECC (bit address XOR, LSB first)."""
    odd = 0
    par = 0
    for i, v in enumerate(data):
        if v:
            x = _XB[v]
            if _PAR[v]:
                odd ^= (i << 3) ^ x
                par ^= 1
            else:
                odd ^= x
    even = odd ^ (0xFFF if par else 0)
    return bytes(((odd >> 3) & 0xFF, ((odd & 7) << 5) | ((odd >> 11) & 7),
                  (even >> 3) & 0xFF, ((even & 7) << 5) | ((even >> 11) & 7) | 0x10))


_XB = [0] * 256
_PAR = [0] * 256
for _v in range(256):
    _x = 0
    _p = 0
    for _j in range(8):
        if _v >> _j & 1:
            _x ^= _j
            _p ^= 1
    _XB[_v] = _x
    _PAR[_v] = _p


class NandChip:
    """One NAND die: command/address/data state machine over a paged backing store."""

    def __init__(self, dump, log=None):
        self.dump = dump
        self.log = log or (lambda s: None)
        self.ppb = dump.block_pages
        self.pages = dump.pages
        self.id = dump.id_bytes + b'\x00'          # 5th ID byte not recorded by s1clone
        self.overlay = {}        # page -> bytearray(RAW) for programmed / erased pages
        self.erased_blocks = set()
        self.cache = {}
        self.trace = bool(os.environ.get('NANDLOG'))
        self.reset()
        self.stats = dict(reads=0, programs=0, erases=0, status=0, ids=0,
                          overprogram=0, nop_over=0, out_of_order=0)
        # rule checks (real NAND, not enforced by the model): a program must not touch bytes
        # that are already programmed (non-FF in the page and non-FF in the register), at most
        # NOP = 4 programs per page between erases, and in a block erased in this session pages
        # are programmed in ascending order. Violations are counted + logged ('NAND RULE').
        self.nop = {}            # page -> programs since the last erase (pages of erased blocks)
        self.top = {}            # block -> highest page programmed since its erase
        self.violations = []

    # ---- backing store
    def page_data(self, page):
        if page in self.overlay:
            return self.overlay[page]
        if page // self.ppb in self.erased_blocks:
            return b'\xff' * RAW
        d = self.cache.get(page)
        if d is None:
            if len(self.cache) > 512:
                self.cache.clear()
            d = self.dump.read_raw(page) if page < self.pages else b'\xff' * RAW
            self.cache[page] = d
        return d

    # ---- state machine
    def reset(self):
        self.mode = 'idle'       # idle | addr | data | status | id | prog
        self.cmd = None
        self.addr = []
        # one page register per plane (plane = block bit 0): the UD driver uses two-plane
        # copy-back (00 a 35 / 00 a' 35, then 85 a .. 11, 81 a' .. 10; UD_SF641 0x3924)
        self.regs = [bytearray(b'\xff' * RAW), bytearray(b'\xff' * RAW)]
        self.plane = 0
        self.reg = self.regs[0]
        self.col = 0
        self.row = 0
        self.fail = False
        self.idx = 0
        self.after_copyback = False
        self.erase_rows = []

    def _row(self, a):
        return a[0] | (a[1] << 8) | (a[2] << 16)

    def cle(self, c):
        a = self.addr
        if c == 0xFF:
            self.reset()
            self.mode = 'status'
        elif c == 0x70:
            self.prev_mode = self.mode
            self.mode = 'status'
            self.stats['status'] += 1
        elif c == 0x90:
            self.cmd = 0x90
            self.addr = []
            self.mode = 'id'
            self.idx = 0
            self.stats['ids'] += 1
        elif c in (0x00, 0x05, 0x60, 0x80, 0x81, 0x85):
            if c == 0x60:
                # two-plane erase = 60 row 60 row D0 (BREC 0x1910, UD_SF641 0x3785)
                if self.cmd == 0x60 and len(a) >= 3:
                    self.erase_rows.append(self._row(a[0:3]))
                elif self.cmd != 0x60:
                    self.erase_rows = []
            self.cmd = c
            self.addr = []
            self.mode = 'addr'
        elif c in (0x30, 0x35):           # read / copy-back read confirm
            if self.cmd != 0x00 or len(a) < 5:
                self.log('NAND: %02x after cmd %r addr %r' % (c, self.cmd, a))
            self.col = (a[0] | (a[1] << 8)) if len(a) >= 2 else 0
            self.row = self._row(a[2:5]) if len(a) >= 5 else 0
            self._select_plane()
            self.reg[:] = self.page_data(self.row)
            self.stats['reads'] += 1
            self.mode = 'data'
            self.after_copyback = (c == 0x35)
            self.cmd = c
        elif c == 0xE0:                   # random data output confirm
            if self.cmd == 0x05 and len(a) >= 2:
                self.col = a[0] | (a[1] << 8)
            self.mode = 'data'
            self.cmd = c
        elif c in (0x10, 0x11):           # program confirm / dummy program (first plane)
            if self.cmd in (0x80, 0x81, 0x85) or self.mode == 'prog':
                self._program()
            self.mode = 'status'
            self.cmd = c
            if c == 0x10:
                self.after_copyback = False
        elif c == 0xD0:
            if self.cmd == 0x60 and len(a) >= 3:
                for r in self.erase_rows + [self._row(a[0:3])]:
                    self._erase(r)
            self.erase_rows = []
            self.mode = 'status'
            self.cmd = c
        else:
            self.log('NAND: unsupported command %02x' % c)
            self.cmd = c

    def ale(self, v):
        if self.mode == 'id':
            return                         # 90 00
        self.addr.append(v)
        a = self.addr
        if self.cmd in (0x80, 0x81, 0x85):
            if self.cmd == 0x85 and len(a) == 2:
                self.col = a[0] | (a[1] << 8)
                self.mode = 'prog'
            if len(a) == 5:
                self.col = a[0] | (a[1] << 8)
                self.row = self._row(a[2:5])
                self._select_plane()
                # 80 = serial data input: clears the register; 81 too unless it is the second
                # plane of a copy-back program; 85 (copy-back / random data input) keeps it
                if self.cmd == 0x80 or (self.cmd == 0x81 and not self.after_copyback):
                    self.reg[:] = b'\xff' * RAW
                self.mode = 'prog'

    def _select_plane(self):
        self.plane = (self.row // self.ppb) & 1
        self.reg = self.regs[self.plane]

    def read(self):
        m = self.mode
        if m == 'data':
            v = self.reg[self.col] if self.col < RAW else 0xFF
            self.col += 1
            return v
        if m == 'status':
            return 0xC1 if self.fail else 0xC0
        if m == 'id':
            v = self.id[self.idx % len(self.id)]
            self.idx += 1
            return v
        return 0xFF

    def write(self, v):
        if self.mode == 'prog':
            if self.col < RAW:
                self.reg[self.col] = v
            self.col += 1

    def _program(self):
        p = self.row
        if p >= self.pages:
            self.fail = True
            return
        old = bytearray(self.page_data(p))
        clash = sum(1 for i in range(RAW) if self.reg[i] != 0xFF and old[i] != 0xFF)
        b, pg = p // self.ppb, p % self.ppb
        n = self.nop[p] = self.nop.get(p, 0) + 1
        if clash:
            self._rule('overprogram', 'page %06x (block %04x page %2d): %d bytes already programmed'
                       % (p, b, pg, clash))
        if n > 4:
            self._rule('nop_over', 'page %06x (block %04x page %2d): program #%d since erase' % (p, b, pg, n))
        if b in self.top and pg < self.top[b]:
            self._rule('out_of_order', 'page %06x (block %04x page %2d) after page %d' % (p, b, pg, self.top[b]))
        if b in self.top or b in self.erased_blocks:
            self.top[b] = max(self.top.get(b, -1), pg)
        for i in range(RAW):
            old[i] &= self.reg[i]        # NAND programming only clears bits
        self.overlay[p] = old
        self.fail = False
        self.stats['programs'] += 1
        if self.trace:
            self.log('NAND: program page %06x (block %04x page %2d) spare %s' % (
            p, p // self.ppb, p % self.ppb, bytes(old[0x800:0x810]).hex()))

    def _erase(self, row):
        b = row // self.ppb
        if b * self.ppb >= self.pages:
            self.fail = True
            return
        for p in range(b * self.ppb, (b + 1) * self.ppb):
            self.overlay.pop(p, None)
            self.cache.pop(p, None)
        self.erased_blocks.add(b)
        for q in range(b * self.ppb, (b + 1) * self.ppb):
            self.nop.pop(q, None)
        self.top[b] = -1
        self.fail = False
        self.stats['erases'] += 1
        self.log('NAND: erase block %04x' % b)

    def _rule(self, kind, text):
        self.stats[kind] += 1
        if len(self.violations) < 200:
            self.violations.append((kind, text))
        self.log('NAND RULE %s: %s' % (kind, text))

    # ---- overlay persistence (simple format: magic, then records)
    def save_overlay(self, path):
        with open(path, 'wb') as f:
            f.write(b'NANDOVL1')
            f.write(struct.pack('<I', len(self.erased_blocks)))
            for b in sorted(self.erased_blocks):
                f.write(struct.pack('<I', b))
            f.write(struct.pack('<I', len(self.overlay)))
            for p in sorted(self.overlay):
                f.write(struct.pack('<I', p) + bytes(self.overlay[p]))

    def load_overlay(self, path):
        d = open(path, 'rb').read()
        if d[:8] != b'NANDOVL1':
            raise ValueError('not a NAND overlay file')
        o = 8
        n, = struct.unpack_from('<I', d, o); o += 4
        for _ in range(n):
            self.erased_blocks.add(struct.unpack_from('<I', d, o)[0]); o += 4
        n, = struct.unpack_from('<I', d, o); o += 4
        for _ in range(n):
            p, = struct.unpack_from('<I', d, o); o += 4
            self.overlay[p] = bytearray(d[o:o + RAW]); o += RAW


class NandController:
    """Port-level glue between the SoC model and the chips (one chip per CE)."""

    def __init__(self, soc, dump_path, ce=1, cache_dir=None, log=None):
        self.soc = soc
        self.log = log or soc.log
        self.dump = S1Clone(dump_path, cache_dir)
        self.chips = {ce: NandChip(self.dump, log=self.log)}
        self.ecc_left = 0
        self.ecc_buf = bytearray()
        self.absent_logged = set()
        self.trace = bool(os.environ.get('NANDLOG'))

    def chip_for(self, ce):
        return self.chips.get(ce)

    # ---- window (memory) cycles, called by soc while port 0x02 selects a NAND CE.
    # phase = EMA16:15 = port 0x01 bits 1:0 for CPU cycles, SA/DA bits 16:15 for DMA cycles.
    def mem_write(self, ce, v, ph=None):
        chip = self.chips.get(ce)
        if ph is None:
            ph = self.soc.ports[0x01]
        if chip is None:
            self._absent(ce)
            return
        if ph == 1:
            if self.trace:
                self.log('[%08d pc=%04x] NAND CE%d CLE %02x' % (self.soc.ticks, self.soc.m.pc, ce, v))
            chip.cle(v)
        elif ph == 2:
            chip.ale(v)
        elif ph == 0:
            chip.write(v)
            self._ecc_feed(v)

    def mem_read(self, ce, ph=None):
        chip = self.chips.get(ce)
        if chip is None:
            self._absent(ce)
            return 0xFF                  # floating bus: status reads "ready + fail" -> no flash
        if ph is None:
            ph = self.soc.ports[0x01]
        if ph != 0:
            return 0xFF
        v = chip.read()
        if chip.mode == 'data':
            self._ecc_feed(v)
        return v

    def _absent(self, ce):
        if ce not in self.absent_logged:
            self.absent_logged.add(ce)
            self.log('[%08d pc=%04x] NAND: access to CE%d (no chip)' % (self.soc.ticks, self.soc.m.pc, ce))

    def cur_ce(self):
        return (self.soc.ports[0x02] >> 3) & 7

    # ---- ports
    def port_out(self, p, v):
        if p == 0x2A:
            ce = self.cur_ce()
            chip = self.chips.get(ce)
            if self.trace:
                self.log('[%08d pc=%04x] NAND CE%d SEQ %02x' % (self.soc.ticks, self.soc.m.pc, ce, v))
            if v == 0x37 or chip is None:
                return
            chip.cle(v)
            if v == 0x90:
                chip.ale(self.soc.ports[0x2B])
        elif p == 0xCC:
            if v & 0x10:
                self.ecc_left = 512
                self.ecc_buf = bytearray()
                self.soc.ports[0xCC] = v

    def port_in(self, p):
        if p == 0x28:
            return self.soc.ports[0x28] & 0xFE     # bit 0: sequencer busy -> never
        return self.soc.ports[p]

    def _ecc_feed(self, v):
        if self.ecc_left:
            self.ecc_buf.append(v)
            self.ecc_left -= 1
            if not self.ecc_left:
                cd, cc, cf, ce = ecc512(self.ecc_buf)
                pr = self.soc.ports
                pr[0xCD], pr[0xCC], pr[0xCF], pr[0xCE] = cd, cc, cf, ce

    # ---- DMA (external side)
    def dma_read(self, addr, a3):
        """One external DMA read cycle: addr = SA[23:0], a3 = SA3 (bit 7 external, 4-0 SA[28:24])."""
        return self.mem_read((a3 >> 2) & 7, (addr >> 15) & 3)

    def dma_write(self, addr, a3, v):
        self.mem_write((a3 >> 2) & 7, v, (addr >> 15) & 3)

    def save_overlay(self, path):
        for ce, c in self.chips.items():
            c.save_overlay(path)

    def load_overlay(self, path):
        for ce, c in self.chips.items():
            c.load_overlay(path)

    def stats(self):
        return {ce: dict(c.stats, overlay_pages=len(c.overlay), erased_blocks=len(c.erased_blocks))
                for ce, c in self.chips.items()}
