"""Smoke test of the GUI code paths without user interaction."""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from emu.gui import App
app = App(os.path.join(os.path.dirname(__file__), '..', '..', 'my-ximpro.fw'))
app.toggle()                       # start running
t0 = time.time()
def stop():
    app.running = False
    app.emu.key_model.hold('LEFT'); app.emu.run(3_000_000); app.emu.key_model.release('LEFT')
    app.emu.run(20_000_000)
    app.refresh(); app.step(5)
    print('gui ok: ticks=%d pc=%04x frames-run-ms=%.0f' % (app.emu.ticks, app.emu.m.pc, (time.time()-t0)*1000))
    app.root.destroy()
app.root.after(12000, stop)
app.root.mainloop()
