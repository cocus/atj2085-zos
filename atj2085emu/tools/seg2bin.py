"""seg2bin.py SEG.s OUT.bin -> raw bytes of an apdis segment (from .db values + '; addr: hex' comments).
Prints the load address."""
import re, sys
org = None; out = {}
for line in open(sys.argv[1]):
    m = re.match(r'\s*\.org\s+0x([0-9a-f]+)', line)
    if m: org = int(m.group(1), 16); continue
    m = re.match(r'\s*\.db\s+([^;]+);\s*([0-9a-f]{4})', line)
    if m:
        a = int(m.group(2), 16)
        for i, v in enumerate(x.strip() for x in m.group(1).split(',')):
            out[a + i] = int(v, 16)
        continue
    m = re.search(r';\s*([0-9a-f]{4}):\s*([0-9a-f]+)', line)
    if m and not line.strip().startswith(';'):
        a = int(m.group(1), 16); h = bytes.fromhex(m.group(2))
        for i, b in enumerate(h): out[a + i] = b
lo, hi = min(out), max(out)
buf = bytearray(hi - lo + 1)
for a, v in out.items(): buf[a - lo] = v
open(sys.argv[2], 'wb').write(buf)
print('%04x' % lo)
