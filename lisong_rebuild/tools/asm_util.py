"""Assemble sdasz80 source to a flat binary at an absolute org (via sdasz80 + sdldz80 ihx)."""
import subprocess,os,tempfile
SDAS=r'C:\Program Files (x86)\SDCC\bin\sdasz80.exe'
SDLD=r'C:\Program Files (x86)\SDCC\bin\sdldz80.exe'
def parse_ihx(txt):
    mem={}
    for line in txt.splitlines():
        if not line.startswith(':'): continue
        r=bytes.fromhex(line[1:]); n,addr,typ=r[0],(r[1]<<8)|r[2],r[3]
        if typ==0:
            for k in range(n): mem[addr+k]=r[4+k]
    return mem
def assemble(src, workdir=None, name='t'):
    wd=workdir or tempfile.mkdtemp()
    p=os.path.join(wd,name)
    open(p+'.s','w').write(src)
    r=subprocess.run([SDAS,'-g','-l','-o',p+'.rel',p+'.s'],capture_output=True,text=True)
    if r.returncode: raise RuntimeError(r.stdout+r.stderr)
    r=subprocess.run([SDLD,'-i',p+'.ihx',p+'.rel'],capture_output=True,text=True)
    if r.returncode: raise RuntimeError(r.stdout+r.stderr)
    return parse_ihx(open(p+'.ihx').read())
