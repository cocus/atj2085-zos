"""xref.py FILE BASE  - list call/jp targets outside the file's own range + all in/out port numbers."""
import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'lisong_rebuild', 'tools'))
import z80dis
f, base = sys.argv[1], int(sys.argv[2], 16)
d = open(f, 'rb').read()
ext = collections.Counter(); ports = collections.defaultdict(list)
pc = base
while pc - base < len(d):
    try:
        i = z80dis.decode(d, pc, base)
    except Exception:
        pc += 1; continue
    t = i.text
    if i.kind in ('call', 'jp', 'callcc', 'jpcc') and i.target is not None and not (base <= i.target < base + len(d)):
        ext[(i.kind, i.target)] += 1
    if t.startswith('in ') or t.startswith('out '):
        if '#0x' in t:
            p = t.split('#0x')[1][:2]; ports[p].append(pc)
    pc += max(1, i.size)
for (k, tgt), n in sorted(ext.items(), key=lambda x: x[0][1]):
    print(f'{k:6s} {tgt:04x} x{n}')
print('ports:', ' '.join(f'{p}({len(v)})' for p, v in sorted(ports.items())))
