"""Emulator test matrix for a zospatch-patched firmware (development only; needs the repo's
atj2085emu, lisong_rebuild/tools/{nandinject,fwinject}.py and the NAND dump).

usage: emu_matrix.py PATCHED.fw NOVBUS.fw STOCK.fw ZOSDIR OUTDIR [--only a,b] [-j N]
  ZOSDIR  a zos install set (ZOS.AP, *.ZM, MP3DEC.DSP, SKIN\\, ...) as copied to the player
Cases (expected decision):
  stock_fw      STOCK.fw, zos files present            control: stock kernel
  nozos         PATCHED, no zos files on the disk       go_stock
  zos           PATCHED, zos files                      go_zos (zos SHELL on screen)
  menu          PATCHED, zos files, MENU held           go_stock
  vol           PATCHED, zos files, VOL held            go_stock
  vbus          PATCHED, zos files, USB at power-on     go_stock
  vbus_off      NOVBUS (--no-vbus-escape), USB          go_zos
  trunc         PATCHED, ZOS.AP cut to 6 KB             go_stock
  garbage       PATCHED, ZOS.AP = random bytes          go_stock
  badtext       PATCHED, valid header, text not `jp`    go_stock
  copyA         PATCHED written to copy A only (fresh s1res flash), zos files: go_zos after A->B copy
"""
import os, sys, json, shutil, subprocess, random, struct
from concurrent.futures import ThreadPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
PY = os.path.join(TOP, 'lisong_rebuild', '.venv', 'Scripts', 'python.exe')
TOOLS = os.path.join(TOP, 'lisong_rebuild', 'tools')
DUMP = os.path.join(TOP, 'readback-my-ximpro.s1clone')

EXPECT = dict(stock_fw='kernel', nozos='go_stock', zos='go_zos', menu='go_stock', vol='go_stock',
              vbus='go_stock', vbus_off='go_zos', trunc='go_stock', garbage='go_stock',
              badtext='go_stock', copyA='go_zos')


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout


def disk_overlay(out, zdir, mutate=None):
    """nandinject the zos set (optionally with a modified ZOS.AP) -> overlay"""
    tmp = out + '.files'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    files = []
    for f in sorted(os.listdir(zdir)):
        s = os.path.join(zdir, f)
        d = os.path.join(tmp, f)
        if os.path.isdir(s):
            shutil.copytree(s, d)
        else:
            data = open(s, 'rb').read()
            if f.upper() == 'ZOS.AP' and mutate:
                data = mutate(data)
            open(d, 'wb').write(data)
        files.append(d)
    run([PY, os.path.join(TOOLS, 'nandinject.py'), DUMP, out] + files)
    return out


def main():
    a = sys.argv[1:]
    only = None
    jobs = 4
    if '--only' in a:
        i = a.index('--only'); only = a[i + 1].split(','); del a[i:i + 2]
    if '-j' in a:
        i = a.index('-j'); jobs = int(a[i + 1]); del a[i:i + 2]
    patched, novbus, stock, zdir, out = [os.path.abspath(x) for x in a]
    os.makedirs(out, exist_ok=True)
    ovl = os.path.join(out, 'ovl')
    os.makedirs(ovl, exist_ok=True)
    rnd = random.Random(1)

    def trunc(d):
        return d[:0x1800]

    def garbage(d):
        return bytes(rnd.randrange(256) for _ in range(len(d)))

    def badtext(d):
        d = bytearray(d)
        d[0x800] = 0x00          # header valid, image does not start with jp
        return bytes(d)
    disks = {'none': None, 'zos': disk_overlay(os.path.join(ovl, 'd_zos.ovl'), zdir)}
    need = set(only or EXPECT)
    for nm, fn in (('trunc', trunc), ('garbage', garbage), ('badtext', badtext)):
        if nm in need:
            disks[nm] = disk_overlay(os.path.join(ovl, 'd_%s.ovl' % nm), zdir, fn)

    def fwovl(fw, disk, copies='AB'):
        name = os.path.join(ovl, 'f_%s_%s_%s.ovl' % (os.path.basename(fw)[:12], disk, copies))
        cmd = [PY, os.path.join(TOOLS, 'fwinject.py'), DUMP, name, fw, '--copies', copies]
        if disks[disk]:
            cmd += ['--merge', disks[disk]]
        run(cmd)
        return name
    HOLD = '200000'
    cases = dict(
        stock_fw=(stock, fwovl(stock, 'zos') if 'stock_fw' in need else None, [], 260),
        nozos=(patched, fwovl(patched, 'none') if 'nozos' in need else None, [], 260),
        zos=(patched, fwovl(patched, 'zos'), [], 200),
        menu=(patched, None, ['--keys', '0:MENU:' + HOLD], 260),
        vol=(patched, None, ['--keys', '0:K0D:' + HOLD], 260),
        vbus=(patched, None, ['--usb'], 260),
        vbus_off=(novbus, fwovl(novbus, 'zos') if 'vbus_off' in need else None, ['--usb'], 200),
        trunc=(patched, fwovl(patched, 'trunc') if 'trunc' in need else None, [], 260),
        garbage=(patched, fwovl(patched, 'garbage') if 'garbage' in need else None, [], 260),
        badtext=(patched, fwovl(patched, 'badtext') if 'badtext' in need else None, [], 260),
        copyA=(patched, fwovl(patched, 'zos', 'A') if 'copyA' in need else None, [], 400),
    )
    zos_ovl = cases['zos'][1]
    todo = [c for c in EXPECT if not only or c in only]

    def one(c):
        fw, o, extra, mt = cases[c]
        o = o or zos_ovl
        d = os.path.join(out, c)
        r = subprocess.run([PY, os.path.join(HERE, 'emu_run.py'), fw, d, '--overlay', o, '--ticks', str(mt * 1000000),
                            '--shots', '%d:%s.png' % (mt * 1000000 - 200000, c),
                            '--zosap', os.path.join(zdir, 'ZOS.AP')] + extra, capture_output=True, text=True)
        if r.returncode:
            return c, dict(error=r.stderr[-2000:])
        return c, json.load(open(os.path.join(d, 'result.json')))
    with ThreadPoolExecutor(jobs) as ex:
        results = dict(ex.map(one, todo))
    ok_all = True
    rows = []
    for c in todo:
        r = results[c]
        if 'error' in r:
            got = 'ERROR'
        elif c == 'stock_fw':
            got = 'kernel' if 'kernel_entry' in r['milestones'] else 'none'
        else:
            got = r['decision']
            if got == 'go_stock' and 'kernel_entry' not in r['milestones']:
                got = 'go_stock(no kernel!)'
            if got == 'go_zos' and r.get('image_ok') is not True:
                got = 'go_zos(image mismatch!)'
            if got == 'go_zos' and 'zos_entry' not in r['milestones']:
                got = 'go_zos(no zos entry!)'
        ok = got == EXPECT[c]
        ok_all &= ok
        rows.append(dict(case=c, expect=EXPECT[c], got=got, ok=ok, ms=r.get('milestones'), sums=r.get('sums'),
                         err=r.get('error')))
        print('%-9s expect %-9s got %-22s %s  %s' % (c, EXPECT[c], got, 'PASS' if ok else 'FAIL',
                                                  json.dumps(r.get('milestones', r.get('error')))))
    json.dump(rows, open(os.path.join(out, 'matrix.json'), 'w'), indent=1)
    sys.exit(0 if ok_all else 1)


if __name__ == '__main__':
    main()
