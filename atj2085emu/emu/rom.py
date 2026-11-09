"""Internal mask-ROM runtime library (IAR Z80 C helpers) at 0x8800..0x8E77.

Evidence: firmware calls absolute addresses such as 0x8889/0x8920/0x8D0C from
SYSCFG/SYSBNKx/APs/DRVs; these match exactly the symbol addresses of the SDK
library `Sdk_i_43/lib/comlib.r01` (absolute-located module "cliball") as linked
by XLINK in lisong_rebuild/ref/sdklib/comlib.map (?L_ADDASG_L03=8889,
?L_LSH_L03=8920, ?UC_DIV_L01=8D0C, memmove=8E5B ...). We load that S-record.
ASSUMPTION: the real chip ROM content equals this library (no ROM dump exists
in the workspace; TROM_L_M.rom in the SDKs is encrypted).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROM_S28 = os.path.join(HERE, '..', 'rom', 'comlib.s28')


def load_srec(path=ROM_S28, min_addr=0x8000):
    out = {}
    for line in open(path):
        line = line.strip()
        if not line.startswith('S1'):
            continue
        n = int(line[2:4], 16)
        addr = int(line[4:8], 16)
        data = bytes.fromhex(line[8:8 + (n - 3) * 2])
        if addr >= min_addr:
            out[addr] = data
    return out


def symbols(path=os.path.join(HERE, '..', 'rom', 'comlib.map')):
    syms = {}
    try:
        for line in open(path):
            p = line.split()
            if len(p) == 2 and p[0].startswith(('?', 'memmove')) and len(p[1]) == 4:
                syms[int(p[1], 16)] = p[0]
    except OSError:
        pass
    return syms
