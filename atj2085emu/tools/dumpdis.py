"""dumpdis.py TICKS ADDR COUNT : run TICKS then disassemble emulated memory."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from emu.fwimage import FwImage
from emu.soc import ATJ2085
e = ATJ2085(FwImage(os.path.join(os.path.dirname(__file__), '..', '..', 'my-ximpro.fw')), log=lambda s: None)
e.run(int(sys.argv[1]))
for a, h, t in e.disasm(int(sys.argv[2], 16), int(sys.argv[3])):
    print('%04x %-10s %s' % (a, h, t))
