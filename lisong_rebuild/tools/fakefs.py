"""High-level fake user disk for the emulator test runner: serves a host folder through the
OS calls the homebrew launcher uses (RST 10h HL = API number, IAR register convention).
Every other syscall passes through to the real firmware."""
import os

APIS = {0x1300: 'DRV_ActiveUD', 0x1101: 'DRV_ActiveDrive', 0x1080: 'FS_Init', 0x1081: 'FS_CD',
        0x1082: 'FS_Dir', 0x1087: 'FS_FOpen', 0x128c: 'FS_FRead', 0x108a: 'FS_FClose'}


class FakeFS:
    def __init__(self, emu, folder, log=print):
        self.emu, self.folder, self.log = emu, folder, log
        self.files = sorted(f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f)))
        self.dir_idx = 0
        self.handles = {}
        self.calls = []
        emu.add_hle(0x0010, self.hook, name='fakefs')

    def name83(self, f):
        b, _, e = f.upper().partition('.')
        return (b[:8].ljust(8) + e[:3].ljust(3)).encode()

    def hook(self):
        e, m = self.emu, self.emu.m
        api = m.hl
        if api not in APIS:
            return False                       # real OS
        mem = e.mem
        sp = m.sp
        arg_stack = mem[(sp + 2) & 0xffff] | mem[(sp + 3) & 0xffff] << 8
        a = 1
        if api == 0x1082:                      # FS_Dir(DE=buf, BC=ext, [sp+2]=type)
            ext = bytes(mem[m.bc:m.bc + 3]).decode('latin1')
            if arg_stack & 0x80:
                self.dir_idx = 0
            a = 0
            while self.dir_idx < len(self.files):
                n83 = self.name83(self.files[self.dir_idx]); self.dir_idx += 1
                if ext.strip('* ') == '' or n83[8:11].decode() == ext:
                    mem[m.de:m.de + 12] = n83 + b'\0'
                    a = 1
                    break
        elif api == 0x1087:                    # FS_FOpen(DE=name) -> HL
            n83 = bytes(mem[m.de:m.de + 11])
            hit = [f for f in self.files if self.name83(f) == n83]
            if hit:
                h = len(self.handles) + 1
                self.handles[h] = [open(os.path.join(self.folder, hit[0]), 'rb').read(), 0]
                m.hl = h
            else:
                m.hl = 0
        elif api == 0x128c:                    # FS_FRead(DE=buf, C=sectors, [sp+2]=handle)
            data, pos = self.handles.get(arg_stack, (b'', 0))
            n = m.c * 512
            chunk = data[pos:pos + n]
            if chunk:
                chunk = chunk + bytes(n - len(chunk))
                mem[m.de:m.de + n] = chunk
                self.handles[arg_stack][1] = pos + n
            else:
                a = 0
        elif api == 0x108a:
            self.handles.pop(m.de, None)
        self.calls.append((APIS[api], m.de, m.bc, a))
        m.a = a
        e._ret()
        return True
