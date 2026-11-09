"""tkinter front end: LCD, buttons, debugger pane (registers / disassembly / memory /
port + syscall trace)."""
import os
import sys
import time


def _fix_tcl_env():
    """A foreign TCL_LIBRARY (e.g. set by CSR BlueSuite) breaks tkinter; point it
    back at this interpreter's own Tcl/Tk if it has no init.tcl."""
    lib = os.environ.get('TCL_LIBRARY')
    if lib and not os.path.exists(os.path.join(lib, 'init.tcl')):
        base = os.path.join(sys.base_prefix, 'tcl')
        os.environ['TCL_LIBRARY'] = os.path.join(base, 'tcl8.6')
        os.environ['TK_LIBRARY'] = os.path.join(base, 'tk8.6')


_fix_tcl_env()
import tkinter as tk  # noqa: E402
from tkinter import ttk, filedialog  # noqa: E402

from .soc import ATJ2085, PORT_NAMES, RST_NAMES
from .fwimage import FwImage
from .keys import KEYS
from . import pngout

SCALE = 3
KEYMAP = {'Return': 'MENU', 'm': 'MENU', 'space': 'PLAY', 'p': 'PLAY',
          'Left': 'LEFT', 'Up': 'LEFT', 'Right': 'RIGHT', 'Down': 'RIGHT', 'k': 'K0D'}


class App:
    def __init__(self, fw_path, vm_path=None, ticks_per_frame=400_000, disk_factory=None,
                 sink_factory=None, live_audio=False, nand=None, nand_overlay=None):
        self.fw_path = fw_path
        self.nand = nand                      # s1clone dump path (hardware-level NAND) or None
        self.nand_overlay = nand_overlay
        self.vm_path = vm_path
        self.disk_factory = disk_factory      # () -> emu.disk.UserDisk or None
        self.sink_factory = sink_factory      # () -> emu.audio.AudioSink or None
        self.live_audio = live_audio
        self.sink = None
        self.root = tk.Tk()
        self.root.title('ATJ2085 emulator - %s' % fw_path)
        self.ticks_per_frame = tk.IntVar(value=ticks_per_frame)
        self.running = False
        self.logbuf = []
        self._build()
        self.reset()

    # ---------------------------------------------------------------- setup
    def log(self, s):
        self.logbuf.append(s)

    def reset(self):
        from .dsp import DspHLE
        if self.sink is not None:
            self.sink.close()
        self.sink = self.sink_factory() if self.sink_factory else None
        disk = self.disk_factory() if self.disk_factory else None
        import os
        self.emu = ATJ2085(FwImage(self.fw_path), log=self.log, disk=disk, nand=self.nand,
                           nand_overlay=self.nand_overlay,
                           nand_cache=os.path.join(os.path.dirname(__file__), '..', 'work', 'nandcache')
                           if self.nand else None)
        self.emu.dsp = DspHLE(self.emu, self.sink, pacing='live' if self.live_audio else 'emu', log=self.log)
        if self.vm_path:
            try:
                self.emu.load_vm(self.vm_path)
            except OSError:
                pass
        self.refresh()

    def _build(self):
        r = self.root
        left = ttk.Frame(r, padding=6)
        left.grid(row=0, column=0, sticky='n')
        right = ttk.Frame(r, padding=6)
        right.grid(row=0, column=1, sticky='nsew')
        r.columnconfigure(1, weight=1)
        r.rowconfigure(0, weight=1)

        self.canvas = tk.Canvas(left, width=128 * SCALE, height=128 * SCALE, bg='black',
                                highlightthickness=0)
        self.canvas.grid(row=0, column=0, columnspan=5)
        self.img = tk.PhotoImage(width=128 * SCALE, height=128 * SCALE)
        self.canvas.create_image(0, 0, image=self.img, anchor='nw')

        for i, k in enumerate(['LEFT', 'MENU', 'RIGHT', 'PLAY', 'K0D']):
            b = ttk.Button(left, text=k, width=6)
            b.grid(row=1, column=i, pady=4)
            b.bind('<ButtonPress-1>', lambda e, k=k: self.emu.key_model.hold(k))
            b.bind('<ButtonRelease-1>', lambda e, k=k: self.emu.key_model.release(k))
        ttk.Label(left, text='keys: Left/Up=LEFT(<<) Right/Down=RIGHT(>>) Enter/m=MENU Space/p=PLAY k=K0D').grid(
            row=2, column=0, columnspan=5)

        ctl = ttk.Frame(left)
        ctl.grid(row=3, column=0, columnspan=5, pady=6)
        self.runbtn = ttk.Button(ctl, text='Run', command=self.toggle)
        self.runbtn.grid(row=0, column=0)
        ttk.Button(ctl, text='Step', command=self.step).grid(row=0, column=1)
        ttk.Button(ctl, text='Step 100', command=lambda: self.step(100)).grid(row=0, column=2)
        ttk.Button(ctl, text='Reset', command=self.reset).grid(row=0, column=3)
        ttk.Button(ctl, text='Save PNG', command=self.save_png).grid(row=0, column=4)
        self.usb_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(ctl, text='USB cable', variable=self.usb_var,
                        command=lambda: self.emu.set_usb(self.usb_var.get())).grid(row=0, column=5)
        ttk.Label(ctl, text='T-states/frame').grid(row=1, column=0, columnspan=2)
        ttk.Entry(ctl, textvariable=self.ticks_per_frame, width=10).grid(row=1, column=2)
        ttk.Label(ctl, text='Breakpoint').grid(row=2, column=0)
        self.bp_entry = ttk.Entry(ctl, width=8)
        self.bp_entry.grid(row=2, column=1)
        ttk.Button(ctl, text='Toggle BP', command=self.toggle_bp).grid(row=2, column=2)
        self.bplabel = ttk.Label(ctl, text='')
        self.bplabel.grid(row=2, column=3, columnspan=2)
        self.status = ttk.Label(left, text='', width=60)
        self.status.grid(row=4, column=0, columnspan=5)

        nb = ttk.Notebook(right)
        nb.pack(fill='both', expand=True)
        mono = ('Consolas', 9)
        cpu = ttk.Frame(nb)
        nb.add(cpu, text='CPU')
        self.regtext = tk.Text(cpu, height=6, width=90, font=mono)
        self.regtext.pack(fill='x')
        self.distext = tk.Text(cpu, height=26, width=90, font=mono)
        self.distext.pack(fill='both', expand=True)

        memf = ttk.Frame(nb)
        nb.add(memf, text='Memory')
        top = ttk.Frame(memf)
        top.pack(fill='x')
        ttk.Label(top, text='addr (hex)').pack(side='left')
        self.memaddr = ttk.Entry(top, width=8)
        self.memaddr.insert(0, '0400')
        self.memaddr.pack(side='left')
        ttk.Button(top, text='Show', command=self.refresh).pack(side='left')
        self.memtext = tk.Text(memf, height=32, width=90, font=mono)
        self.memtext.pack(fill='both', expand=True)

        tr = ttk.Frame(nb)
        nb.add(tr, text='Ports')
        self.porttext = tk.Text(tr, height=32, width=90, font=mono)
        self.porttext.pack(fill='both', expand=True)

        sc = ttk.Frame(nb)
        nb.add(sc, text='Syscalls')
        self.systext = tk.Text(sc, height=32, width=90, font=mono)
        self.systext.pack(fill='both', expand=True)

        lg = ttk.Frame(nb)
        nb.add(lg, text='Log')
        self.logtext = tk.Text(lg, height=32, width=90, font=mono)
        self.logtext.pack(fill='both', expand=True)

        r.bind('<KeyPress>', self._kdown)
        r.bind('<KeyRelease>', self._kup)

    # ---------------------------------------------------------------- input
    def _kdown(self, e):
        k = KEYMAP.get(e.keysym)
        if k and not isinstance(e.widget, (tk.Entry, ttk.Entry)):
            self.emu.key_model.hold(k)

    def _kup(self, e):
        k = KEYMAP.get(e.keysym)
        if k:
            self.emu.key_model.release(k)

    # ---------------------------------------------------------------- control
    def toggle(self):
        self.running = not self.running
        self.runbtn.config(text='Pause' if self.running else 'Run')
        if self.running:
            self.root.after(1, self.frame)
        else:
            self.refresh()

    def frame(self):
        if not self.running:
            return
        t0 = time.time()
        try:
            r = self.emu.run(max(1000, int(self.ticks_per_frame.get())))
        except Exception as ex:      # keep GUI alive, show the error
            r = 'exception: %r' % ex
        self.draw_lcd()
        self.status.config(text='ticks=%d pc=%04x  %.0f ms/frame' % (
            self.emu.ticks, self.emu.m.pc, (time.time() - t0) * 1000))
        if r:
            self.running = False
            self.runbtn.config(text='Run')
            self.status.config(text='stopped: ' + r)
            self.refresh()
            return
        self.root.after(max(1, 33 - int((time.time() - t0) * 1000)), self.frame)

    def step(self, n=1):
        for _ in range(n):
            self.emu.step()
        self.refresh()

    def toggle_bp(self):
        try:
            a = int(self.bp_entry.get(), 16)
        except ValueError:
            return
        e = self.emu
        if a in e.user_bps:
            e.user_bps.discard(a)
            if a not in e.bp_handlers:
                e.m.clear_breakpoint(a)
        else:
            e.user_bps.add(a)
            e.m.set_breakpoint(a)
        self.bplabel.config(text=' '.join('%04x' % x for x in sorted(e.user_bps)))

    def save_png(self):
        p = filedialog.asksaveasfilename(defaultextension='.png')
        if p:
            pngout.save_lcd(self.emu.lcd, p)

    # ---------------------------------------------------------------- views
    def draw_lcd(self):
        w, h, rgb = self.emu.lcd.to_rgb()
        ppm = b'P6 %d %d 255\n' % (w, h) + rgb
        small = tk.PhotoImage(data=ppm, format='PPM')
        self.img.blank()
        self.img.tk.call(self.img, 'copy', small, '-zoom', SCALE, SCALE)
        self._small = small

    def refresh(self):
        self.draw_lcd()
        e = self.emu
        r = e.regs()
        self.regtext.delete('1.0', 'end')
        self.regtext.insert('end',
                            'PC=%(pc)04x SP=%(sp)04x AF=%(af)04x BC=%(bc)04x DE=%(de)04x HL=%(hl)04x IX=%(ix)04x IY=%(iy)04x\n'
                            "AF'=%(af_)04x BC'=%(bc_)04x DE'=%(de_)04x HL'=%(hl_)04x IFF1=%(iff1)d HALT=%(halted)d\n" % r)
        self.regtext.insert('end', 'ticks=%d  irq-nest[0x37]=%d  MINT_EN=%02x RTC_IRQ=%02x  EM_PAGE_HI=%02x GPIOC=%02x\n' % (
            e.ticks, e.mem[0x37], e.ports[0x27], e.ports[0x44], e.ports[0x02], e.ports[0xf4]))
        sp = r['sp']
        self.regtext.insert('end', 'stack: ' + ' '.join('%04x' % (e.mem[(sp + 2 * i) & 0xffff] | e.mem[(sp + 2 * i + 1) & 0xffff] << 8) for i in range(10)) + '\n')
        self.distext.delete('1.0', 'end')
        for a, h, t in e.disasm(r['pc'], 30):
            sym = e.rom_syms.get(a, '')
            mark = '>' if a == r['pc'] else ('*' if a in e.user_bps else ' ')
            self.distext.insert('end', '%s %04x  %-10s %-28s %s\n' % (mark, a, h, t, sym))
        try:
            ma = int(self.memaddr.get(), 16)
        except ValueError:
            ma = 0
        self.memtext.delete('1.0', 'end')
        for row in range(32):
            a = (ma + row * 16) & 0xffff
            bs = bytes(e.mem[(a + i) & 0xffff] for i in range(16))
            self.memtext.insert('end', '%04x  %s  %s\n' % (a, bs.hex(' '), ''.join(chr(b) if 32 <= b < 127 else '.' for b in bs)))
        self.porttext.delete('1.0', 'end')
        self.porttext.insert('end', 'last port accesses (tick, pc-after-insn, dir, port, value):\n')
        for t, pc, d, p, v in list(e.port_log)[-300:]:
            self.porttext.insert('end', '%10d %04x %s %02x %-14s %02x\n' % (t, pc, d, p, PORT_NAMES.get(p, '?'), v))
        self.porttext.insert('end', '\ncounts:\n')
        for (d, p), n in sorted(e.port_stats.items(), key=lambda x: x[0][1]):
            self.porttext.insert('end', '  %s %02x %-14s %d\n' % (d, p, PORT_NAMES.get(p, '?'), n))
        self.systext.delete('1.0', 'end')
        for t, v, A, HL, DE, BC, c in list(e.syscalls)[-300:]:
            self.systext.insert('end', '%10d %-12s A=%02x HL=%04x DE=%04x BC=%04x from %04x\n' % (
                t, RST_NAMES.get(v, '%02x' % v), A, HL, DE, BC, c))
        if self.logbuf:
            self.logtext.insert('end', '\n'.join(self.logbuf[-2000:]) + '\n')
            self.logbuf.clear()
            self.logtext.see('end')

    def mainloop(self):
        self.root.mainloop()
        if self.vm_path and self.emu.vm:
            self.emu.save_vm(self.vm_path)
