"""Build a homebrew ACTOS AP with SDCC and (optionally) a flashable firmware.

usage: python build.py APPDIR [--as GAME.AP] [--fw]      (env LISONG_PROFILE selects v04/v05; default v04)
  APPDIR    folder with *.c / *.s of the app (e.g. snake)
  --as      which AP of the firmware the app replaces (default GAME.AP)
  --fw      also build a complete firmware image: build_<profile>/homebrew-<app>.fw

Layout of a homebrew AP: one text segment loaded by the OS at 0x0800 (code, constants
and variables), no banks; entry 0x0800 = sdk/crt0.s:ap_entry. Max end 0x2B00 (the
largest stock AP layouts end at ~0x2B4C, the OS resident part starts at 0x2E00).
"""
import os, sys, glob, subprocess, shutil, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
os.environ.setdefault('LISONG_PROFILE', 'v04')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import aptool
from asm_util import SDAS, SDLD, parse_ihx
from profile import P, NAME

SDCC = r'C:\Program Files (x86)\SDCC\bin\sdcc.exe'
SDCC_LIB = r'C:\Program Files (x86)\SDCC\lib\z80'
LOAD = 0x0800
LIMIT = 0x2B00


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout + r.stderr


def build_ap(appdir):
    app = os.path.basename(os.path.normpath(appdir))
    out = os.path.join(HERE, 'out', app)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    sdk = os.path.join(HERE, 'sdk')
    rels = []
    # crt0 must be first: it defines the entry point and the area order
    srcs = [os.path.join(sdk, 'crt0.s')] + sorted(glob.glob(os.path.join(sdk, '*.s')) +
                                                 glob.glob(os.path.join(sdk, '*.c')) +
                                                 glob.glob(os.path.join(appdir, '*.s')) +
                                                 glob.glob(os.path.join(appdir, '*.c')))
    seen = set()
    for s in srcs:
        if s in seen:
            continue
        seen.add(s)
        base = os.path.splitext(os.path.basename(s))[0] + ('_s' if s.endswith('.s') else '')
        rel = os.path.join(out, base + '.rel')
        if s.endswith('.s'):
            run([SDAS, '-g', '-l', '-o', rel, s], out)
        else:
            run([SDCC, '-mz80', '--reserve-regs-iy', '--opt-code-size', '--max-allocs-per-node', '20000',
                 '-I', sdk, '-c', s, '-o', rel], out)
        rels.append(rel)
    run([SDLD, '-n', '-m', '-i', 'app.ihx', '-b', f'_CODE=0x{LOAD:04x}', '-k', SDCC_LIB, '-l', 'z80'] + rels, out)
    mem = parse_ihx(open(os.path.join(out, 'app.ihx')).read())
    lo, hi = min(mem), max(mem) + 1
    # RAM areas (_DATA/_BSS) are not in the ihx; take their end from the map
    end = hi
    for ln in open(os.path.join(out, 'app.map'), errors='replace'):
        p = ln.split()
        if len(p) >= 3 and p[0] in ('_DATA', '_INITIALIZED', '_BSS', '_HEAP', '_BSEG'):
            try:
                end = max(end, int(p[1], 16) + int(p[2], 16))
            except ValueError:
                pass
    if lo != LOAD:
        sys.exit(f'image starts at {lo:#06x}, expected {LOAD:#06x} (crt0 not first?)')
    if end > LIMIT:
        sys.exit(f'app too large: ends at {end:#06x} > {LIMIT:#06x}')
    text = bytes(mem.get(a, 0) for a in range(LOAD, end))
    ap = dict(pad=0x76, entry=LOAD, entry_bank=0, nbanks=0, bss_len=0, bss_addr=0, size=None,
              segs=[dict(name='text', bank=None, off=0x800, addr=LOAD, bytes=text)])
    apbytes = aptool.build(ap)
    apfile = os.path.join(out, app.upper() + '.AP')
    open(apfile, 'wb').write(apbytes)
    print(f'{app}: code+data {LOAD:#06x}-{end:#06x} ({end - LOAD} bytes, {LIMIT - end} free)  -> {apfile}')
    return apfile


def build_fw(apfile, target):
    app_ = os.path.splitext(os.path.basename(apfile))[0].lower()
    fwdir = os.path.join(P['build'], 'fw_homebrew_' + app_)   # per app: no shared dir to delete
    if os.path.exists(fwdir):
        shutil.rmtree(fwdir, ignore_errors=True)
    tools = os.path.join(ROOT, 'tools')
    run([sys.executable, '-I', os.path.join(tools, 'fwtool.py'), 'unpack', P['fw'], fwdir], ROOT)
    dst = os.path.join(fwdir, 'fwimage', target)
    if not os.path.exists(dst):
        sys.exit(f'{target} is not in the firmware')
    shutil.copy(apfile, dst)
    app = os.path.splitext(os.path.basename(apfile))[0].lower()
    out = os.path.join(P['build'], f'homebrew-{app}-{NAME}.fw')
    run([sys.executable, '-I', os.path.join(tools, 'fwtool.py'), 'pack', fwdir, out], ROOT)
    print(f'firmware ({NAME}) with {target} replaced -> {out}  md5 {hashlib.md5(open(out, "rb").read()).hexdigest()}')
    return out


if __name__ == '__main__':
    args = sys.argv[1:]
    appdir = os.path.join(HERE, args[0]) if not os.path.isabs(args[0]) else args[0]
    target = args[args.index('--as') + 1] if '--as' in args else 'GAME.AP'
    apf = build_ap(appdir)
    if '--fw' in args:
        build_fw(apf, target)
