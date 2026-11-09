"""Assemble stub/zosboot.s into zosboot.bin (the SYSCFG.SYS template zospatch.py uses).

usage: python build_stub.py [--sdcc DIR]
Needs SDCC's sdasz80/sdldz80 (DIR, $SDCC_BIN, PATH, or C:\\Program Files (x86)\\SDCC\\bin).
Outputs next to this script:
  zosboot.bin   0x3800 bytes = the new SYSCFG.SYS before the patcher fills in the parameters:
                  file 0x0000-0x01FF -> RAM 0x0600 (entry + parameter block)
                  file 0x2600-0x2CFF -> RAM 0x2C00 (selector code)
                  file 0x3700-0x377F -> RAM 0x0000 (vector page)
                rest = 0x76 filler. Contains only our own code.
  zosboot.sym   label -> address (JSON), for the emulator tests
"""
import os, sys, json, shutil, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SYS_LEN = 0x3800
FILL = 0x76
REGIONS = [  # (ram start, ram end, file offset)
    (0x0600, 0x0800, 0x0000),
    (0x2C00, 0x3100, 0x2600),
    (0x0000, 0x0080, 0x3700),
]


def find_tool(name, sdcc):
    cands = []
    if sdcc:
        cands.append(os.path.join(sdcc, name))
    if os.environ.get('SDCC_BIN'):
        cands.append(os.path.join(os.environ['SDCC_BIN'], name))
    w = shutil.which(name)
    if w:
        cands.append(w)
    cands.append(os.path.join(r'C:\Program Files (x86)\SDCC\bin', name + '.exe'))
    cands.append(os.path.join(r'C:\Program Files\SDCC\bin', name + '.exe'))
    for c in cands:
        for x in (c, c + '.exe'):
            if os.path.isfile(x):
                return x
    sys.exit('%s not found (install SDCC or pass --sdcc DIR)' % name)


def parse_ihx(txt):
    mem = {}
    for ln in txt.splitlines():
        if not ln.startswith(':'):
            continue
        b = bytes.fromhex(ln[1:])
        n, a, t = b[0], (b[1] << 8) | b[2], b[3]
        if t == 0:
            for i in range(n):
                mem[a + i] = b[4 + i]
    return mem


def main():
    a = sys.argv[1:]
    sdcc = a[a.index('--sdcc') + 1] if '--sdcc' in a else None
    sdas, sdld = find_tool('sdasz80', sdcc), find_tool('sdldz80', sdcc)
    with tempfile.TemporaryDirectory() as tmp:
        rel = os.path.join(tmp, 'zosboot.rel')
        for cmd in ([sdas, '-g', '-l', '-I' + os.path.join(HERE, 'stub'), '-o', rel,
                     os.path.join(HERE, 'stub', 'zosboot.s')],
                    [sdld, '-n', '-m', '-i', 'zosboot.ihx', rel]):
            r = subprocess.run(cmd, cwd=tmp, capture_output=True, text=True)
            if r.returncode or 'rror' in r.stdout + r.stderr:
                sys.exit(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        mem = parse_ihx(open(os.path.join(tmp, 'zosboot.ihx')).read())
        syms = {}
        for ln in open(os.path.join(tmp, 'zosboot.map'), errors='replace'):
            for col in ln.split('|'):
                p = col.split()
                if len(p) == 2 and len(p[0]) == 8:
                    try:
                        syms[p[1]] = int(p[0], 16)
                    except ValueError:
                        pass
        shutil.copy(os.path.join(tmp, 'zosboot.lst') if os.path.exists(os.path.join(tmp, 'zosboot.lst'))
                    else rel, os.path.join(HERE, 'stub', 'zosboot.lst'))
    img = bytearray([FILL]) * SYS_LEN
    used = set()
    for lo, hi, off in REGIONS:
        for addr in range(lo, hi):
            if addr in mem:
                img[off + addr - lo] = mem[addr]
                used.add(addr)
    stray = sorted(set(mem) - used)
    if stray:
        sys.exit('stub bytes outside the loaded regions: %#06x..' % stray[0])
    end = syms.get('main_end')
    if end is None or end > 0x3100:
        sys.exit('selector code/variables end at %s > 0x3100 (sector buffer)' % (end and hex(end)))
    if img[3:11] != b'ZOSBOOT1' or img[0] != 0xC3:
        sys.exit('entry/parameter block not where zospatch.py expects it')
    if syms.get('p_end') != 0x0636:
        sys.exit('parameter block layout changed (p_end = %s)' % syms.get('p_end'))
    open(os.path.join(HERE, 'zosboot.bin'), 'wb').write(img)
    json.dump(syms, open(os.path.join(HERE, 'zosboot.sym'), 'w'), indent=1, sort_keys=True)
    print('zosboot.bin: entry/params %d B, selector 0x2C00-%#06x (%d B free before the buffer), vectors'
          % (syms['p_end'] - 0x0600, end, 0x3100 - end))


if __name__ == '__main__':
    main()
