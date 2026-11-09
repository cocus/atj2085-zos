import sys,os; sys.path.insert(0,os.path.dirname(__file__))
from z80dis import decode
from asm_util import assemble
pats=[]
for p in [[],[0xcb],[0xed],[0xdd],[0xfd],[0xdd,0xcb,0x85],[0xfd,0xcb,0xfb]]:
    for op in range(256):
        pats.append(bytes(p+[op,0x34,0x12,0x56]))
lines=['.area CODE (ABS)']; exp=[]; addr=0x4000
for pat in pats:
    ins=decode(pat,addr,addr)
    if ins.kind=='bad': continue
    lines.append(f' .org 0x{addr:04x}'); lines.append(' '+ins.text)
    exp.append((addr,pat[:ins.size],ins.text)); addr+=8
mem=assemble('\n'.join(lines)+'\n',name='rt')
bad=0
for a,want,t in exp:
    got=bytes(mem.get(a+k,0xEE) for k in range(len(want)))
    if got!=want: bad+=1; print('MISMATCH',t,want.hex(),got.hex()) if bad<30 else None
print(len(exp),'insns tested,',bad,'mismatches')
