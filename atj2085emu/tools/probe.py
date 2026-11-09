import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
e = ATJ2085(FwImage(os.path.join(os.path.dirname(__file__), '..', '..', 'my-ximpro.fw')), log=lambda s: None)
e.run(int(sys.argv[1]))
print([ (t, '%04x' % pc, d, '%02x' % p, '%02x' % v) for (t, pc, d, p, v) in e.port_log if p in (0x27, 0x26, 0x44, 0x43, 0x22, 0x23, 0x24)])
print(e.regs(), 'mint_en=%02x' % e.ports[0x27], 'p44=%02x' % e.ports[0x44], 'p43=%02x' % e.ports[0x43])
print('mem37=%02x' % e.mem[0x37], 'im=', e.m.int_mode if hasattr(e.m,'int_mode') else '?')
