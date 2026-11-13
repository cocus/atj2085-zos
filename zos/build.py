"""Build zos.

usage: python build.py [--target atj2085] [--no-deploy] [--drive G:]
  -> out/<target>/ZOS.AP  (hosted image: loaded by the homebrew launcher from the player's disk)
  -> out/<target>/<NAME>.ZM for every modules/<name>/ (copy next to ZOS.AP)

Layout (atj2085, hosted): one image at 0x0800 (code, data, bss), entry 0x0800 = target crt0.
Limit 0x2C00 (the launcher's stub lives at 0x2C00). Packaging uses lisong_rebuild/tools/aptool.py.
Link order: crt0 (jp at 0x0800), jtab (generated from core/api.txt, at 0x0803), then the rest.
Environment: ZOS_BUILD_TIME=YYYY-MM-DDTHH:MM overrides the build time stamped into buildtime.h
(default: now), so two builds of the same sources are byte-identical.
Modules: mod_crt0 + module sources + api.s (absolute API symbols), linked at 0x4000.
"""
import os, sys, glob, re, subprocess, shutil, json, hashlib, struct, concurrent.futures
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'lisong_rebuild', 'tools'))
import aptool
from asm_util import SDAS, SDLD, parse_ihx

SDCC = r'C:\Program Files (x86)\SDCC\bin\sdcc.exe'
SDCC_LIB = r'C:\Program Files (x86)\SDCC\lib\z80'
TARGETS = {'atj2085': dict(load=0x0800, limit=0x2C00, ram_limit=0x2E80, mod_base=0x4000, mod_limit=0x8000,
                              zram2_size=0x1000, modram=(0x2E80, 0x3E00),
                              )}   # ZRAM2 modules: vars from 0x2E80, then _CODE2 (build_module)
JTAB = 0x0803


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode or "Undefined" in r.stdout + r.stderr:
        sys.exit(' '.join(cmd) + '\n' + r.stdout + r.stderr)


def make_font():
    """ui/font5x7.h from the homebrew SDK font table (single source of truth for now)"""
    dst = os.path.join(HERE, 'ui', 'font5x7.h')
    if os.path.exists(dst):
        return
    src = open(os.path.join(HERE, '..', 'lisong_rebuild', 'homebrew', 'sdk', 'gfx.c')).read()
    a = src.index('static const uint8_t font5x7[][5] = {')
    b = src.index('};', a) + 2
    open(dst, 'w').write('/* 5x7 font, ASCII 32..90, column bytes, LSB = top row */\n#ifndef FONT5X7_H\n'
                         '#define FONT5X7_H\n#include <stdint.h>\n' + src[a:b] + '\n#endif\n')


def api_list():
    return [l.strip() for l in open(os.path.join(HERE, 'core', 'api.txt')) if l.strip() and not l.startswith('#')]


INCS = []                                # include dirs (target dir, generated regs.inc)


def gen_regs_inc(tdir, out):
    """regs.h -> regs.inc (NAME = value) for the assembler: one source of truth for port names"""
    src = os.path.join(tdir, 'regs.h')
    if not os.path.exists(src):
        return
    lines = [';; generated from regs.h - do not edit']
    for ln in open(src):
        m = re.match(r'^#define\s+([A-Z][A-Z0-9_]*)\s+(\(?[0-9A-Za-z_| ]+\)?|0x[0-9A-Fa-f]+|[0-9]+)\s*(/\*.*)?$', ln)
        if m and 'SFR' not in m.group(1):          # (the include guard has no value)
            lines.append(f'{m.group(1)} = {m.group(2).strip()}')
    open(os.path.join(out, 'regs.inc'), 'w').write('\n'.join(lines) + '\n')


def map_ram(mapfile, below=None):
    """bytes in the RAM-only areas (data/bss), optionally only those placed below `below`"""
    n = 0
    for ln in open(mapfile, errors='replace'):
        m = re.match(r'^(_DATA|_INITIALIZED|_BSS|_HEAP)\s+([0-9A-F]{8})\s+([0-9A-F]{8})', ln)
        if m and (below is None or int(m.group(2), 16) < below):
            n += int(m.group(3), 16)
    return n


CACHE = os.path.join(HERE, 'out', 'cache')    # compiled .rel by content hash (survives `out/<target>` wipes)
JOBS = max(2, os.cpu_count() or 2)
_INC_RE = re.compile(r'^\s*(?:#\s*include|\.include)\s+"([^"]+)"', re.M)


def _deps_hash(path, h, seen):
    """hash a source and, recursively, every quoted include it pulls in (generated headers too)"""
    path = os.path.normpath(path)
    if path in seen:
        return
    seen.add(path)
    data = open(path, 'rb').read()
    h.update(path.encode() + b'\0' + data)
    for name in _INC_RE.findall(data.decode('latin-1')):
        for d in [os.path.dirname(path)] + INCS:
            p = os.path.join(d, name)
            if os.path.exists(p):
                _deps_hash(p, h, seen)
                break


def _compile_one(s, out, defs):
    rel = os.path.join(out, os.path.basename(os.path.dirname(s)) + '_' + os.path.basename(s).replace('.', '_') + '.rel')
    if s.endswith('.s'):
        cmd = [SDAS, '-g', '-l'] + [f'-I{d}' for d in INCS] + ['-o', rel, s]
    else:
        cmd = ([SDCC, '-mz80', '--reserve-regs-iy', '--opt-code-size', '--max-allocs-per-node', '20000']
               + [f'-I{d}' for d in INCS] + defs + ['-c', s, '-o', rel])
    h = hashlib.sha1(' '.join(cmd[:1] + cmd[1:]).replace(rel, '').encode())
    _deps_hash(s, h, set())
    cached = os.path.join(CACHE, h.hexdigest() + '.rel')
    if os.path.exists(cached):
        shutil.copyfile(cached, rel)
        return rel
    run(cmd, out)
    os.makedirs(CACHE, exist_ok=True)
    tmp = cached + '.%d.tmp' % os.getpid()
    shutil.copyfile(rel, tmp)
    os.replace(tmp, cached)
    return rel


def compile_all(srcs, out, defs):
    """SDCC is the slow part (2-5 s per file with --max-allocs-per-node 20000): unchanged files come
    from the content-hash cache, the rest compile in parallel"""
    with concurrent.futures.ThreadPoolExecutor(min(JOBS, 4)) as ex:
        return list(ex.map(lambda s: _compile_one(s, out, defs), srcs))


def link(rels, out, name, base, data=None, code2=None):
    extra = ['-b', f"_DATA=0x{data:04x}"] if data is not None else []
    if code2 is not None:
        extra += ['-b', f"_CODE2=0x{code2:04x}"]
    run([SDLD, '-n', '-m', '-i', name + '.ihx', '-b', f"_CODE=0x{base:04x}"] + extra +
        ['-k', SDCC_LIB, '-l', 'z80'] + rels, out)
    mem = parse_ihx(open(os.path.join(out, name + '.ihx')).read())
    if code2 is not None:                        # second code section: kept apart (build_module)
        mem = {a: v for a, v in mem.items() if a >= base}
    fend = max(mem) + 1
    end = fend
    syms = {}
    for ln in open(os.path.join(out, name + '.map'), errors='replace'):
        m = re.match(r'^(_DATA|_INITIALIZED|_BSS|_HEAP)\s+([0-9A-F]{8})\s+([0-9A-F]{8})', ln)
        if m:
            end = max(end, int(m.group(2), 16) + int(m.group(3), 16))
        for a, n in re.findall(r'\b([0-9A-F]{8})\s+([A-Za-z_$.][\w$.]*)', ln):
            syms.setdefault(n, int(a, 16))
    if min(mem) != base:
        sys.exit(f"{name}: image starts at {min(mem):#06x}, expected {base:#06x}")
    return mem, fend, end, syms


def gen_strings(mdir, mout):
    """modules/<m>/strings.txt  'ID | english | spanish'  ->  <mout>/strings.h: enum S_ID + const tables
    (in the module's code, nothing copied to RAM); T(S_ID) picks the language set in SETTINGS."""
    src = os.path.join(mdir, 'strings.txt')
    if not os.path.exists(src):
        return []
    rows = []
    for ln in open(src, encoding='utf-8'):
        if ln.strip() and not ln.lstrip().startswith('#'):
            f = [x.strip() for x in ln.rstrip('\n').split('|')]
            if len(f) != 3:
                sys.exit(f"{src}: need 'ID | EN | ES': {ln.strip()}")
            if max(len(f[1]), len(f[2])) > 20:             # 128 px - 4 px margin, 6 px per char
                sys.exit(f"{src}: {f[0]}: longer than a screen line (20 chars)")
            rows.append(f)
    cfgh = os.path.join(HERE, 'core', 'config.h').replace('\\', '/')
    h = ['/* generated from strings.txt - do not edit */', '#ifndef ZOS_STRINGS_H', '#define ZOS_STRINGS_H',
         f'#include "{cfgh}"', 'enum { ' + ', '.join('S_' + r[0] for r in rows) + ', S_COUNT };',
         '/* several source files: one defines ZS_PROVIDE (holds the table + zs_get), the others',
         ' * ZS_EXTERN (call zs_get) - otherwise every file gets its own copy of the table */',
         '#ifdef ZS_EXTERN', 'const char *zs_get(uint8_t id);', '#define T(id) zs_get(id)', '#else',
         'static const char *const zs_tab[NLANG][S_COUNT] = {']
    for col in (1, 2):
        h.append('    { ' + ', '.join(json.dumps(r[col]) for r in rows) + ' },')
    h += ['};', '#define T(id) (zs_tab[ZCFG_LANG][id])',
          '#ifdef ZS_PROVIDE', 'const char *zs_get(uint8_t id) { return T(id); }', '#endif',
          '#endif', '#endif', '']
    open(os.path.join(mout, 'strings.h'), 'w').write('\n'.join(h))
    return [f'-I{mout}']


def read_bmp(path):
    """uncompressed BMP (1/4/8-bit palette, 24-bit, 32-bit) -> rows[y][x] = (r, g, b), top row first"""
    d = open(path, 'rb').read()
    if d[:2] != b'BM':
        sys.exit(f"{path}: not a BMP file")
    off, hsz = struct.unpack_from('<II', d, 10)
    w, h, _, bpp, comp = struct.unpack_from('<iiHHI', d, 18)
    ncol, = struct.unpack_from('<I', d, 46)
    if comp not in (0, 3) or bpp not in (1, 4, 8, 24, 32) or (comp == 3 and bpp != 32):
        sys.exit(f"{path}: unsupported BMP ({bpp} bpp, compression {comp}): save it uncompressed")
    pal = []
    if bpp <= 8:
        pal = [(d[o + 2], d[o + 1], d[o]) for o in range(14 + hsz, 14 + hsz + 4 * (ncol or 1 << bpp), 4)]
    stride = (w * bpp + 31) // 32 * 4
    rows = []
    for y in range(abs(h)):
        o = off + (abs(h) - 1 - y if h > 0 else y) * stride      # h > 0: stored bottom-up
        if bpp >= 24:
            n = bpp // 8
            rows.append([(d[o + x * n + 2], d[o + x * n + 1], d[o + x * n]) for x in range(w)])
        else:
            per = 8 // bpp
            rows.append([pal[(d[o + x // per] >> (8 - bpp * (x % per + 1))) & ((1 << bpp) - 1)]
                         for x in range(w)])
    return w, abs(h), rows


def gen_icons(out):
    """icons/<NAME>.bmp (16x16) -> out/icons.h, 1 bit per pixel, MSB left (icons/README.md).
    A pixel is set when it is dark: r + g + b < 384 (black on white; white or magenta = background).
    NAME = a module (SNAKE) or a SHELL category (MUSIC, GAMES, ...); DEFAULT for anything else."""
    ent = []
    for p in sorted(glob.glob(os.path.join(HERE, 'icons', '*.bmp'))):
        w, h, rows = read_bmp(p)
        if (w, h) != (16, 16):
            sys.exit(f"{p}: icons are 16x16 (this one is {w}x{h})")
        bits = []
        for r in rows:
            v = sum(1 << (15 - i) for i, c in enumerate(r) if sum(c) < 384)
            bits += [v >> 8, v & 0xFF]
        name = os.path.splitext(os.path.basename(p))[0].upper()[:8]
        ent.append(f'    {{ "{name}", {{ ' + ', '.join(f'0x{b:02x}' for b in bits) + ' } },')
    open(os.path.join(out, 'icons.h'), 'w').write(
        '/* generated from icons/*.bmp - do not edit */\n#include <stdint.h>\n'
        'typedef struct { char name[9]; uint8_t bits[32]; } zicon_t;\n'
        f'#define NZICONS {len(ent)}\nstatic const zicon_t zicons[NZICONS] = {{\n' + '\n'.join(ent) + '\n};\n')


def build_module(mdir, out, cfg, api, mod_crt0_rel, api_rel):
    name = os.path.basename(mdir).upper()[:8]
    mout = os.path.join(out, 'mod_' + name.lower())
    os.makedirs(mout)
    srcs = sorted(glob.glob(os.path.join(mdir, '*.c')) + glob.glob(os.path.join(mdir, '*.s')))
    rels = [mod_crt0_rel] + compile_all(srcs, mout, gen_strings(mdir, mout)) + [api_rel]
    z2 = os.path.exists(os.path.join(mdir, 'zram2'))      # marker file: ZRAM2 module (MOD_F_ZRAM2)
    if z2:
        # optional second code section _CODE2 (#pragma codeseg CODE2): linked at code2[0] in ZRAM1,
        # stored in the .ZM at offset zram2_size (past the page image), loaded by the module itself
        # ZRAM1 block modram: the variables first, _CODE2 right behind them (16-byte aligned): link once
        # to learn where the variables end, then again with _CODE2 there
        def data_end():
            e = cfg['modram'][0]
            for ln in open(os.path.join(mout, name.lower() + '.map'), errors='replace'):
                m = re.match(r'^(_DATA|_INITIALIZED|_BSS|_HEAP)\s+([0-9A-F]{8})\s+([0-9A-F]{8})', ln)
                if m and int(m.group(2), 16) < cfg['mod_base']:
                    e = max(e, int(m.group(2), 16) + int(m.group(3), 16))
            return e
        link(rels, mout, name.lower(), cfg['mod_base'], cfg['modram'][0], cfg['modram'][0])
        dend = data_end()
        c2 = ((dend + 15) & ~15, cfg['modram'][1])
        mem, fend, end, syms = link(rels, mout, name.lower(), cfg['mod_base'], cfg['modram'][0], c2[0])
        if data_end() != dend:
            sys.exit(f"module {name}: variables moved between the two links")
        full = parse_ihx(open(os.path.join(mout, name.lower() + '.ihx')).read())
        m = re.search(r'^_CODE2\s+[0-9A-F]{8}\s+([0-9A-F]{8})', open(os.path.join(mout, name.lower() + '.map'),
                      errors='replace').read(), re.M)
        n2 = int(m.group(1), 16) if m else 0                # the linker's size, not the ihx extent
        if c2[0] + n2 > c2[1]:
            sys.exit(f"module {name} variables + _CODE2 too large: {dend - cfg['modram'][0]} + {n2} B "
                     f"> {cfg['modram'][1] - cfg['modram'][0]}")
        code2 = bytes(full.get(a, 0) for a in range(c2[0], c2[0] + n2))
        if fend > cfg['mod_base'] + cfg['zram2_size']:
            sys.exit(f"module {name} code too large for ZRAM2: ends at {fend:#06x}")
        end = fend                                         # header 'end' = code end (in the page)
    else:
        mem, fend, end, syms = link(rels, mout, name.lower(), cfg['mod_base'])
        if end > cfg['mod_limit']:
            sys.exit(f"module {name} too large: ends at {end:#06x}")
    img = bytearray(mem.get(a, 0) for a in range(cfg['mod_base'], fend))
    assert img[0:2] == b'ZM'
    if z2:
        img[2] |= 0x80
    # API level = highest API entry the module really references (+1), not the length of the list:
    # appending entries to core/api.txt must not make a core in flash refuse modules that do not use
    # them (native boot: the device was left with every module saying "NEWER API")
    used = set()
    for r in rels[:-1]:
        for ln in open(r, errors='replace'):
            if ln.startswith('S _') and ' Ref' in ln:
                used.add(ln.split()[1][1:])
    img[3] = max([api.index(n) + 1 for n in used if n in api] + [0])
    img[4:6] = len(img).to_bytes(2, 'little')
    img[6:8] = end.to_bytes(2, 'little')
    img[12:28] = name.encode().ljust(16, b'\0')
    if z2 and code2:
        img_file = bytes(img).ljust(cfg['zram2_size'], b'\x00') + code2
    else:
        img_file = bytes(img)
    open(os.path.join(out, name + '.ZM'), 'wb').write(img_file)
    ram = map_ram(os.path.join(mout, name.lower() + '.map'))
    if z2:
        where = (f"ZRAM2 code {len(img):5d} B ({cfg['zram2_size'] - len(img)} free) + code2 {len(code2)} B "
                 f"({c2[1] - c2[0] - len(code2)} free) | "
                 f"vars {ram:5d} B at {cfg['modram'][0]:#06x}, code2 at {c2[0]:#06x}")
    else:
        where = (f"page  code {len(img):5d} B | RAM {ram:5d} B | page used {end - cfg['mod_base']:5d} of "
                 f"{cfg['mod_limit'] - cfg['mod_base']}")
    return f"  {name:<8} {where}"


def main():
    tgt = sys.argv[sys.argv.index('--target') + 1] if '--target' in sys.argv else 'atj2085'
    cfg = TARGETS[tgt]
    sys.path.insert(0, os.path.join(HERE, 'tools'))
    import check_sfr                     # hardware registers live in regs.h/sfr.h only (README: Hardware registers)
    bad = check_sfr.check(HERE)
    if bad:
        sys.exit('check_sfr: SFR()/__sfr declared outside targets/<target>/regs.h + sfr.h (use the register names from regs.h, see README.md "Hardware registers"):\n'
                 + '\n'.join(f'  {p}:{n}: {ln}' for p, n, ln in bad))
    make_font()
    out = os.path.join(HERE, 'out', tgt)
    if os.path.exists(out):              # empty it (the directory itself may be some shell's cwd)
        for f in os.listdir(out):
            p = os.path.join(out, f)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    os.makedirs(out, exist_ok=True)
    tdir = os.path.join(HERE, 'targets', tgt)
    INCS[:] = [tdir, out]
    gen_regs_inc(tdir, out)
    import datetime                      # build date/time = the clock's last-resort start (SHELL, rtc.c)
    bt = (datetime.datetime.strptime(os.environ['ZOS_BUILD_TIME'], '%Y-%m-%dT%H:%M')   # reproducible builds
          if os.environ.get('ZOS_BUILD_TIME') else datetime.datetime.now())
    open(os.path.join(out, 'buildtime.h'), 'w').write(
        '/* generated by build.py - do not edit: the build time (host local time) */\n'
        f'#define BUILD_DAY {(bt.date() - datetime.date(2000, 1, 1)).days}   /* days since 2000-01-01 */\n'
        f'#define BUILD_HOUR {bt.hour}\n#define BUILD_MIN {bt.minute}\n')
    api = api_list()
    # jump table (core side) and absolute API symbols (module side)
    jtab = os.path.join(out, 'jtab.s')
    open(jtab, 'w').write(';; generated from core/api.txt - do not edit\n\t.module jtab\n\t.area\t_CODE\n_zos_api::\n'
                          + ''.join(f'\tjp\t_{n}\n' for n in api))
    apis = os.path.join(out, 'api.s')
    open(apis, 'w').write(';; generated from core/api.txt - do not edit\n\t.module api\n'
                          + ''.join(f'_{n} == 0x{JTAB + 3 * i:04x}\n\t.globl _{n}\n' for i, n in enumerate(api)))
    tdir = os.path.join(HERE, 'targets', tgt)
    srcs = [os.path.join(tdir, 'crt0.s'), jtab]
    skip = {os.path.join(tdir, 'mod_crt0.s')}
    for d in (tdir, os.path.join(HERE, 'core'), os.path.join(HERE, 'ui'), os.path.join(HERE, 'apps'), HERE):
        for f in sorted(glob.glob(os.path.join(d, '*.c')) + glob.glob(os.path.join(d, '*.s'))):
            if f not in srcs and f not in skip:
                srcs.append(f)
    rels = compile_all(srcs, out, [f'-DZOS_API_COUNT={len(api)}'])
    mem, fend, end, syms = link(rels, out, 'zos', cfg['load'])
    if syms.get('_zos_api') != JTAB:
        sys.exit(f"jump table at {syms.get('_zos_api')}, expected {JTAB:#06x}")
    # loaded image (code + initializers) must fit the launcher; variables are cleared at start-up
    # and may extend over stock memory up to ram_limit (takeover mode, see targets/*/start.s)
    if fend > cfg['limit']:
        sys.exit(f"zos image too large: ends at {fend:#06x} > {cfg['limit']:#06x}")
    if end > cfg['ram_limit']:
        sys.exit(f"zos RAM too large: ends at {end:#06x} > {cfg['ram_limit']:#06x}")
    text = bytes(mem.get(a, 0) for a in range(cfg['load'], fend))
    ap = dict(pad=0x76, entry=cfg['load'], entry_bank=0, nbanks=0, bss_len=0, bss_addr=0, size=None,
              segs=[dict(name='text', bank=None, off=0x800, addr=cfg['load'], bytes=text)])
    open(os.path.join(out, 'ZOS.AP'), 'wb').write(aptool.build(ap))
    ram = map_ram(os.path.join(out, 'zos.map'))
    print(f"ZOS      image {fend - cfg['load']:5d} B ({cfg['limit'] - fend} free to {cfg['limit']:#06x}) | "
          f"RAM {ram:5d} B, ends {end:#06x} ({cfg['ram_limit'] - end} free to {cfg['ram_limit']:#06x}) | "
          f"{len(api)} API entries")
    # modules
    mod_crt0_rel, api_rel = compile_all([os.path.join(tdir, 'mod_crt0.s'), apis], out, [])
    gen_icons(out)
    failed = []
    mdirs = [m for m in sorted(glob.glob(os.path.join(HERE, 'modules', '*')))
             if os.path.isdir(m) and glob.glob(os.path.join(m, '*.[cs]'))
             and os.path.basename(m) != 'common']      # common/ = shared module sources

    def one(mdir):                       # one broken module must not stop the others
        try:
            return build_module(mdir, out, cfg, api, mod_crt0_rel, api_rel), None
        except SystemExit as e:
            return None, str(e)
    with concurrent.futures.ThreadPoolExecutor(JOBS) as ex:  # modules in parallel, report in order
        for mdir, (line, err) in zip(mdirs, ex.map(one, mdirs)):
            if err is None:
                print(line)
            else:
                failed.append(os.path.basename(mdir))
                print(f"  module {os.path.basename(mdir).upper()}: FAILED\n    " + err.replace('\n', '\n    '))
    skin = sys.argv[sys.argv.index('--skin') + 1] if '--skin' in sys.argv else 'ilike'
    sdir = os.path.join(HERE, 'skins', skin)
    if os.path.isdir(sdir):                      # Rockbox theme -> SKIN\ (tools/rbskin.py)
        run([sys.executable, os.path.join(HERE, 'tools', 'rbskin.py'), sdir, os.path.join(out, 'SKIN')], HERE)
        print(f"  skin {skin}: {len(os.listdir(os.path.join(out, 'SKIN')))} files -> SKIN\\")
    make_cfg(out)
    got = fw_extract(out)
    if got:
        print(f"  from {os.path.basename(FW_IMAGE)}: {', '.join(got)}")
    if failed:
        sys.exit('failed modules: ' + ', '.join(failed) + ' (not deployed)')
    if '--no-deploy' not in sys.argv:
        deploy(out, sys.argv[sys.argv.index('--drive') + 1] if '--drive' in sys.argv else 'G:')


FW_IMAGE = os.path.join(HERE, '..', 'my-ximpro.fw')    # the player's own firmware (stock DSP programs)
FW_FILES = ['MP3DEC.DSP', 'OGGDEC.DSP', 'PCMDEC.DSP', 'ADENCM.DSP']   # copied next to ZOS.AP (disk root)


def fw_extract(out):
    """take FW_FILES from the stock firmware image (fwtool: AFI -> FWIMAGE.FW directory)"""
    import fwtool
    if not os.path.exists(FW_IMAGE):
        print(f"  (no {FW_IMAGE}: {', '.join(FW_FILES)} not extracted)")
        return []
    d = open(FW_IMAGE, 'rb').read()
    got = []
    for e in fwtool.afi_parse(d):
        if e['name'] != 'FWIMAGE.FW':
            continue
        blob = d[e['off']:e['off'] + e['len']]
        for fe in fwtool.fw_parse(blob):
            if fe['name'].upper() in FW_FILES:
                open(os.path.join(out, fe['name'].upper()), 'wb').write(
                    blob[fe['sector'] * 512:fe['sector'] * 512 + fe['len']])
                got.append(fe['name'].upper())
    return got


ZCFG_SIZE = 512                               # core/config.h ZCFG_FILE_SIZE


def make_cfg(out):
    """default ZOS.CFG (modules/common/cfg.c format): fixed size, rewritten in place by zos"""
    text = ('LANG=EN\r\nCONTRAST=6\r\nBACKLIGHT=0\r\nBATTERY=1\r\nVOLUME=20\r\n'
            'RESDIR=0\r\nRESFILE=0\r\nRESPOS=0\r\n')
    open(os.path.join(out, 'ZOS.CFG'), 'wb').write(text.encode().ljust(ZCFG_SIZE, b' '))


def deploy(out, drive):
    """copy changed files to the player's USB disk, flush the volume and eject it (Windows).
    Only touches a drive that already holds ZOS.AP in its root (= the player's disk)."""
    root = drive.rstrip('\\/') + '\\'
    if not os.path.exists(os.path.join(root, 'ZOS.AP')):
        print(f"deploy: {root} not present or no ZOS.AP on it - skipped")
        return
    files = [(os.path.join(out, f), f) for f in sorted(os.listdir(out))
             if f.upper().endswith(('.AP', '.ZM', '.DSP'))]
    skin = os.path.join(out, 'SKIN')
    if os.path.isdir(skin):
        files += [(os.path.join(skin, f), os.path.join('SKIN', f)) for f in sorted(os.listdir(skin))]
    changed = []
    cfg = os.path.join(root, 'ZOS.CFG')       # the player's saved settings: only create, never replace
    if not os.path.exists(cfg) or os.path.getsize(cfg) != ZCFG_SIZE:
        shutil.copyfile(os.path.join(out, 'ZOS.CFG'), cfg)
        changed.append('ZOS.CFG (new)')
    for src, rel in files:
        dst = os.path.join(root, rel)
        new = open(src, 'rb').read()
        if os.path.exists(dst) and open(dst, 'rb').read() == new:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(new)
        changed.append(rel)
    print(f"deploy: {len(changed)} of {len(files)} files changed on {root}" +
          (': ' + ', '.join(changed) if changed else ''))
    letter = root[0]
    ps = (f"Write-VolumeCache -DriveLetter {letter}; "
          f"(New-Object -ComObject Shell.Application).Namespace(17).ParseName('{letter}:').InvokeVerb('Eject')")
    r = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True)
    print(f"deploy: {letter}: flushed and ejected" if r.returncode == 0 else f"deploy: flush/eject failed: {r.stderr.strip()}")


def locked(fn):
    """serialize concurrent builds (several agents/shells share out/)"""
    import time
    os.makedirs(os.path.join(HERE, 'out'), exist_ok=True)
    with open(os.path.join(HERE, 'out', '.buildlock'), 'a+b') as f:
        if os.name == 'nt':
            import msvcrt
            while True:
                try:
                    f.seek(0)
                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.5)
            try:
                fn()
            finally:
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX)
            fn()


if __name__ == '__main__':
    locked(main)
