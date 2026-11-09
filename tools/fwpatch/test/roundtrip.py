"""Patch -> verify -> unpatch round trip over firmware files (offline, no emulator).

usage: python test/roundtrip.py FW [FW ...]
For every input: patch (default options and --no-vbus-escape), check the result re-parses with
all checksums valid, unpatch and compare byte for byte. Unsupported inputs must be REFUSED
with a message (never produce output).
"""
import os, sys, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import zospatch as Z  # noqa: E402

stub = open(os.path.join(os.path.dirname(HERE), 'zosboot.bin'), 'rb').read()
for p in sys.argv[1:]:
    src = open(p, 'rb').read()
    name = os.path.basename(p)
    try:
        out = Z.patch(src, stub)
    except Z.PatchError as e:
        print('%-36s REFUSED: %s' % (name, str(e).splitlines()[0] + (' ...' if '\n' in str(e) else '')))
        continue
    a = Z.analyse(out)
    assert not a['errs'], a['errs']
    back = Z.unpatch(out)
    out2 = Z.patch(src, stub, Z.F_KEYS)
    assert Z.unpatch(out2) == src
    try:
        Z.patch(out, stub)
        raise SystemExit('patched file accepted twice!')
    except Z.PatchError:
        pass
    print('%-36s PATCHED %d -> %d B, unpatch %s, brecs: %s' % (
        name, len(src), len(out), 'byte-identical' if back == src else 'DIFFERENT',
        ', '.join('%s=%s' % (b['name'], b['status'].split(' ')[0]) for b in a['brecs'])))
