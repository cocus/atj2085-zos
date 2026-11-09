"""Make a raw FAT image (superfloppy, no MBR) from a host folder, for atj2085emu --disk IMG.

usage: mkfatimg.py SRC_DIR OUT.img [--mb 300] [--fat 32] [--spc 8]
"""
import os, sys, argparse
import fs.osfs
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('out')
    ap.add_argument('--mb', type=int, default=300)
    ap.add_argument('--fat', type=int, default=32)
    ap.add_argument('--spc', type=int, default=8, help='sectors per cluster')
    a = ap.parse_args()
    with open(a.out, 'wb') as f:
        f.truncate(a.mb * 1024 * 1024)
    pf = PyFat()
    pf.mkfs(a.out, fat_type=a.fat, size=a.mb * 1024 * 1024, label='ZOSTEST')
    pf.close()
    dst = PyFatFS(a.out, read_only=False)
    for root, dirs, files in os.walk(a.src):
        rel = os.path.relpath(root, a.src).replace('\\', '/')
        base = '/' if rel == '.' else '/' + rel + '/'
        for d in dirs:
            dst.makedir(base + d)
        for fn in files:
            with open(os.path.join(root, fn), 'rb') as s, dst.openbin(base + fn, 'w') as o:
                o.write(s.read())
    dst.close()
    print('wrote', a.out)


if __name__ == '__main__':
    main()
