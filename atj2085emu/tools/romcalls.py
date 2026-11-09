"""romcalls.py DIR : list call/jp targets >= 0x8000 (internal MROM entry points) used by firmware files.
Byte-pattern scan (not a real disassembly) -> only targets used by >=2 files or by SYS/DRV are printed."""
import sys, os, glob, collections

c = collections.defaultdict(set)
for f in glob.glob(sys.argv[1] + '/*'):
    if f.upper().endswith(('.RES', '.$$$', '.DSP')):
        continue
    d = open(f, 'rb').read()
    for op in (0xcd, 0xc3):
        i = d.find(bytes([op]))
        while i >= 0:
            if i + 2 < len(d):
                t = d[i + 1] | d[i + 2] << 8
                if 0x8000 <= t:
                    c[t].add(os.path.basename(f))
            i = d.find(bytes([op]), i + 1)
for t, fs in sorted(c.items()):
    if len(fs) >= 3 or any(x.endswith(('.SYS', '.DRV')) for x in fs):
        print('%04x %d %s' % (t, len(fs), ' '.join(sorted(fs))[:150]))
