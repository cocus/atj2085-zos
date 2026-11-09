import sys
d=open(sys.argv[1],'rb').read(); base=int(sys.argv[2],16)
for pat in sys.argv[3:]:
    p=bytes.fromhex(pat); i=d.find(p)
    while i>=0:
        print(pat, hex(base+i)); i=d.find(p,i+1)
