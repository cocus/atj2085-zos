#!/usr/bin/env python3
"""zospatch - add the zos boot selector to a stock ATJ2085 (s1mp3-class) firmware .fw, safely.

The patched firmware boots the UNCHANGED stock OS by default. zos starts only when its core
file (ZOS.AP) is present in the root directory of the player's own USB disk, is intact, and no
escape is active (MENU/VOL held at power-on; USB cable connected, unless --no-vbus-escape).
Install zos = copy files over USB; uninstall = delete ZOS.AP with the stock USB disk.

usage:
  zospatch.py STOCK.fw -o OUT.fw [--no-vbus-escape] [--zos-file NAME.EXT] [--stub zosboot.bin]
                                 [--repatch]
  zospatch.py --check FW [FW ...]           show what is in the firmware, verify every checksum
  zospatch.py --unpatch PATCHED.fw -o OUT.fw   restore the original firmware byte for byte

Pure Python 3 standard library. Contains no stock code or data: the stub (zosboot.bin) is our own
code, and the boot record is only recognised by 16-bit sums of its code ranges.
See README.md.
"""
import os, sys, struct, hashlib, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_VERSION = 1

# ------------------------------------------------------------------------------------- constants
FW_MAGIC = b'\x55\xaa\xf0\x0f'
AFI_MAGIC = b'AFI\0'
SYS_NAME = b'SYSCFG  SYS'
STK_NAME = b'SYSCFG  STK'
FWIMAGE_NAME = b'FWIMAGE FW '
SYS_LEN = 0x3800                  # stock kernel size; BREC loads file 0x0000-0x2CFF and 0x3700-0x377F
COPY_B = 0x800000                 # BREC keeps a second FWIMAGE copy at 8 MB: the image must fit below
DIR_OFF, DIR_END = 0x200, 0x2000  # directory: 32-byte entries, end = first entry with name[0] == 0
UNDO_OFF = 0x2D00                 # undo record inside the new SYSCFG.SYS (never loaded by BREC)
UNDO_MAGIC = b'ZOSPUNDO'
UNDO_LEN = 0x500                  # (file 0x2D00-0x31FF)
PARAM_OFF = 3                     # zosboot.bin parameter block (see stub/zosboot.s)
DEFAULT_VOLX = 0x01040000         # user disk sector 0x200 (FAT boot sector) in BREC reader space
F_VBUS, F_KEYS = 0x01, 0x02

# Boot-record recognition: 16-bit byte sums over code ranges (the same ranges the stub sums in RAM
# at run time). Values of the Actions "F641" boot record (BRECF641.BIN, LSD_501/504 and relatives).
LOADER_RANGES = [(0x020C, 0x0224), (0x0226, 0x0230), (0x0232, 0x0274), (0x02C9, 0x02D7), (0x02D9, 0x0324),
                 (0x0326, 0x0349), (0x034B, 0x036E), (0x0370, 0x037B)]   # helper-call operands excluded
READER_RANGES = [(0x3CAF, 0x3D34), (0x3D3A, 0x3DB1), (0x3DC9, 0x3F40), (0x3F69, 0x3FCE), (0x3FDB, 0x3FDE)]
KNOWN_LOADER_SIG = 0x6028
KNOWN_READER_SIG = 0x33A7
NAMECMP = 0x0274                  # 11 x `cp #c` (FE c) with stride 8 = "SYSCFG  SYS"


class PatchError(Exception):
    pass


# ------------------------------------------------------------------------------------- checksums
def cs32(b):
    b = bytes(b) + b'\0' * (-len(b) % 4)
    return sum(struct.unpack('<%dI' % (len(b) // 4), b)) & 0xFFFFFFFF


def cs16(b):
    b = bytes(b) + b'\0' * (-len(b) % 2)
    return sum(struct.unpack('<%dH' % (len(b) // 2), b)) & 0xFFFF


def sum8_16(b, ranges):
    return sum(sum(b[lo:hi]) for lo, hi in ranges) & 0xFFFF


# ------------------------------------------------------------------------------------- AFI
def afi_parts(afi):
    if afi[:4] != AFI_MAGIC:
        raise PatchError('not an AFI firmware file (no "AFI" magic) - give the .fw that s1res flashes')
    parts = []
    for i in range(0x20, 0x200, 0x20):
        e = afi[i:i + 0x20]
        if e[:4] == b'\0\0\0\0':
            break
        off, ln = struct.unpack_from('<II', e, 0x10)
        parts.append(dict(slot=i, name=bytes(e[:11]), off=off, len=ln, cs=struct.unpack_from('<I', e, 0x1C)[0]))
    return parts


def afi_fwimage(afi):
    for p in afi_parts(afi):
        if p['name'] == FWIMAGE_NAME:
            return p
    raise PatchError('no FWIMAGE.FW part in the AFI')


# ------------------------------------------------------------------------------------- FWIMAGE
def fw_dir(fw):
    ents = []
    for o in range(DIR_OFF, DIR_END, 0x20):
        if fw[o] == 0:
            break
        sec, ln, unk, ck = struct.unpack_from('<IIII', fw, o + 0x10)
        ents.append(dict(o=o, name=bytes(fw[o:o + 11]), sector=sec, len=ln, cs=ck))
    return ents


def fw_find(fw, name):
    for e in fw_dir(fw):
        if e['name'] == name:
            return e
    return None


def fw_file(fw, e):
    return bytes(fw[e['sector'] * 512:e['sector'] * 512 + e['len']])


def brec_verified(name):
    """BREC skips entries with extension '$..' and 'R?S' (.RES) when it checks the image"""
    return not (name[8:9] == b'$' or (name[8:9] == b'R' and name[10:11] == b'S'))


def fw_seal(fw):
    struct.pack_into('<I', fw, 0x10, cs32(fw[DIR_OFF:DIR_END]))
    struct.pack_into('<H', fw, 0x1FE, cs16(fw[0:0x1FE]))


# ------------------------------------------------------------------------------------- analysis
def brec_info(blob):
    ok_names = all(blob[NAMECMP + 8 * i] == 0xFE and blob[NAMECMP + 8 * i + 1] == SYS_NAME[i] for i in range(11))
    ld = sum8_16(blob, LOADER_RANGES)
    rd = sum8_16(blob, READER_RANGES)
    loader = ok_names and ld == KNOWN_LOADER_SIG
    reader = rd == KNOWN_READER_SIG
    return dict(loader=loader, reader=reader, ldsig=ld, rdsig=rd,
                status='zos-capable' if loader and reader else
                ('stock-only (unknown flash reader: zos never starts, stock boots)' if loader else
                 'UNSUPPORTED (unknown loader)'))


def kernel_ok(sysdata):
    """the stock kernel is linked for BREC's 0x0600 load: its vector page (file 0x3700) holds
    RST jumps into the loaded range"""
    vec = sysdata[0x3700:0x3780]
    hits = 0
    for v in range(0x08, 0x38, 8):
        if vec[v] == 0xC3 and 0x0600 <= struct.unpack_from('<H', vec, v + 1)[0] < 0x3300:
            hits += 1
    return hits > 0


def analyse(afi):
    """parse + verify everything; returns a dict (raises PatchError on a malformed file)"""
    errs = []
    parts = afi_parts(afi)
    for p in parts:
        if p['off'] + p['len'] > len(afi):
            errs.append('AFI part %s extends past the end of the file' % p['name'].decode('latin1'))
        elif p['len'] and cs32(afi[p['off']:p['off'] + p['len']]) != p['cs']:
            errs.append('AFI part %s: checksum' % p['name'].decode('latin1'))
    if struct.unpack_from('<I', afi, 0xFFC)[0] != cs32(afi[:0xFFC]):
        errs.append('AFI header checksum @0xFFC')
    pf = afi_fwimage(afi)
    fw = bytearray(afi[pf['off']:pf['off'] + pf['len']])
    if fw[:4] != FW_MAGIC:
        raise PatchError('FWIMAGE.FW has no 55 AA F0 0F header')
    if struct.unpack_from('<H', fw, 0x1FE)[0] != cs16(fw[:0x1FE]):
        errs.append('FWIMAGE header sum16 @0x1FE')
    if struct.unpack_from('<I', fw, 0x10)[0] != cs32(fw[DIR_OFF:DIR_END]):
        errs.append('FWIMAGE directory sum32 @0x10')
    ents = fw_dir(fw)
    spans = []
    for e in ents:
        d = fw_file(fw, e)
        if len(d) != e['len']:
            errs.append('%s: extends past the image' % e['name'].decode('latin1'))
        if cs32(d) != e['cs']:
            errs.append('%s: file checksum%s' % (e['name'].decode('latin1'),
                                                 ' (verified by BREC at every boot!)' if brec_verified(e['name']) else ''))
        if e['len']:
            spans.append((e['sector'] * 512, e['sector'] * 512 + e['len'], e['name']))
    spans.sort()
    for x, y in zip(spans, spans[1:]):
        if y[0] < x[1]:
            errs.append('files overlap: %s / %s' % (x[2].decode('latin1'), y[2].decode('latin1')))
    brecs = []
    for p in parts:
        if p['name'][:4] == b'BREC' and p['len'] == 0x4000:
            bi = brec_info(afi[p['off']:p['off'] + 0x4000])
            bi['name'] = p['name'].decode('latin1').replace(' ', '')
            brecs.append(bi)
    sys_e = fw_find(fw, SYS_NAME)
    stk_e = fw_find(fw, STK_NAME)
    patched = None
    if sys_e and sys_e['len'] >= UNDO_OFF + UNDO_LEN:
        rec = fw_file(fw, sys_e)[UNDO_OFF:UNDO_OFF + UNDO_LEN]
        if rec[:8] == UNDO_MAGIC:
            patched = parse_undo(rec)
            patched['params'] = parse_params(fw_file(fw, sys_e))
    return dict(afi=afi, parts=parts, pf=pf, fw=fw, ents=ents, errs=errs, brecs=brecs, sys=sys_e,
                stk=stk_e, patched=patched, md5=hashlib.md5(afi).hexdigest())


# ------------------------------------------------------------------------------------- stub + undo
def parse_params(sysdata):
    p = sysdata[PARAM_OFF:PARAM_OFF + 0x33]
    if p[:8] != b'ZOSBOOT1':
        return None
    flags, ver, volx, ld, rd = struct.unpack_from('<BBIHH', p, 8)
    return dict(flags=flags, ver=ver, volx=volx, ldsig=ld, rdsig=rd, zname=bytes(p[0x12:0x1D]),
                stk=bytes(p[0x1D:0x28]), sys=bytes(p[0x28:0x33]))


def name83(s):
    b, _, e = s.upper().partition('.')
    if not (0 < len(b) <= 8 and len(e) <= 3) or any(c in '"*+,/:;<=>?[\\]| ' for c in s):
        raise PatchError('bad 8.3 file name: %r' % s)
    return (b.ljust(8) + e.ljust(3)).encode('ascii')


def make_sys(stub, flags, volx, zname):
    if len(stub) != SYS_LEN or stub[0] != 0xC3 or stub[PARAM_OFF:PARAM_OFF + 8] != b'ZOSBOOT1':
        raise PatchError('stub file is not a zosboot.bin (rebuild it with build_stub.py)')
    if volx % 512:
        raise PatchError('volume offset must be a multiple of 512')
    s = bytearray(stub)
    struct.pack_into('<BBIHH', s, PARAM_OFF + 8, flags, s[PARAM_OFF + 9], volx, KNOWN_LOADER_SIG, KNOWN_READER_SIG)
    s[PARAM_OFF + 0x12:PARAM_OFF + 0x1D] = zname
    s[PARAM_OFF + 0x1D:PARAM_OFF + 0x28] = STK_NAME
    s[PARAM_OFF + 0x28:PARAM_OFF + 0x33] = SYS_NAME
    if any(b != 0x76 for b in s[UNDO_OFF:UNDO_OFF + UNDO_LEN]):
        raise PatchError('stub uses the undo-record area')
    return s


def make_undo(afi, a, insert_at, delta, stk_slot):
    """everything --unpatch needs besides the stock kernel copy (SYSCFG.STK)"""
    fw = a['fw']
    r = bytearray(UNDO_LEN)
    r[0:8] = UNDO_MAGIC
    struct.pack_into('<HHIII', r, 8, TOOL_VERSION, 0, len(afi), insert_at, delta)
    r[0x18:0x28] = bytes.fromhex(a['md5'])
    r[0x28:0x48] = fw[a['sys']['o']:a['sys']['o'] + 0x20]      # SYSCFG.SYS directory entry
    r[0x48:0x68] = fw[stk_slot:stk_slot + 0x20]                 # the slot SYSCFG.STK takes
    r[0x68:0x268] = fw[0:0x200]                                 # FWIMAGE header sector
    r[0x268:0x468] = afi[0:0x200]                               # AFI header + part table
    r[0x468:0x46C] = afi[0xFFC:0x1000]                          # AFI header sum
    assert len(r) == UNDO_LEN
    return r


def parse_undo(r):
    ver, _, afi_len, ins, delta = struct.unpack_from('<HHIII', r, 8)
    return dict(ver=ver, afi_len=afi_len, insert_at=ins, delta=delta, md5=bytes(r[0x18:0x28]).hex(),
                sys_ent=bytes(r[0x28:0x48]), stk_slot=bytes(r[0x48:0x68]), fwhdr=bytes(r[0x68:0x268]),
                afihdr=bytes(r[0x268:0x468]), afisum=bytes(r[0x468:0x46C]))


# ------------------------------------------------------------------------------------- patch
def patch(afi, stub, flags=F_KEYS | F_VBUS, volx=DEFAULT_VOLX, zname=b'ZOS     AP '):
    a = analyse(afi)
    if a['errs']:
        raise PatchError('input firmware fails its own checks, refusing to patch it:\n  ' + '\n  '.join(a['errs']))
    if a['patched']:
        raise PatchError('input is already patched (use --unpatch first, or --repatch)')
    if a['stk']:
        raise PatchError('input already contains SYSCFG.STK (an older zkern native build?): '
                         'patch the original stock firmware instead')
    sys_e = a['sys']
    if sys_e is None:
        raise PatchError('no SYSCFG.SYS in FWIMAGE: not an ATJ2085/s1mp3 firmware')
    if sys_e['len'] != SYS_LEN:
        raise PatchError('SYSCFG.SYS is %#x bytes, expected %#x (unknown kernel layout)' % (sys_e['len'], SYS_LEN))
    stock_sys = fw_file(a['fw'], sys_e)
    if not kernel_ok(stock_sys):
        raise PatchError('SYSCFG.SYS does not look like a kernel loaded at 0x0600 (vector page)')
    for e in a['ents']:
        if e['o'] != sys_e['o'] and e['len'] and e['sector'] * 512 < sys_e['sector'] * 512 + SYS_LEN \
                and sys_e['sector'] * 512 < e['sector'] * 512 + e['len']:
            raise PatchError('SYSCFG.SYS shares sectors with %s' % e['name'].decode('latin1'))
    if not a['brecs']:
        raise PatchError('no boot record (BREC*.BIN) in the AFI: cannot verify the boot path')
    bad = [b['name'] for b in a['brecs'] if not b['loader']]
    if bad:
        raise PatchError('unknown boot-record loader in %s: on such a device the stock fallback could not '
                         'work, refusing' % ', '.join(bad))
    if not any(b['reader'] for b in a['brecs']):
        raise PatchError('no boot record with the known flash reader: zos could never start here')
    if len(a['ents']) + 1 >= (DIR_END - DIR_OFF) // 0x20:
        raise PatchError('FWIMAGE directory full')

    fw0 = a['fw']
    pf = a['pf']
    fw = bytearray(fw0)
    # SYSCFG.STK = stock kernel, appended after everything (sector aligned)
    end = max([e['sector'] * 512 + e['len'] for e in a['ents']] + [len(fw)])
    sec = -(-end // 512)
    fw.extend(bytes(sec * 512 - len(fw)))
    fw.extend(stock_sys + bytes(-len(stock_sys) % 512))
    if len(fw) > COPY_B:
        raise PatchError('patched FWIMAGE would be %d bytes > %d (BREC keeps copy B at 8 MB)' % (len(fw), COPY_B))
    stk_slot = DIR_OFF + 0x20 * len(a['ents'])
    insert_at = pf['off'] + pf['len']
    delta = len(fw) - len(fw0)
    for p in a['parts']:
        if p['off'] < insert_at < p['off'] + p['len']:
            raise PatchError('AFI part %s straddles the end of FWIMAGE' % p['name'].decode('latin1'))
    new_sys = make_sys(stub, flags, volx, zname)
    new_sys[UNDO_OFF:UNDO_OFF + UNDO_LEN] = make_undo(afi, a, insert_at, delta, stk_slot)
    fw[stk_slot:stk_slot + 0x20] = STK_NAME + bytes(5) + struct.pack('<IIII', sec, SYS_LEN, 0, cs32(stock_sys))
    fw[sys_e['sector'] * 512:sys_e['sector'] * 512 + SYS_LEN] = new_sys
    struct.pack_into('<I', fw, sys_e['o'] + 0x1C, cs32(new_sys))
    fw_seal(fw)
    # AFI: FWIMAGE grows in place, later parts move by delta
    out = bytearray(afi[:pf['off']]) + fw + bytearray(afi[insert_at:])
    for p in a['parts']:
        if p['slot'] == pf['slot']:
            struct.pack_into('<I', out, p['slot'] + 0x14, len(fw))
            struct.pack_into('<I', out, p['slot'] + 0x1C, cs32(fw))
        elif p['off'] >= insert_at and p['len']:
            struct.pack_into('<I', out, p['slot'] + 0x10, p['off'] + delta)
    struct.pack_into('<I', out, 0xFFC, cs32(out[:0xFFC]))
    verify_patched(afi, bytes(out))
    return bytes(out)


def unpatch(afi):
    a = analyse(afi)
    u = a['patched']
    if not u:
        raise PatchError('not a zospatch-patched firmware (no undo record)')
    if a['errs']:
        raise PatchError('patched file is damaged:\n  ' + '\n  '.join(a['errs']))
    stk = a['stk']
    if stk is None:
        raise PatchError('SYSCFG.STK missing')
    stock_sys = fw_file(a['fw'], stk)
    ins, delta = u['insert_at'], u['delta']
    out = bytearray(afi[:ins]) + bytearray(afi[ins + delta:])
    if len(out) != u['afi_len']:
        raise PatchError('length mismatch while removing the appended data')
    pf_off = a['pf']['off']
    fwo = lambda x: pf_off + x
    sys_e = a['sys']
    out[fwo(sys_e['sector'] * 512):fwo(sys_e['sector'] * 512) + SYS_LEN] = stock_sys
    out[fwo(sys_e['o']):fwo(sys_e['o']) + 0x20] = u['sys_ent']
    out[fwo(stk['o']):fwo(stk['o']) + 0x20] = u['stk_slot']
    out[fwo(0):fwo(0x200)] = u['fwhdr']
    out[0:0x200] = u['afihdr']
    out[0xFFC:0x1000] = u['afisum']
    if hashlib.md5(out).hexdigest() != u['md5']:
        raise PatchError('restored file does not match the recorded md5 of the original')
    return bytes(out)


def verify_patched(orig, out):
    """re-parse the output and check it against the original"""
    a = analyse(out)
    if a['errs']:
        raise PatchError('internal: output fails checks: ' + '; '.join(a['errs']))
    o = analyse(orig)
    if fw_file(a['fw'], a['stk']) != fw_file(o['fw'], o['sys']):
        raise PatchError('internal: SYSCFG.STK != stock SYSCFG.SYS')
    for e in o['ents']:
        n = fw_find(a['fw'], e['name'])
        if n is None or n['sector'] != e['sector']:
            raise PatchError('internal: %s moved' % e['name'])
        if e['name'] != SYS_NAME and fw_file(a['fw'], n) != fw_file(o['fw'], e):
            raise PatchError('internal: %s changed' % e['name'])
    for p in o['parts']:
        q = [x for x in a['parts'] if x['slot'] == p['slot']][0]
        if p['name'] != FWIMAGE_NAME and orig[p['off']:p['off'] + p['len']] != out[q['off']:q['off'] + q['len']]:
            raise PatchError('internal: AFI part %s changed' % p['name'])
    new_sys = fw_file(a['fw'], a['sys'])
    pr = parse_params(new_sys)
    if new_sys[0] != 0xC3 or pr is None or pr['ldsig'] != KNOWN_LOADER_SIG:
        raise PatchError('internal: stub entry/parameters')
    if unpatch(out) != orig:
        raise PatchError('internal: --unpatch would not restore the original')


# ------------------------------------------------------------------------------------- report
def describe(path, afi):
    try:
        a = analyse(afi)
    except PatchError as e:
        print('%s: NOT USABLE: %s' % (path, e))
        return False
    print('%s' % path)
    print('  AFI %d bytes, md5 %s' % (len(afi), a['md5']))
    for p in a['parts']:
        print('    part %-12s off %#09x len %#09x' % (p['name'].decode('latin1').strip('\0'), p['off'], p['len']))
    for b in a['brecs']:
        print('  boot record %-12s loader sig %#06x rd sig %#06x -> %s' % (b['name'], b['ldsig'], b['rdsig'], b['status']))
    nver = sum(brec_verified(e['name']) for e in a['ents'])
    print('  FWIMAGE %d bytes (limit %d), %d files (%d checked by BREC at boot)' % (
        len(a['fw']), COPY_B, len(a['ents']), nver))
    s = a['sys']
    if s:
        print('  SYSCFG.SYS sector %d, %#x bytes, sum %#010x%s' % (s['sector'], s['len'], s['cs'],
              '' if a['patched'] else (', kernel vector page ' + ('OK' if kernel_ok(fw_file(a['fw'], s)) else 'NOT FOUND'))))
    if a['patched']:
        u, pr = a['patched'], a['patched']['params']
        print('  PATCHED by zospatch v%d: stub v%d, stock kernel kept as SYSCFG.STK (sector %d)' % (
            u['ver'], pr['ver'] if pr else -1, a['stk']['sector'] if a['stk'] else -1))
        if pr:
            print('    zos file %s, escapes: %s%s, volume at %#x' % (
                pr['zname'][:8].decode().strip() + '.' + pr['zname'][8:].decode().strip(),
                'MENU/VOL held' if pr['flags'] & F_KEYS else '',
                ', USB cable at power-on' if pr['flags'] & F_VBUS else '', pr['volx']))
        print('    original firmware md5 %s (--unpatch restores it)' % u['md5'])
    if a['errs']:
        print('  CHECKS FAILED:\n    ' + '\n    '.join(a['errs']))
    else:
        print('  all checksums OK (AFI parts, AFI header, FWIMAGE header/directory, every file)')
    return not a['errs']


def main():
    ap = argparse.ArgumentParser(description='zos boot-selector patcher for ATJ2085 .fw files')
    ap.add_argument('input', nargs='*')
    ap.add_argument('-o', '--output')
    ap.add_argument('--check', action='store_true', help='only analyse and verify the input(s)')
    ap.add_argument('--unpatch', action='store_true', help='restore the original from a patched file')
    ap.add_argument('--repatch', action='store_true', help='input is patched: unpatch, then patch again')
    ap.add_argument('--stub', default=os.path.join(HERE, 'zosboot.bin'))
    ap.add_argument('--no-vbus-escape', action='store_true',
                    help='do NOT boot stock when a USB cable is connected at power-on')
    ap.add_argument('--zos-file', default='ZOS.AP', help='zos core file in the disk root (default ZOS.AP)')
    ap.add_argument('--volume-offset', type=lambda x: int(x, 0), default=DEFAULT_VOLX,
                    help='FAT volume start in the boot-record reader address space (default 0x1040000)')
    ap.add_argument('--force', action='store_true', help='overwrite an existing output file')
    a = ap.parse_args()
    if not a.input:
        ap.error('no input firmware')
    try:
        if a.check:
            ok = True
            for p in a.input:
                ok &= describe(p, open(p, 'rb').read())
            return 0 if ok else 1
        if len(a.input) != 1 or not a.output:
            ap.error('give one input and -o OUTPUT')
        if os.path.abspath(a.output) == os.path.abspath(a.input[0]):
            raise PatchError('output must differ from the input')
        if os.path.exists(a.output) and not a.force:
            raise PatchError('%s exists (use --force to overwrite)' % a.output)
        src = open(a.input[0], 'rb').read()
        if a.unpatch:
            out = unpatch(src)
        else:
            if a.repatch and analyse(src)['patched']:
                src = unpatch(src)
            flags = F_KEYS | (0 if a.no_vbus_escape else F_VBUS)
            out = patch(src, open(a.stub, 'rb').read(), flags, a.volume_offset, name83(a.zos_file))
        open(a.output, 'wb').write(out)
        describe(a.output, out)
        print('written: %s (md5 %s)' % (os.path.abspath(a.output), hashlib.md5(out).hexdigest()))
        return 0
    except PatchError as e:
        print('zospatch: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
