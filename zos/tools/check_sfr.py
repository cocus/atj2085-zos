"""Guard: hardware registers are declared ONLY in targets/<target>/regs.h + sfr.h.

usage: python check_sfr.py [zos dir]      (build.py runs it first; exit status 1 = violation)

Fails if any C/header source under zos/ outside targets/*/regs.h and targets/*/sfr.h contains an
SFR(...) declaration or the __sfr keyword (comments are ignored). attic/ (retired code, not built)
and out/ are skipped. Use the register names from regs.h/sfr.h instead (P_PAVOL -> PAVOL); to add a
port, add `#define P_xxx 0x..` to regs.h and one SFR line to sfr.h.
"""
import os, re, sys

SKIP_DIRS = {'attic', 'out', 'brec_src', '__pycache__'}
ALLOWED = {'regs.h', 'sfr.h'}
BAD = re.compile(r'\bSFR\s*\(|\b__sfr\b')
COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


def check(zdir):
    bad = []
    for root, dirs, files in os.walk(zdir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if not f.endswith(('.c', '.h', '.inc')):
                continue
            if f in ALLOWED and os.sep + 'targets' + os.sep in root + os.sep:
                continue
            p = os.path.join(root, f)
            text = open(p, encoding='latin-1').read()
            # blank the comments but keep the newlines so the reported line numbers are right
            text = COMMENT.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)
            for n, ln in enumerate(text.split('\n'), 1):
                if BAD.search(ln):
                    bad.append((os.path.relpath(p, zdir), n, ln.strip()))
    return bad


if __name__ == '__main__':
    zdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    bad = check(zdir)
    if bad:
        print('check_sfr: hardware registers must not be declared outside targets/<target>/regs.h + sfr.h:')
        for p, n, ln in bad:
            print(f'  {p}:{n}: {ln}')
        print('  -> #include "regs.h" and use the canonical register names (P_PAVOL -> PAVOL, P_USB_STAT -> USB_STAT);\n'
              '     a missing port is added in targets/<target>/regs.h (P_xxx) and sfr.h (one SFR line). See README.md.')
        sys.exit(1)
