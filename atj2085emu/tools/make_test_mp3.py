"""Generate the test MP3s used by the README tests (needs lameenc + numpy).
usage: make_test_mp3.py OUTDIR"""
import os, sys
import numpy as np
import lameenc

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'testdisk')
os.makedirs(out, exist_ok=True)


def enc(pcm, sr, ch, kbps):
    e = lameenc.Encoder()
    e.set_bit_rate(kbps); e.set_in_sample_rate(sr); e.set_channels(ch); e.set_quality(2)
    return e.encode(pcm.tobytes()) + e.flush()


def id3(title, artist):
    def frame(fid, text):
        b = b'\x00' + text.encode('latin1')
        return fid + len(b).to_bytes(4, 'big') + b'\x00\x00' + b
    body = frame(b'TIT2', title) + frame(b'TPE1', artist)
    sz = len(body)
    syn = bytes([(sz >> 21) & 0x7f, (sz >> 14) & 0x7f, (sz >> 7) & 0x7f, sz & 0x7f])
    return b'ID3\x03\x00\x00' + syn + body


sr = 44100
t = np.arange(int(sr * 12)) / sr
# 440 Hz left, 660 Hz right, with a 1 Hz amplitude wobble so time alignment is checkable
env = 0.25 * (0.6 + 0.4 * np.sin(2 * np.pi * 0.5 * t))
l = (env * np.sin(2 * np.pi * 440 * t) * 32767).astype(np.int16)
r = (env * np.sin(2 * np.pi * 660 * t) * 32767).astype(np.int16)
st = np.stack([l, r], 1)
open(os.path.join(out, 'TONE440.MP3'), 'wb').write(id3('Tone 440-660', 'atj2085emu') + enc(st, sr, 2, 128))
# a rising sweep, mono 22.05 kHz 64 kbps (exercises another DAC rate)
sr2 = 22050
t2 = np.arange(int(sr2 * 8)) / sr2
f = 200 + 600 * t2 / 8
ph = 2 * np.pi * np.cumsum(f) / sr2
m = (0.3 * np.sin(ph) * 32767).astype(np.int16)
open(os.path.join(out, 'Sweep mono 22k.mp3'), 'wb').write(enc(m, sr2, 1, 64))
print('wrote', os.listdir(out))
