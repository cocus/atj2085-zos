"""zdis.py FILE BASE START [COUNT] [FILEOFF] - linear disassembly helper (uses lisong_rebuild z80dis)."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'lisong_rebuild', 'tools'))
import z80dis
f, base, start = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
foff = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0
d = open(f, 'rb').read()[foff:]
pc = start
for _ in range(n):
    if pc - base >= len(d): break
    try:
        i = z80dis.decode(d, pc, base)
    except Exception as e:
        print(f'{pc:04x}: ?? {e}'); pc += 1; continue
    print(f'{pc:04x}: {d[pc-base:pc-base+i.size].hex():10s} {i.text}')
    pc += i.size
