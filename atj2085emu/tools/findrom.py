"""findrom.py ROOT : search files (<2MB) for byte patterns of known MROM helper routines."""
import os, sys
pats = {'add32 (BREC 0x3db)': bytes.fromhex('f57e83775f237e8a7757'),
        'shl32 (BREC 0x3c3)': bytes.fromhex('b7c8fe083809414c652e00'),
        'giveio/rom-ish': bytes.fromhex('b7c8fe0838')}
for dp, dn, fn in os.walk(sys.argv[1]):
    if '.venv' in dp or 'atj2085emu' in dp:
        continue
    for f in fn:
        p = os.path.join(dp, f)
        try:
            if os.path.getsize(p) > 2_000_000:
                continue
            d = open(p, 'rb').read()
        except Exception:
            continue
        for k, v in pats.items():
            i = d.find(v)
            if i >= 0:
                print('%-20s %6x %7d %s' % (k, i, len(d), p))
