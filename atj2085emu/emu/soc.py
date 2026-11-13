"""ATJ2085 system model around the `z80` package (Ivan Kosarev's C Z80 core).

See README.md for the memory/port map and the evidence behind each item.
"""
import collections
import struct
import sys
import os

import z80

from .lcd import Lcd
from . import rom
from .keys import KeyModel

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'lisong_rebuild', 'tools'))
try:
    import z80dis  # lisong_rebuild disassembler (read-only import)
except Exception:  # pragma: no cover
    z80dis = None

BP = z80.Z80Machine._BREAKPOINT_HIT
TL = z80.Z80Machine._TICKS_LIMIT_HIT
RMARK = z80.Z80Machine.READ_MARK
WMARK = z80.Z80Machine.WRITE_MARK

# Reset values (s1mp3_github/inc/s1reg.inc "def=" comments).
PORT_RESET = {0x2e: 0x01, 0x2f: 0x73, 0x3e: 0x04, 0x40: 0x04, 0x41: 0x55, 0x43: 0x04,
              0x4f: 0xec, 0x61: 0x02, 0x80: 0x03, 0x8b: 0x15, 0x91: 0x07, 0x9e: 0x40,
              0xd1: 0x04, 0xd3: 0x03, 0x9f: 0x5d, 0xd4: 0x05, 0xd6: 0x55, 0xde: 0x80,
              0xdf: 0x28, 0xee: 0x01, 0xf3: 0x07, 0xf4: 0x03}

PORT_NAMES = {
    0x00: 'MCU_CLKCTRL', 0x01: 'EM_PAGE_LO', 0x02: 'EM_PAGE_HI', 0x03: 'EM_PAGE_INC',
    0x04: 'MCU_A15CTRL', 0x05: 'MROM_SRAM_PAGE', 0x22: 'CTC1_PRESCALE', 0x23: 'CTC1_PER_LO',
    0x24: 'CTC1_PER_HI', 0x25: 'IRQ_STATUS', 0x26: 'MINT_STATUS', 0x27: 'MINT_ENABLE',
    0x28: 'NAND_ENABLE', 0x29: 'NAND_CEMODE', 0x2a: 'NAND_CMD', 0x2b: 'NAND_CA', 0x2c: 'NAND_RA',
    0x2d: 'NAND_BA1', 0x2e: 'DSP_STATUS', 0x2f: 'DSP_BOOTMODE', 0x3f: 'VOLTAGE_CTRL',
    0x40: 'HFC_CTRL', 0x41: 'PLL_PERF', 0x42: 'PLL_CTRL', 0x43: 'RTC_CTRL', 0x44: 'RTC_IRQSTAT',
    0x45: 'RTC_TIME_LO', 0x46: 'RTC_TIME_MI', 0x47: 'RTC_TIME_HI', 0x4b: 'LOSC_DIV_LO',
    0x4c: 'LOSC_DIV_MI', 0x4d: 'LOSC_DIV_HI', 0x4e: 'WATCHDOG', 0x4f: 'DCDC_CTRL',
    0x50: 'USB_STATUS', 0x6b: 'USB_IRQSTATUS', 0x70: 'MEMMAP/CLK', 0x7a: 'I2C_CTRL',
    0x7b: 'I2C_STATUS', 0x7c: 'I2C_DATA', 0x71: 'I2C_ADDR', 0x80: 'DAC_CTRL1', 0x9a: 'ADC_DATA',
    0x9d: 'UART_CTRL', 0xc0: 'KEYSCAN_DATA', 0xc1: 'KEYSCAN_CTRL', 0xd0: 'BATTERY_MON',
    0xd8: 'LRADC_DATA', 0xde: 'SARADC_IRQ', 0xdf: 'POWER_CTRL', 0xee: 'MFP/GPOA', 0xef: 'GPIOB_CFG',
    0xf0: 'GPIOB_OE', 0xf1: 'GPIOB_IE', 0xf2: 'GPIOB_DATA', 0xf3: 'GPIOC_EN', 0xf4: 'GPIOC_DATA',
    0xfe: 'GPIOG_EN', 0xff: 'GPIOG_DATA',
}

# Ports with a behavioural model or whose plain latch behaviour is known to be enough.
MODELLED = {0x43, 0x00, 0x01, 0x02, 0x04, 0x05, 0x25, 0x26, 0x27, 0x43, 0x44, 0x4e, 0x50, 0xd0,
            0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xfe, 0xff, 0xdf}
NAND_PORTS = {0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0xec, 0xcc, 0xcd, 0xce, 0xcf,
              0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13}

RST_NAMES = {0x08: 'RST08 fast', 0x10: 'RST10 bank', 0x18: 'RST18 stg', 0x20: 'RST20 disp',
             0x28: 'RST28 fs', 0x30: 'RST30 syserr'}

# Bit numbers of the emulated buttons inside the GPIO / key model (see keys.py)
VM_BASE = 0x1000000   # FW-read offsets >= this address the VM (settings) NAND area
CPU_HZ = 12_000_000     # GUESS: only used to pace the RTC tick; real clock is PLL-programmable


class ATJ2085:
    def __init__(self, fw, log=print, brec=None, trace_syscalls=True, disk=None, dsp=None,
                 nand=None, nand_overlay=None, nand_cache=None, tuner=None, rtc=None, rtc_counter=0):
        self.fw = fw
        self.log = log
        self.m = z80.Z80Machine()
        self.mem = self.m.memory
        self.ports = bytearray(256)
        for p, v in PORT_RESET.items():
            self.ports[p] = v
        self.ports[0x05] = 0xF7          # see self.p05 (BREC init, skipped, leaves 0xF7)
        self.ports[0xd0] = 0x07          # battery monitor: "full enough" (kernel low-bat at <4/5/6)
        self.lcd = Lcd(log=self._log_lcd)
        self.ticks = 0
        self.port_stats = collections.Counter()
        self.port_log = collections.deque(maxlen=4000)   # (tick, pc, dir, port, value)
        self.unknown_seen = collections.Counter()
        self.syscalls = collections.deque(maxlen=2000)
        self.syscall_stats = collections.Counter()
        self.fwreads = collections.deque(maxlen=500)
        self.trace_syscalls = trace_syscalls
        self.stopped_reason = None
        self.user_bps = set()
        self.bp_handlers = {}
        self.oneshot = set()
        # 0x4000..0x7FFF paging (port 0x05 low 3 bits) - see README (partly a guess)
        # 0x4000..0x7FFF window (port 0x05, ATJ2085X PG p.12-13): two 8 KB halves, each showing
        # either the default "ZRAM" view (ZRAM2 + whatever the firmware keeps there) or a byte
        # lane of DSP IPM/IDM owned by the MCU. Backing stores for the non-visible content:
        self.p05 = 0xF7             # stock runtime value measured on the device (lane 7 = ZRAM2)
        self.zram_win = bytearray(0x4000)                         # default view backing
        self.dsp_lanes = {l: bytearray(0x4000) for l in (0, 1, 2, 4, 5, 6)}  # IPM L/M/H, IDM L/M/H
        self.dma_log = collections.deque(maxlen=200)
        self.dsp = dsp               # DSP model (emu.dsp.DspHLE) or None
        self.dac = None              # optional MCU-fed DAC FIFO model (emu.dac.DacModel), see now()
        self._slice_n = 0
        self.lcd_writes = 0
        self.watch = set()           # ports whose OUTs are logged with the exact time in watch_log (tests)
        self.watch_log = []
        self.dsp_program = None
        # external memory window 0x8000..0xFFFF
        self.ext_lcd = False
        self.ext_nand = None        # CE number while port 0x02 selects a NAND chip enable
        self.nand = None            # emu.nand.NandController (hardware-level NAND, --nand)
        self.usb_cable = False      # set_usb(True/False) injects a USB plug/unplug
        self.keys = 0               # bitmask of pressed emulated buttons (see keys.py)
        self.key_model = KeyModel()
        # optional FM tuner on the GPIO C bit-banged I2C bus (emu/i2cbus.py): 'tea' or 'rda'
        self.i2c = None
        if tuner or rtc:       # rtc: DS3231 at 0x68 on the same bus (see i2cbus.py; 'none' tuner = DS3231 only)
            from .i2cbus import I2CBus
            self.i2c = I2CBus(self, tuner or 'none', log=log, rtc=rtc)
        self.rtc_next = 0
        self.rtc_fast_period = CPU_HZ // 100     # GUESS 10 ms
        self.rtc_slow_div = 50                  # GUESS: bit0 every 0.5 s
        self.rtc_cnt = 0
        # MCU RTC counter: 24 bit, 2 Hz (ports 45h-47h, read after the latch: port 43h bit 3; bit 4 clears).
        # rtc_cbase = the value at emulated time 0 (tests add to it to emulate time passing while the CPU
        # was stopped, e.g. standby); no counter overflow flag modelled
        self.rtc_cbase = rtc_counter
        self.rtc_latch = 0
        self.halted_ticks = 0

        m = self.m
        m.set_input_callback(self._in)
        m.set_output_callback(self._out)
        m.set_read_callback(self._ext_read)
        m.set_write_callback(self._ext_write)

        # ---- hardware-level NAND (optional): the real BREC init, FW reads and UD driver run
        if nand:
            from .nand import NandController
            self.nand = NandController(self, nand, cache_dir=nand_cache, log=log)
            if nand_overlay and os.path.exists(nand_overlay):
                self.nand.load_overlay(nand_overlay)
                log('NAND overlay loaded from %s' % nand_overlay)
            if brec is None:
                brec = self.brom_load_brec()
        # ---- load boot record and set up HLE hooks ----
        brom_fail = brec is False
        brec = None if brom_fail else (brec or fw.brec())
        if brec is None and not brom_fail:
            raise ValueError('need BREC (boot record) - give an AFI image')
        if brom_fail:
            # BROM found no valid BREC: it would copy its USB recovery code (ROM 0x9000) to RAM
            # and wait for the PC (ADFU PID FF51). Model: a `jr $` and a stop reason.
            m.set_memory_block(0, bytes(0x4000))
            m.set_memory_block(0x0200, b'\x18\xfe')
            self.brom_recovery = True
        else:
            m.set_memory_block(0, bytes(brec[:0x4000]))
            self.brom_recovery = False
        for a, d in rom.load_srec().items():       # mask-ROM C runtime library
            m.set_memory_block(a, d)
        self.rom_syms = rom.symbols()
        m.pc = 0x0200
        m.sp = 0x0200
        self.disk = disk            # emu.disk.UserDisk or None (user disk = UD logical sectors)
        self.ud_log = collections.deque(maxlen=2000)
        if self.nand is None:
            # HLE: BREC hardware/NAND init (call 0x1b12 from 0x0209). The NAND/FTL is not
            # emulated; the only thing later code needs from it is the FW-read entry 0x3FDB.
            self.add_hle(0x1b12, self._hle_brec_init, oneshot=True, name='BREC hw init (skipped)')
            # HLE: "read firmware bytes" (BREC 0x3FDB -> 0x3CAF, RST 08 with A<0x10 -> 0x3FDB)
            self.add_hle(0x3fdb, self._hle_fwread, name='FW read')
        else:
            # real code; only trace the FW-read entry
            self.add_hle(0x3fdb, self._trace_fwread, name='FW read (trace)', passthru=True)
            if os.environ.get('UDLOG'):
                self.add_hle(0x35e3, self._trace_ud, name='UD entry (trace)', passthru=True)
        self.vm = {}
        self.vm_dirty = False
        # HLE: interrupt nesting counter. SYSCFG's vector page sets RAM 0x0037 = 1 and
        # every kernel entry does di/inc ... dec/`ei if 0`, so nothing in the loaded
        # code ever brings it to 0 (=> interrupts would stay off forever). On the real
        # device the skipped BREC init presumably leaves this differently (UNVERIFIED).
        # We clear it when SYSCFG starts (0x0600), so the first API return executes EI.
        self.add_hle(0x0600, self._hle_irq_counter, oneshot=self.nand is None, name='irq counter fix')
        for v in RST_NAMES:
            if trace_syscalls or v == 0x10:      # 0x10 always: InitDSPCode hook
                self.add_hle(v, self._trace_rst, name=RST_NAMES[v], passthru=True)
        self.add_hle(0x18, self._rst18, name='RST18 storage')

    def brom_load_brec(self):
        """Model of the mask-ROM NAND boot loader (ROM 0x8100, runs at 0x4000; large-page path
        0x4213, sector read 0x439b, check 0x436e): for blocks 0, 1, 2, 3 (4 attempts) read the
        DATA columns of pages 0-7 (32 x 512 bytes by DMA2, no ECC, spare not read) to RAM
        0x0000-0x3FFF and accept the first block where word 0x3FFC == 0x55AA and word 0x3FFE ==
        16-bit sum of the LE words 0x0000..0x3FFC. Reads go through the chip model, so NAND
        overlays (BREC rewritten by zos) are seen. Returns the 16 KB or False (-> ROM recovery)."""
        chip = self.nand.chips[1]
        ppb = chip.ppb
        for blk in range(4):
            d = b''.join(bytes(chip.page_data(blk * ppb + p)[:0x800]) for p in range(8))
            w = struct.unpack('<8192H', d)
            ok = w[0x1FFE] == 0x55AA and sum(w[:0x1FFF]) & 0xFFFF == w[0x1FFF]
            self.log('BROM: block %d BREC %s (sum %04x stored %04x sig %04x)' % (
                blk, 'OK' if ok else 'rejected', sum(w[:0x1FFF]) & 0xFFFF, w[0x1FFF], w[0x1FFE]))
            if ok:
                self.brom_block = blk
                return d
        self.log('BROM: no valid BREC in blocks 0-3 -> ROM recovery ADFU (VID 10D6 PID FF51)')
        self.brom_block = None
        return False

    # ------------------------------------------------------------------ logging
    def _log_lcd(self, s):
        self.log('[%08d pc=%04x] %s' % (self.ticks, self.m.pc, s))

    # ------------------------------------------------------------------ HLE hooks
    def add_hle(self, addr, fn, oneshot=False, name='', passthru=False):
        self.bp_handlers[addr] = (fn, name, passthru)
        if oneshot:
            self.oneshot.add(addr)
        self.m.set_breakpoint(addr)

    def _ret(self):
        m = self.m
        sp = m.sp
        m.pc = self.mem[sp] | (self.mem[(sp + 1) & 0xffff] << 8)
        m.sp = (sp + 2) & 0xffff

    def _hle_irq_counter(self):
        if self.nand is not None:
            # with the real BREC init, 0x0600 is first BREC's own routine (called from 0x1B29);
            # act only once SYSCFG's vector page (RST 18 -> jp 0x32BE) is in place.
            if bytes(self.mem[0x18:0x1b]) != bytes([0xc3, 0xbe, 0x32]):
                return False
            self.log('[%08d] SYSCFG start: RAM 0x0037 (irq nesting) = %d, port05=%02x' % (
                self.ticks, self.mem[0x37], self.p05))
            self.oneshot.discard(0x0600)
            self.bp_handlers.pop(0x0600, None)
            self.m.clear_breakpoint(0x0600)
            self.mem[0x37] = 0
            return False
        self.mem[0x37] = 0
        return False

    def _trace_ud(self):
        """UDLOG=1 with --nand: log calls of the real UD_SF641 RST 18h entry (0x35E3)."""
        m, mem = self.m, self.mem
        if mem[0x18] == 0xc3 and (mem[0x19] | mem[0x1a] << 8) == 0x35e3:
            de = m.de
            sec = struct.unpack_from('<I', mem, de)[0]
            arg = mem[(de + 5) & 0xffff] | (mem[(de + 6) & 0xffff] << 8)
            self.log('[%08d] UD A=%d sec=%d arg=%04x sel=%02x (caller %04x) [NAND]' % (
                self.ticks, m.a, sec, arg, mem[(de + 7) & 0xffff], self.ret_addr()))
        return False

    def _trace_fwread(self):
        m, mem = self.m, self.mem
        de, n = m.de, m.bc
        off = struct.unpack_from('<I', mem, de)[0]
        dest = mem[de + 5] | (mem[de + 6] << 8)
        sel = mem[de + 7]
        if off >= VM_BASE:
            fname, foff = 'VM', off - VM_BASE
        else:
            fname, foff = self.fw.name_at(off)
        self.fwreads.append((self.ticks, self.ret_addr(), off, n, dest, sel, fname, foff))
        self.log('[%08d] FWREAD %-12s +%06x len=%04x -> %04x sel=%02x  (caller %04x) [NAND]' %
                 (self.ticks, fname, foff, n, dest, sel, self.ret_addr()))
        return False

    def _hle_ret(self):
        self._ret()
        return True

    def _hle_brec_init(self):
        """BREC 0x1B12 = hw init (0x3000), flash detect (0x150D), FTL/NAND init (0x0A36,
        0x2075, 0x0600). Skipped; we only reproduce the flash-detect results that the
        kernel's DRV_ActiveUD (SYSBNKB 0x4AB6) and UD_GetCap (UD_*.DRV bank A02) read:
          0x0080 = bitmask of chip enables with a NAND (bit n = CEn; BREC 0x1558)
          0x0081 = flash family of CE1 (BREC 0x1526): 0 -> *321, 1 -> *641, 2 -> *644
                   (SYSBNKB 0x4B85 switch -> UD_SF641.DRV / UD_FF641.DRV for 1)
          0x0088+2n = size of CEn in physical blocks (BREC 0x154C; UD_GetCap sums them)
        Only done when a user disk is attached, so the no-disk path stays as before."""
        if self.disk is not None:
            self.mem[0x80] = 0x02
            self.mem[0x81] = 0x01
            # UD logical block = 512 sectors = 2 physical blocks; UD_SF641 0x362C adds
            # 0x40 logical blocks (16 MB firmware area) before comparing with size/2.
            blocks = min(0xfffe, (self.disk.sectors // 512 + 0x40 + 1) * 2)
            struct.pack_into('<H', self.mem, 0x8a, blocks)
        self._ret()
        return True

    def _hle_fwread(self):
        """DE -> {u32 byte_offset; u8 ?; u16 dest; u8 page_sel}, BC = byte count.
        page_sel = port-0x05 page for dest in 0x4000..0x7FFF (SYSCFG:0x31EA stores
        `in a,(0x05) & 7` there); 8 = plain ZRAM (BREC, SYSCFG:0x0733)."""
        m = self.m
        de, n = m.de, m.bc
        mem = self.mem
        off = struct.unpack_from('<I', mem, de)[0]
        dest = mem[de + 5] | (mem[de + 6] << 8)
        sel = mem[de + 7]
        if off >= VM_BASE:
            data = self.vm_read(off - VM_BASE, n)
            fname, foff = 'VM', off - VM_BASE
        else:
            data = self.fw.read(off, n)
            fname, foff = self.fw.name_at(off)
        self.write_block(dest, data, sel)
        self.fwreads.append((self.ticks, self.ret_addr(), off, n, dest, sel, fname, foff))
        self.log('[%08d] FWREAD %-12s +%06x len=%04x -> %04x sel=%02x  (caller %04x)' %
                 (self.ticks, fname, foff, n, dest, sel, self.ret_addr()))
        m.a = 1
        self._ret()
        return True

    # ---- VM (settings) storage: NAND area behind the FW-read entry at byte offset >= 16 MB
    def vm_read(self, off, n):
        out = bytearray([0xff]) * n
        for i in range(n):
            sec, o = divmod(off + i, 512)
            s = self.vm.get(sec)
            if s is not None:
                out[i] = s[o]
        return bytes(out)

    def _hle_stg(self):
        """RST 18 (storage driver entry). No storage driver DRV is loaded in normal
        operation (only USBDISK/ADFU load SD_/UD_*.DRV, which patch 0x0018); the
        VM_NANF stubs at 0x3FEC/0x3FF1 still call it: A=1 write one 512-byte VM
        sector {DE struct: u32 sector, u8 ?, u16 buf, u8 sel}, A=4 = finish/flush."""
        m = self.m
        if bytes(self.mem[0x18:0x1b]) != bytes([0xc3, 0xbe, 0x32]):
            return False             # a real storage driver patched the vector
        a = m.a
        if a == 1:
            de = m.de
            sec = struct.unpack_from('<I', self.mem, de)[0]
            buf = self.mem[de + 5] | (self.mem[de + 6] << 8)
            data = bytes(self.mem[buf:buf + 512])
            self.vm[sec] = bytearray(data)
            self.vm_dirty = True
            self.log('[%08d] VMWRITE sector %d from %04x (caller %04x)' % (self.ticks, sec, buf, self.ret_addr()))
        elif a == 4:
            pass
        else:
            self.log('[%08d] RST18 A=%02x unhandled (caller %04x)' % (self.ticks, a, self.ret_addr()))
        m.a = 1
        self._ret()
        return True

    def save_vm(self, path):
        with open(path, 'wb') as f:
            for sec in sorted(self.vm):
                f.write(struct.pack('<I', sec) + bytes(self.vm[sec]))

    def load_vm(self, path):
        d = open(path, 'rb').read()
        for i in range(0, len(d) - 515, 516):
            self.vm[struct.unpack_from('<I', d, i)[0]] = bytearray(d[i + 4:i + 516])

    def ret_addr(self):
        sp = self.m.sp
        return self.mem[sp] | (self.mem[(sp + 1) & 0xffff] << 8)

    def _rst18(self):
        if self.trace_syscalls:
            self._trace_rst()
        if self.disk is not None and self.mem[0x18] == 0xc3 and                 (self.mem[0x19] | (self.mem[0x1a] << 8)) != 0x32be:
            return self._hle_ud()
        return self._hle_stg()

    # ---- user disk: RST 18h of UD_*.DRV (entry patched into 0x0018 by UD_Init, API 0x0080)
    def _hle_ud(self):
        """Driver-level HLE of the UD_*.DRV RST 18h entry (UD_SF641 0x35E3):
          DE -> struct {u32 sector; u8 rsv; u16 buf_or_offset; u8 page}
          A=0 SectorRead  : 512 B of logical sector -> buf (page = port-05 lane / 8 = ZRAM)
          A=1 SectorWrite : 512 B buf -> logical sector
          A=2 WordRead    : HL = u16 at byte `offset` of sector (driver caches the sector at 0x3A80)
          A=3 DwordRead   : HL = low u16, BC = high u16 of u32 at `offset`
          A=4 Update      : flush write cache
          A>=5            : `ret` with A unchanged in the real driver
        Returns A=0 on success (FS_FAT16 bank A15: `and a; jr nz error`)."""
        m, mem = self.m, self.mem
        a, de = m.a, m.de
        sec = struct.unpack_from('<I', mem, de)[0]
        arg = mem[(de + 5) & 0xffff] | (mem[(de + 6) & 0xffff] << 8)
        sel = mem[(de + 7) & 0xffff]
        caller = self.ret_addr()
        self.ud_log.append((self.ticks, a, sec, arg, sel, caller))
        if a == 0:
            self.write_block(arg, self.ud_read(sec), sel)
            m.a = 0
        elif a == 1:
            self.ud_write(sec, self.read_block(arg, 512, sel))
            m.a = 0
        elif a in (2, 3):
            d = self.ud_read(sec)
            o = arg & 0x1ff
            v = d[o:o + 4] + bytes(4)
            m.hl = v[0] | (v[1] << 8)
            if a == 3:
                m.bc = v[2] | (v[3] << 8)
            m.a = 0
        elif a == 4:
            m.a = 0
        if os.environ.get('UDLOG'):
            self.log('[%08d] UD A=%d sec=%d arg=%04x sel=%02x (caller %04x)' % (self.ticks, a, sec, arg, sel, caller))
        self._ret()
        return True

    # UD logical sector space (evidence: SYSBNKB slot 1 0x4872 `ld de,0x0200` = base of the
    # flash drive's FAT area; DRV_ActiveDrive reads base+0 and checks 0xEB / MBR):
    #   sectors 0..511   = VM (settings) area, also reached by FW-read offsets >= 16 MB
    #   sectors 512..    = user FAT volume (sector 512 = boot sector or MBR)
    UD_FAT_BASE = 512

    def ud_read(self, sec):
        if sec < self.UD_FAT_BASE:
            s = self.vm.get(sec)
            return bytes(s) if s is not None else bytes([0xff]) * 512
        return self.disk.read(sec - self.UD_FAT_BASE)

    def ud_write(self, sec, data):
        if sec < self.UD_FAT_BASE:
            self.vm[sec] = bytearray(data)
            self.vm_dirty = True
            return
        self.disk.write(sec - self.UD_FAT_BASE, data)

    def read_block(self, addr, n, sel=8):
        return bytes(self.mem[i & 0xffff] for i in range(addr, addr + n))

    def _trace_rst(self):
        m = self.m
        pc = m.pc
        if pc == 0x10 and m.hl == 0x0023:          # InitDSPCode(DE = name, C = part)
            nm = bytes(self.mem[m.de:m.de + 12]).split(bytes(1))[0].decode('latin1', 'replace')
            self.dsp_program = (nm, m.c)
            self.log('[%08d] InitDSPCode %r part %d (caller %04x)' % (self.ticks, nm, m.c, self.ret_addr()))
            if self.dsp is not None:
                self.dsp.on_load(nm, m.c)
        caller = self.ret_addr()
        key = (pc, m.hl if pc in (0x10, 0x18, 0x30) else m.a)
        self.syscall_stats[key] += 1
        self.syscalls.append((self.ticks, pc, m.a, m.hl, m.de, m.bc, caller))
        return False

    # ------------------------------------------------------------------ memory
    def write_block(self, addr, data, sel=8):
        """Store FW-read / UD-read data like BREC's reader (0x3E30) does.
        `sel` is the page byte of the request (struct +7):
          8            -> plain ZRAM1 (dest < 0x4000)
          0-2 / 4-6 with dest < 0x4000 -> DMA into DSP IPM/IDM lane `sel`, word address
                          `dest` (InitDSPCode: header to IDM-L 0x3000, read back at 0x7010
                          with port 05 = 0xF4; code sections to word 0x0000..)
          otherwise (dest >= 0x4000, any sel) -> the default ZRAM view of 0x4000-0x7FFF
                          (kernel bank-B loads use sel = in(05)&7 with dest 0x4800; the UI
                          driver uses sel 7 = ZRAM2 at 0x41A2).
        The middle rule is INFERRED from SYSBNKA InitDSPCode (bank 0x23) + BREC 0x3E30
        (which ORs the lane into port 05 and sets DMA DA3 bit6 for sel < 8)."""
        a = addr & 0xffff
        n = len(data)
        if sel in (0, 1, 2, 4, 5, 6) and a < 0x4000:
            for i in range(n):
                self.dsp_write(sel, a + i, data[i])
            return
        if a < 0x4000 and a + n <= 0x4000:
            self.mem[a:a + n] = data
            return
        if 0x4000 <= a and a + n <= 0x8000:
            self.zram_write(a, bytes(data))
            return
        for i in range(n):
            x = (a + i) & 0xffff
            if x < 0x4000:
                self.mem[x] = data[i]
            elif x < 0x8000:
                self.zram_write(x, data[i:i + 1])
            elif not self.ext_lcd:
                self.mem[x] = data[i]

    def peek(self, a):
        return self.mem[a & 0xffff]

    # ---- port 0x05 window -------------------------------------------------------------
    @staticmethod
    def _half_src(h, v):
        """What the MCU sees in half h (0: 4000-5FFF, 1: 6000-7FFF) for port-05 value v.
        Device-measured (probe app, coordinator 2026-11-05) + ATJ2085X PG p.12-13:
          lane 7 (xxxxx111, stock value 0xF7) -> ZRAM2 (4 KB at 0x4000-0x4FFF; 0x5000-0x7FFF
                 is kept as RAM here - not measured)
          lanes 0-2 IPM L/M/H, 4-6 IDM L/M/H -> that lane's 8 KB half if the half is given to
                 the MCU (bit7/6 = IPM low/high half, bit5/4 = IDM low/high half); each lane is
                 a separate 16 KB page (0xC0-0xC2 / 0x34-0x36 = whole page)
          anything else (e.g. 0x00, lane 3) -> reads 0, writes are lost."""
        lane = v & 7
        if lane == 7:
            return ('z',)
        if lane <= 2:
            own = v & (0x80 if h == 0 else 0x40)
        elif 4 <= lane <= 6:
            own = v & (0x20 if h == 0 else 0x10)
        else:
            own = 0
        return ('d', lane) if own else ('0',)

    def _backing(self, src, h):
        if src[0] == '0':
            return bytearray(0x4000)          # open bus: reads 0, writes discarded
        return self.zram_win if src[0] == 'z' else self.dsp_lanes[src[1]]

    def _set_p05(self, new):
        old = self.p05
        self.p05 = new
        for h in (0, 1):
            o, n = self._half_src(h, old), self._half_src(h, new)
            if o != n:
                a = 0x4000 + h * 0x2000
                self._backing(o, h)[h * 0x2000:(h + 1) * 0x2000] = self.mem[a:a + 0x2000]
                self.mem[a:a + 0x2000] = self._backing(n, h)[h * 0x2000:(h + 1) * 0x2000]

    def dsp_read(self, lane, addr):
        """Byte `lane` of DSP word `addr` (0..0x3FFF) - live window content if MCU-owned."""
        addr &= 0x3fff
        h = addr >> 13
        if self._half_src(h, self.p05) == ('d', lane):
            return self.mem[0x4000 + addr]
        return self.dsp_lanes[lane][addr]

    def dsp_write(self, lane, addr, val):
        addr &= 0x3fff
        h = addr >> 13
        if self._half_src(h, self.p05) == ('d', lane):
            self.mem[0x4000 + addr] = val & 0xff
        else:
            self.dsp_lanes[lane][addr] = val & 0xff

    def zram_write(self, addr, data):
        """Write into the default (ZRAM) view of 0x4000-0x7FFF regardless of port 05."""
        for h in (0, 1):
            lo, hi = 0x4000 + h * 0x2000, 0x6000 + h * 0x2000
            a0, a1 = max(addr, lo), min(addr + len(data), hi)
            if a0 >= a1:
                continue
            chunk = data[a0 - addr:a1 - addr]
            if self._half_src(h, self.p05) == ('z',):
                self.mem[a0:a1] = chunk
            else:
                self.zram_win[a0 - 0x4000:a1 - 0x4000] = chunk

    # ---- DMA1 (ports 0x06-0x13, PG p.14-16) ------------------------------------------------
    def _dma_addr(self, base):
        p = self.ports
        a = p[base] | (p[base + 1] << 8) | (p[base + 2] << 16)
        a3 = p[base + 3]
        return a, a3

    def _dma1(self):
        p = self.ports
        sa, sa3 = self._dma_addr(0x06)
        da, da3 = self._dma_addr(0x0b)
        isa, ida = p[0x0a] & 7, p[0x0f] & 7
        n = (p[0x10] | (p[0x11] << 8)) + 1
        mode = p[0x12]
        dspmode = bool(mode & 0x20)
        st = dict(sa=sa, sa3=sa3, isa=isa, da=da, da3=da3, ida=ida, n=n, mode=mode)

        def gen(addr, a3, lane, down):
            # yields (kind, lane, address) per byte
            if a3 & 0x80:
                while True:
                    yield ('x', 0, addr); addr += -1 if down else 1
            if not (a3 & 0x40) or lane == 7:
                while True:
                    yield ('z', 0, addr & 0xffff); addr += -1 if down else 1
            base = lane & 4
            li = lane & 3
            addr &= 0x3fff
            while True:
                yield ('d', base + li, addr)
                if dspmode:
                    if down:
                        li -= 1
                        if li < 0:
                            li = 2; addr = (addr - 1) & 0x3fff
                    else:
                        li += 1
                        if li > 2:
                            li = 0; addr = (addr + 1) & 0x3fff
                else:
                    addr = (addr + (-1 if down else 1)) & 0x3fff

        src = gen(sa, sa3, isa, bool(mode & 0x08))
        dst = gen(da, da3, ida, bool(mode & 0x10))
        data = bytearray(n)
        nand = self.nand
        for i in range(n):
            k, l, a = next(src)
            if k == 'z':
                data[i] = self.mem[a]
            elif k == 'd':
                data[i] = self.dsp_read(l, a)
            elif nand is not None:
                data[i] = nand.dma_read(a, sa3)
            else:
                data[i] = 0xff
        for i in range(n):
            k, l, a = next(dst)
            if k == 'z':
                if not (a >= 0x8000 and (self.ext_lcd or self.ext_nand is not None)):
                    self.mem[a] = data[i]
            elif k == 'd':
                self.dsp_write(l, a, data[i])
            elif nand is not None:
                nand.dma_write(a, da3, data[i])
        self.dma_log.append((self.ticks, st))
        p[0x13] &= ~0x01
        p[0x25] |= 0x08             # DMA1 end (PG p.19)
        if self.dsp is not None:
            self.dsp.on_dma(st, bytes(data))

    def _ext_select(self):
        p02 = self.ports[0x02]
        lcd = p02 == 0x18
        ce = (p02 >> 3) & 7
        # NAND: port 02 bits 5-3 = EMA28:26 = chip enable (CE1 = boot NAND); CE0 = boot ROM /
        # internal ROM, CE3 = LCD (8080 LCM). Only with the hardware NAND model (--nand).
        nce = ce if (self.nand is not None and not lcd and ce not in (0, 3)) else None
        was = self.ext_lcd or self.ext_nand is not None
        self.ext_lcd = lcd
        self.ext_nand = nce
        now = lcd or nce is not None
        if now != was:
            if now:
                self.m.mark_addrs(0x8000, 0x8000, RMARK | WMARK)
            else:
                self.m.unmark_addrs(0x8000, 0x8000, RMARK | WMARK)

    def _ext_write(self, addr, v):
        if self.ext_lcd:
            self.lcd_writes += 1                           # external-memory stores to the LCD (3 wait states each on the device)
            self.lcd.write(self.ports[0xf4] & 4, v)
        elif self.ext_nand is not None:
            self.nand.mem_write(self.ext_nand, v)
        else:
            self.mem[addr] = v

    def _ext_read(self, addr):
        if self.ext_lcd:
            return self.lcd.read(self.ports[0xf4] & 4)
        if self.ext_nand is not None:
            return self.nand.mem_read(self.ext_nand)
        return self.mem[addr]

    # ------------------------------------------------------------------ ports
    def _plog(self, d, p, v):
        self.port_stats[(d, p)] += 1
        self.port_log.append((self.ticks, self.m.pc, d, p, v))
        if p not in MODELLED and not (self.nand is not None and p in NAND_PORTS):
            k = (d, p)
            self.unknown_seen[k] += 1
            if self.unknown_seen[k] <= 3:
                self.log('[%08d pc=%04x] %s port %02x (%s) = %02x' % (
                    self.ticks, self.m.pc, 'IN ' if d == 'i' else 'OUT', p,
                    PORT_NAMES.get(p, '?'), v))

    def _in(self, port):
        p = port & 0xff
        if p == 0x26:
            v = self._mint_status()
        elif p == 0x44:
            v = self.ports[0x44]
        elif 0x45 <= p <= 0x47:
            v = (self.rtc_latch >> (8 * (p - 0x45))) & 0xff
        elif p == 0x43:
            # RTC_CTRL bit3: busy while an RTC/LOSC register update is in progress;
            # code at bank-A 0x042D spins on it -> report "done" immediately (model).
            v = self.ports[0x43]
            self.ports[0x43] &= ~0x08
        elif 0x30 <= p <= 0x37 and self.dsp is not None:
            v = self.dsp.mcu_in(p)
        elif p == 0x50:
            # USB_STATUS bit 6 = cable/VBUS present (kernel 0x2F38 polls it; the device posts
            # system message 0x91 on plug, 0x92 on unplug - confirmed on hardware)
            v = (self.ports[0x50] & ~0x40) | (0x40 if self.usb_cable else 0)
        elif p == 0xf4 and self.i2c is not None:
            v = self.i2c.port_in(self.ports[0xf4])
        elif p == 0xf2 and self.key_model:
            v = self.key_model.port_in(self, p)
        elif self.nand is not None and p == 0x28:
            v = self.nand.port_in(p)
        elif p == 0x8b and self.dac is not None:
            v = self.dac.port_in(p, self.ports[p])
        else:
            v = self.ports[p]
        self._plog('i', p, v)
        return v

    def _out(self, port, v):
        p = port & 0xff
        self._plog('o', p, v)
        if p == 0x44:     # RTC_IRQSTAT: write-1-to-clear for bits 0,1 (SYSCFG 0x2ED8 handler)
            self.ports[0x44] &= ~(v & 0x03) & 0xff
            self.ports[0x44] = (self.ports[0x44] & 0x03) | (v & 0xfc)
            return
        if p == 0x26:     # MINT_STATUS: writing a bit acks it (kbd 0x40, dsp 0x41)
            self.ports[0x26] &= ~v & 0xff
            return
        if p == 0x25:
            self.ports[0x25] &= ~v & 0xff
            return
        self.ports[p] = v
        if self.dac is not None and 0x89 <= p <= 0x8b:
            self.dac.port_out(p, v)
        if p in self.watch:                                # (exact time, port, value) of OUTs to the watched ports
            self.watch_log.append((self.now(), p, v))
        if p == 0x43:
            if v & 0x10:                                   # clear the counter
                self.rtc_cbase = -(self.rtc_cnt // self.rtc_slow_div)
            if v & 0x08:                                   # latch it for reading 45h-47h
                self.rtc_latch = self.rtc_counter()
        if p == 0x02:
            self._ext_select()
        elif p == 0x05:
            self._set_p05(v)
        elif p == 0x13 and v & 1:
            self._dma1()
        elif 0x30 <= p <= 0x37 or p == 0x2e:
            if self.dsp is not None:
                self.dsp.mcu_out(p, v)
        elif self.nand is not None and p in (0x2a, 0xcc):
            self.nand.port_out(p, v)
        if self.i2c is not None and p in (0xf3, 0xf4):
            self.i2c.pins_changed()
        if self.key_model and p in (0xee, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xfe, 0xff, 0xef, 0xc1):
            self.key_model.port_out(self, p, v)

    def now(self):
        """T-states elapsed, exact inside a run() slice (self.ticks is only updated per slice)"""
        if not self._slice_n:
            return self.ticks
        return self.ticks + self._slice_n - self.m.ticks_to_stop

    def rtc_counter(self):
        return (self.rtc_cnt // self.rtc_slow_div + self.rtc_cbase) & 0xFFFFFF

    def _mint_status(self):
        s = self.ports[0x26]
        if self.ports[0x44] & 3:
            s |= 0x20
        return s

    # ------------------------------------------------------------------ run
    CPU_HZ = CPU_HZ

    def _tick_devices(self):
        if self.dsp is not None:
            self.dsp.step()
        if self.key_model:
            self.key_model.tick(self)
        if self.ticks >= self.rtc_next:
            self.rtc_next = self.ticks + self.rtc_fast_period
            self.rtc_cnt += 1
            self.ports[0x44] |= 0x02
            if self.rtc_cnt % self.rtc_slow_div == 0:
                self.ports[0x44] |= 0x01
        if self._mint_status() & self.ports[0x27]:
            m = self.m
            if m.iff1:
                m.on_handle_active_int()

    def run(self, max_ticks, slice_ticks=2000):
        """Run for ~max_ticks T-states. Returns stop reason or None."""
        m = self.m
        end = self.ticks + max_ticks
        self.stopped_reason = None
        if self.brom_recovery:
            self.stopped_reason = 'BROM: no valid BREC -> ROM recovery ADFU'
            return self.stopped_reason
        while self.ticks < end and not self.stopped_reason:
            n = min(slice_ticks, end - self.ticks)
            m.ticks_to_stop = n
            self._slice_n = n
            while True:
                ev = m.run()
                if ev & BP:
                    pc = m.pc
                    if pc in self.user_bps:
                        self.stopped_reason = 'breakpoint %04x' % pc
                        self.ticks += n - m.ticks_to_stop; self._slice_n = 0
                        return self.stopped_reason
                    h = self.bp_handlers.get(pc)
                    handled = False
                    if h:
                        handled = h[0]()
                        if pc in self.oneshot:
                            self.oneshot.discard(pc)
                            del self.bp_handlers[pc]
                            m.clear_breakpoint(pc)
                    if not handled:
                        ev2 = m.step_over_breakpoint()
                        if ev2 & TL:
                            break
                    if m.ticks_to_stop == 0:
                        break
                    continue
                if ev & TL or m.ticks_to_stop == 0:
                    break
            self.ticks += n - m.ticks_to_stop if m.ticks_to_stop else n
            self._slice_n = 0
            self._tick_devices()
        return self.stopped_reason

    def step(self):
        """Execute exactly one instruction (debugger)."""
        m = self.m
        pc = m.pc
        if pc in self.bp_handlers and pc not in self.user_bps:
            if self.bp_handlers[pc][0]():
                return
        m.ticks_to_stop = 1
        if pc in self.user_bps or pc in self.bp_handlers:
            m.step_over_breakpoint()
        else:
            m.run()
        self.ticks += 4

    # ------------------------------------------------------------------ debug helpers
    def disasm(self, addr, count=20):
        out = []
        buf = bytes(self.mem[0:0x10000]) + bytes(4)
        for _ in range(count):
            if z80dis:
                try:
                    i = z80dis.decode(buf, addr, 0)
                    out.append((addr, buf[addr:addr + i.size].hex(), i.text))
                    addr = (addr + i.size) & 0xffff
                    continue
                except Exception:
                    pass
            out.append((addr, '%02x' % buf[addr], '.db'))
            addr = (addr + 1) & 0xffff
        return out

    def regs(self):
        m = self.m
        return dict(pc=m.pc, sp=m.sp, af=m.af, bc=m.bc, de=m.de, hl=m.hl, ix=m.ix, iy=m.iy,
                    af_=m.alt_af, bc_=m.alt_bc, de_=m.alt_de, hl_=m.alt_hl, iff1=int(m.iff1),
                    im=m.int_mode if hasattr(m, 'int_mode') else -1, halted=int(m.halted),
                    page=self.p05)

    def set_usb(self, plugged):
        self.usb_cable = bool(plugged)
        self.log('[%08d] USB cable %s' % (self.ticks, 'plugged' if plugged else 'unplugged'))

    def press(self, key, ms=150):
        if self.key_model:
            self.key_model.press(self, key, ms)
