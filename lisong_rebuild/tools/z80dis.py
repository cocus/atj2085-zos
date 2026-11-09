"""Z80 disassembler emitting sdasz80 syntax + control-flow info.

decode(mem, pc, base) -> Insn(addr, size, text, kind, target, imm16)
  kind: None | 'jp' | 'jr' | 'jpcc' | 'jrcc' | 'call' | 'callcc' | 'ret' | 'retcc'
        | 'rst' | 'jpind' | 'halt' | 'bad'
  target: branch target (absolute) if any
  imm16: 16-bit immediate/address operand value (for relocation/label purposes)
         and its byte offset inside the insn (imm16_off)
Ambiguous/undocumented encodings return kind 'bad' so callers can emit .db and
keep byte-exact round-trips.
"""
R = ['b', 'c', 'd', 'e', 'h', 'l', '(hl)', 'a']
RP = ['bc', 'de', 'hl', 'sp']
RP2 = ['bc', 'de', 'hl', 'af']
CC = ['nz', 'z', 'nc', 'c', 'po', 'pe', 'p', 'm']
ALU = ['add a,', 'adc a,', 'sub ', 'sbc a,', 'and ', 'xor ', 'or ', 'cp ']
ROT = ['rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'sll', 'srl']
X0Z7 = ['rlca', 'rrca', 'rla', 'rra', 'daa', 'cpl', 'scf', 'ccf']
BLI = {(4, 0): 'ldi', (4, 1): 'cpi', (4, 2): 'ini', (4, 3): 'outi',
       (5, 0): 'ldd', (5, 1): 'cpd', (5, 2): 'ind', (5, 3): 'outd',
       (6, 0): 'ldir', (6, 1): 'cpir', (6, 2): 'inir', (6, 3): 'otir',
       (7, 0): 'lddr', (7, 1): 'cpdr', (7, 2): 'indr', (7, 3): 'otdr'}


class _Undoc(Exception):
    pass


class Insn:
    __slots__ = ('addr', 'size', 'text', 'kind', 'target', 'imm16', 'imm16_off', 'bytes')

    def __init__(self, addr, size, text, kind=None, target=None, imm16=None, imm16_off=None):
        self.addr, self.size, self.text, self.kind = addr, size, text, kind
        self.target, self.imm16, self.imm16_off = target, imm16, imm16_off
        self.bytes = b''

    def __repr__(self):
        return f'{self.addr:04x}: {self.bytes.hex():10s} {self.text}'


def h8(v):
    return f'#0x{v:02x}'


def h16(v):
    return f'#0x{v:04x}'


def a16(v):
    return f'0x{v:04x}'


def sdisp(d):
    return d - 256 if d > 127 else d


def decode(mem, pc, base=0):
    """mem: bytes-like, pc: absolute address, base: absolute address of mem[0]."""
    i = pc - base

    def b(k):
        j = i + k
        if j >= len(mem):
            raise IndexError
        return mem[j]

    try:
        ins = _decode(b, pc)
    except IndexError:
        ins = Insn(pc, 1, f'.db {h8(mem[i]) if i < len(mem) else "?"}', 'bad')
    ins.bytes = bytes(mem[i:i + ins.size])
    return ins


def _decode(b, pc):
    op = b(0)
    if op == 0xCB:
        return _cb(b, pc, 1, None)
    if op == 0xED:
        return _ed(b, pc)
    if op in (0xDD, 0xFD):
        ix = 'ix' if op == 0xDD else 'iy'
        op2 = b(1)
        if op2 in (0xDD, 0xFD, 0xED):
            return Insn(pc, 1, f'.db {h8(op)}', 'bad')
        if op2 == 0xCB:
            return _cb(b, pc, 2, ix)
        try:
            return _main(b, pc, 1, ix)
        except _Undoc:
            return Insn(pc, 1, f'.db {h8(op)}', 'bad')
    return _main(b, pc, 0, None)


def _main(b, pc, p0, ix):
    """p0: prefix length (0 or 1). ix: None or 'ix'/'iy'."""
    op = b(p0)
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    n = p0 + 1  # bytes consumed so far
    used_ix = False
    disp = None

    def reg(r):
        nonlocal used_ix, disp, n
        if ix is None:
            return R[r]
        if r == 6:
            used_ix = True
            if disp is None:
                disp = sdisp(b(n)); n += 1
            return f'{disp}({ix})'
        if r in (4, 5):
            raise _Undoc
        return R[r]

    def rp(k):
        nonlocal used_ix
        if k == 2 and ix:
            used_ix = True
            return ix
        return RP[k]

    def rp2(k):
        nonlocal used_ix
        if k == 2 and ix:
            used_ix = True
            return ix
        return RP2[k]

    def imm8():
        nonlocal n
        v = b(n); n += 1
        return v

    def imm16():
        nonlocal n
        v = b(n) | (b(n + 1) << 8); n += 2
        return v

    kind = target = None
    iv = ioff = None
    t = None
    if x == 0:
        if z == 0:
            if y == 0:
                t = 'nop'
            elif y == 1:
                t = "ex af,af'"
            else:
                d = sdisp(imm8())
                target = (pc + n + d) & 0xFFFF
                if y == 2:
                    t, kind = f'djnz {a16(target)}', 'jrcc'
                elif y == 3:
                    t, kind = f'jr {a16(target)}', 'jr'
                else:
                    t, kind = f'jr {CC[y - 4]},{a16(target)}', 'jrcc'
        elif z == 1:
            if q == 0:
                ioff = n; iv = imm16()
                t = f'ld {rp(p)},{h16(iv)}'
            else:
                t = f'add {rp(2)},{rp(p)}'
        elif z == 2:
            if p < 2:
                t = f'ld ({RP[p]}),a' if q == 0 else f'ld a,({RP[p]})'
            else:
                ioff = n; iv = imm16()
                if p == 2:
                    t = f'ld ({a16(iv)}),{rp(2)}' if q == 0 else f'ld {rp(2)},({a16(iv)})'
                else:
                    t = f'ld ({a16(iv)}),a' if q == 0 else f'ld a,({a16(iv)})'
        elif z == 3:
            t = f'{"inc" if q == 0 else "dec"} {rp(p)}'
        elif z == 4:
            t = f'inc {reg(y)}'
        elif z == 5:
            t = f'dec {reg(y)}'
        elif z == 6:
            r = reg(y)
            t = f'ld {r},{h8(imm8())}'
        else:
            t = X0Z7[y]
    elif x == 1:
        if z == 6 and y == 6:
            t, kind = 'halt', 'halt'
        else:
            # with index prefix, ld h,(ix+d) keeps plain h
            if ix and (y == 6 or z == 6):
                if y == 6:
                    src = R[z]
                    dst = reg(6)
                else:
                    dst = R[y]
                    src = reg(6)
                t = f'ld {dst},{src}'
            else:
                dst = reg(y); src = reg(z)
                t = f'ld {dst},{src}'
    elif x == 2:
        t = f'{ALU[y]}{reg(z)}'
    else:
        if z == 0:
            t, kind = f'ret {CC[y]}', 'retcc'
        elif z == 1:
            if q == 0:
                t = f'pop {rp2(p)}'
            else:
                if p == 0:
                    t, kind = 'ret', 'ret'
                elif p == 1:
                    t = 'exx'
                elif p == 2:
                    t, kind = f'jp ({rp(2)})', 'jpind'
                else:
                    t = f'ld sp,{rp(2)}'
        elif z == 2:
            ioff = n; iv = imm16(); target = iv
            t, kind = f'jp {CC[y]},{a16(iv)}', 'jpcc'
        elif z == 3:
            if y == 0:
                ioff = n; iv = imm16(); target = iv
                t, kind = f'jp {a16(iv)}', 'jp'
            elif y == 2:
                t = f'out ({h8(imm8())}),a'
            elif y == 3:
                t = f'in a,({h8(imm8())})'
            elif y == 4:
                t = f'ex (sp),{rp(2)}'
            elif y == 5:
                t = 'ex de,hl'
            elif y == 6:
                t = 'di'
            elif y == 7:
                t = 'ei'
        elif z == 4:
            ioff = n; iv = imm16(); target = iv
            t, kind = f'call {CC[y]},{a16(iv)}', 'callcc'
        elif z == 5:
            if q == 0:
                t = f'push {rp2(p)}'
            elif p == 0:
                ioff = n; iv = imm16(); target = iv
                t, kind = f'call {a16(iv)}', 'call'
        elif z == 6:
            t = f'{ALU[y]}{h8(imm8())}'
        else:
            target = y * 8
            t, kind = f'rst {h8(y * 8)}', 'rst'
    if ix and not used_ix:
        # useless prefix (e.g. DD 00) -> not reassemblable
        return Insn(pc, 1, f'.db {h8(b(0))}', 'bad')
    if ix and t and 'ex de,hl' in t:
        return Insn(pc, 1, f'.db {h8(b(0))}', 'bad')
    return Insn(pc, n, t, kind, target, iv, ioff)


def _cb(b, pc, p0, ix):
    if ix:
        d = sdisp(b(2)); op = b(3); n = 4
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        if z != 6:
            return Insn(pc, 1, f'.db {h8(b(0))}', 'bad')  # undocumented copy-to-reg forms
        r = f'{d}({ix})'
    else:
        op = b(1); n = 2
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        r = R[z]
    if x == 0:
        if y == 6:
            return Insn(pc, 1, f'.db {h8(b(0))}', 'bad')  # sll undocumented
        t = f'{ROT[y]} {r}'
    elif x == 1:
        t = f'bit {y},{r}'
    elif x == 2:
        t = f'res {y},{r}'
    else:
        t = f'set {y},{r}'
    return Insn(pc, n, t)


def _ed(b, pc):
    op = b(1)
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    n = 2
    if x == 1:
        if z == 0:
            if y == 6:
                return Insn(pc, 1, '.db #0xed', 'bad')
            return Insn(pc, 2, f'in {R[y]},(c)')
        if z == 1:
            if y == 6:
                return Insn(pc, 1, '.db #0xed', 'bad')
            return Insn(pc, 2, f'out (c),{R[y]}')
        if z == 2:
            return Insn(pc, 2, f'{"sbc" if q == 0 else "adc"} hl,{RP[p]}')
        if z == 3:
            v = b(2) | (b(3) << 8)
            if p == 2:  # ED-form ld (nn),hl duplicate -> keep bytes
                return Insn(pc, 1, '.db #0xed', 'bad')
            t = f'ld ({a16(v)}),{RP[p]}' if q == 0 else f'ld {RP[p]},({a16(v)})'
            return Insn(pc, 4, t, None, None, v, 2)
        if z == 4:
            if y != 0:
                return Insn(pc, 1, '.db #0xed', 'bad')
            return Insn(pc, 2, 'neg')
        if z == 5:
            if y == 0:
                return Insn(pc, 2, 'retn', 'ret')
            if y == 1:
                return Insn(pc, 2, 'reti', 'ret')
            return Insn(pc, 1, '.db #0xed', 'bad')
        if z == 6:
            m = {0: 'im 0', 2: 'im 1', 3: 'im 2'}.get(y)
            if m is None:
                return Insn(pc, 1, '.db #0xed', 'bad')
            return Insn(pc, 2, m)
        if z == 7:
            m = ['ld i,a', 'ld r,a', 'ld a,i', 'ld a,r', 'rrd', 'rld', None, None][y]
            if m is None:
                return Insn(pc, 1, '.db #0xed', 'bad')
            return Insn(pc, 2, m)
    if x == 2 and (y, z) in BLI:
        return Insn(pc, 2, BLI[(y, z)])
    return Insn(pc, 1, '.db #0xed', 'bad')
