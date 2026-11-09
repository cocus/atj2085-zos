"""Firmware profiles. Select with env LISONG_PROFILE (default v05).
  v05 : LISONG_LSD_501_05-V35.37.bin  (2006-08-07, the updater image)
  v04 : my-ximpro.fw = LSD_501_04 (2006-07-16), read back from the device (re-skinned RES)
"""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROFILES = {
    'v05': dict(fw=os.path.join(ROOT, 'orig', 'LISONG_LSD_501_05-V35.37.bin'),
                fwimage=os.path.join(ROOT, 'orig', 'fwimage'),
                libmatch=os.path.join(ROOT, 'analysis', 'libmatch_lisong.json'),
                disasm=os.path.join(ROOT, 'disasm'),
                c=os.path.join(ROOT, 'c'),
                build=os.path.join(ROOT, 'build')),
    'v04': dict(fw=os.path.join(ROOT, '..', 'my-ximpro.fw'),
                fwimage=os.path.join(ROOT, 'ref', 'myximpro', 'fwimage'),
                libmatch=os.path.join(ROOT, 'analysis', 'libmatch_v04.json'),
                disasm=os.path.join(ROOT, 'disasm_v04'),
                c=os.path.join(ROOT, 'c_v04'),
                build=os.path.join(ROOT, 'build_v04')),
}
NAME = os.environ.get('LISONG_PROFILE', 'v05')
P = PROFILES[NAME]
