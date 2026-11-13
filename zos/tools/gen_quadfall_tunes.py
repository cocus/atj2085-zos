"""Generates the audio data of QUADFALL / the sound engine from note names:
    zos/modules/common/sndtab.h     snd_inc[]: phase increment per note at 8 kHz (modules/common/snd.c)
    zos/modules/quadfall/tunes.c    the three tunes (melody + bass) and the sound effects

Run: python zos/tools/gen_quadfall_tunes.py   (the output is committed; the build does not run this)

Note numbers: n = MIDI - 35 (1 = C2 ... 61 = C7), 0 = rest, 0xFF ends a list.
Tune format (per voice): pairs {note, duration in sixteenths}. Sixteenth length = the tune's `unit`
(1/16 ticks) / (1 + level/16).
SFX format: {amp, then triples {note, ticks, slide} ..., 0xFF}; slide = int8 change of the phase
increment per tick, in units of 16.

The tunes are traditional / public domain melodies in OUR OWN simple arrangement (key, bass line,
rhythm of the accompaniment and the second-pass variation are ours; no recording or published
arrangement was copied):
  A  Korobeiniki (Russian folk song, 19th c.; the melody the "theme" of the falling-block genre uses)
  B  Greensleeves (English traditional, 16th c.)
  C  Ode to Joy (Beethoven, 1824)
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.join(HERE, '..', 'modules')
RATE = 8000
NMAX = 61

NAMES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def midi(name):
    m = re.fullmatch(r'([A-G])([#b]?)(-?\d)', name)
    assert m, name
    v = NAMES[m.group(1)] + {'#': 1, 'b': -1, '': 0}[m.group(2)] + 12 * (int(m.group(3)) + 1)
    return v


def note(name, shift=0):
    n = midi(name) + shift - 35
    assert 1 <= n <= NMAX, (name, shift, n)
    return n


def inc(n):
    f = 440.0 * 2 ** ((n + 35 - 69) / 12)
    return round(f * 65536 / RATE)


def arr(name, data):
    s = ', '.join(str(x) for x in data)
    out, line = [], '   '
    for tok in s.split(', '):
        if len(line) + len(tok) + 2 > 100:
            out.append(line.rstrip())
            line = '   '
        line += ' ' + tok + ','
    out.append(line.rstrip())
    return 'const uint8_t %s[] = {\n%s\n};\n' % (name, '\n'.join(out))


def seq(items, shift=0):
    """'E5:4 B4:2 R:2 ...' -> [note, dur, ...]"""
    r = []
    for tok in items.split():
        nm, d = tok.split(':')
        r += [0 if nm == 'R' else note(nm, shift), int(d)]
    return r


def total(v):
    return sum(v[1::2])


# ---- A: Korobeiniki, written in A minor, played in E minor (-5) ------------------------------------------
KOR = ('E5:4 B4:2 C5:2 D5:4 C5:2 B4:2   A4:4 A4:2 C5:2 E5:4 D5:2 C5:2   B4:6 C5:2 D5:4 E5:4   C5:4 A4:4 A4:8   '
       'D5:6 F5:2 A5:4 G5:2 F5:2   E5:6 C5:2 E5:4 D5:2 C5:2   B4:4 B4:2 C5:2 D5:4 E5:4   C5:4 A4:4 A4:8')


def kor_oct(s):
    out = []
    for tok in s.split():
        nm, d = tok.split(':')
        out.append('%s%d:%s' % (nm[:-1], int(nm[-1]) + 1, d))
    return ' '.join(out)


def kor_bass(eighths):
    # chord per bar (E minor): Em Em B Em | Am Em B Em ; root / fifth in octave 3
    ch = [('E3', 'B3'), ('E3', 'B3'), ('B2', 'F#3'), ('E3', 'B3'), ('A3', 'E4'), ('E3', 'B3'), ('B2', 'F#3'), ('E3', 'B3')]
    v = []
    for r, f in ch:
        if eighths:
            v += seq('%s:2 %s:2 %s:2 %s:2 %s:2 %s:2 %s:2 %s:2' % (r, f, r, f, r, f, r, f))
        else:
            v += seq('%s:4 %s:4 %s:4 %s:4' % (r, f, r, f))
    return v


A_MEL = seq(KOR, -5) + seq(kor_oct(KOR), -5)
A_BASS = kor_bass(False) + kor_bass(True)
assert total(A_MEL) == total(A_BASS) == 16 * 16, (total(A_MEL), total(A_BASS))

# ---- B: Greensleeves, A minor, 3/4 (quarter = 4) -------------------------------------------------------------------
GS = ('A4:4  C5:8 D5:4  E5:6 F5:2 E5:4  D5:8 B4:4  G4:6 A4:2 B4:4  C5:8 A4:4  A4:6 G#4:2 A4:4  B4:8 G#4:4  E4:8 A4:4  '
      'C5:8 D5:4  E5:6 F5:2 E5:4  D5:8 B4:4  G4:6 A4:2 B4:4  C5:6 B4:2 A4:4  G#4:6 F#4:2 G#4:4  A4:8 R:4')
GS_CH = ['A', 'A', 'G', 'G', 'A', 'A', 'E', 'E', 'A', 'A', 'G', 'G', 'A', 'E', 'A']
GS_BS = {'A': ('A2', 'E3'), 'G': ('G2', 'D3'), 'E': ('E3', 'B3')}
B_MEL = seq(GS)
B_BASS = seq('R:4')
for c in GS_CH:
    r, f = GS_BS[c]
    B_BASS += seq('%s:4 %s:4 %s:4' % (r, f, f))
assert total(B_MEL) == total(B_BASS) == 184, (total(B_MEL), total(B_BASS))

# ---- C: Ode to Joy, G major, 4/4 (quarter = 4) -----------------------------------------------------------------------
OA = ['B4:4 B4:4 C5:4 D5:4', 'D5:4 C5:4 B4:4 A4:4', 'G4:4 G4:4 A4:4 B4:4']
OEND1 = 'B4:6 A4:2 A4:8'
OEND2 = 'A4:6 G4:2 G4:8'
OB = ['A4:4 A4:4 B4:4 G4:4', 'A4:4 B4:2 C5:2 B4:4 G4:4', 'A4:4 B4:2 C5:2 B4:4 A4:2 G4:2', 'G4:4 A4:4 D4:8']
C_MEL = seq(' '.join(OA + [OEND1] + OA + [OEND2] + OB + OA + [OEND2]))
CH = {'G': ('G3', 'D4'), 'D': ('D3', 'A3')}
C_CH = list('GGGD') + list('GGGD') + list('DGDG') + list('GGGD')
C_BASS = []
for i, c in enumerate(C_CH):
    r, f = CH[c]
    if i in (7, 15):                                   # the bar before the last / the final one: D then G
        C_BASS += seq('D3:4 A3:4 G3:4 D4:4')
    else:
        C_BASS += seq('%s:4 %s:4 %s:4 %s:4' % (r, f, r, f))
assert total(C_MEL) == total(C_BASS) == 256, (total(C_MEL), total(C_BASS))

TUNES = [('a', A_MEL, A_BASS, 160), ('b', B_MEL, B_BASS, 240), ('c', C_MEL, C_BASS, 200)]

# ---- sound effects -----------------------------------------------------------------------------------------------------
def sfx(amp, items):
    """items: 'C5:3' / 'G5:5:-40' (note:ticks[:slide in inc/tick])"""
    v = [amp]
    for tok in items.split():
        p = tok.split(':')
        sl = int(p[2]) // 16 if len(p) > 2 else 0
        v += [0 if p[0] == 'R' else note(p[0]), int(p[1]), sl & 0xFF]
    return v + [0xFF]


SFX = [
    ('move',   sfx(0x18, 'D6:2')),
    ('rot',    sfx(0x2C, 'E5:2 A5:2')),
    ('lock',   sfx(0x44, 'E3:6:-144')),
    ('drop',   sfx(0x38, 'G5:5:-1100')),
    ('clear1', sfx(0x3C, 'C5:3 E5:3 G5:5')),
    ('clear2', sfx(0x3C, 'C5:3 E5:3 G5:3 C6:6')),
    ('clear3', sfx(0x3C, 'C5:3 E5:3 G5:3 C6:3 E6:7')),
    ('clear4', sfx(0x3C, 'C5:3 E5:3 G5:3 C6:3 E6:3 G6:4 R:1 C6:3 G6:14')),
    ('level',  sfx(0x3C, 'G4:4 C5:4 E5:4 G5:6 R:2 E5:3 G5:16')),
    ('over',   sfx(0x3C, 'G4:8 E4:8 C4:8 G3:10 G3:30:-6')),
]

# ---- write -----------------------------------------------------------------------------------------------------------------
with open(os.path.join(MOD, 'common', 'sndtab.h'), 'w', newline='\n') as f:
    f.write('/* generated by tools/gen_quadfall_tunes.py - do not edit.\n'
            ' * snd_inc[n]: phase increment (65536 = 8 kHz) of note n = MIDI - 35 (1 = C2 ... %d = C7) */\n' % NMAX)
    f.write('const uint16_t snd_inc[%d] = {\n    0,' % (NMAX + 1))
    for n in range(1, NMAX + 1):
        f.write('%s%d,' % ('\n    ' if n % 12 == 1 else ' ', inc(n)))
    f.write('\n};\n')

out = ['/* generated by tools/gen_quadfall_tunes.py - do not edit (edit the script, run it, commit both).\n'
       ' * Public-domain melodies, our own arrangement: A Korobeiniki (Russian folk song), B Greensleeves\n'
       ' * (English traditional), C Ode to Joy (Beethoven). See the script for the format. */\n'
       '#include <stdint.h>\n#include "tunes.h"\n']
for t, mel, bass, unit in TUNES:
    out.append(arr('t_%s_mel' % t, mel))
    out.append(arr('t_%s_bass' % t, bass))
out.append('const snd_tune_t tunes[3] = {\n' + ',\n'.join(
    '    { t_%s_mel, t_%s_bass, %d }' % (t, t, unit) for t, _, _, unit in TUNES) + '\n};\n')
for nm, v in SFX:
    out.append(arr('fx_' + nm, v))
with open(os.path.join(MOD, 'quadfall', 'tunes.c'), 'w', newline='\n') as f:
    f.write('\n'.join(out))
with open(os.path.join(MOD, 'quadfall', 'tunes.h'), 'w', newline='\n') as f:
    f.write('/* generated by tools/gen_quadfall_tunes.py - do not edit */\n#ifndef QF_TUNES_H\n#define QF_TUNES_H\n'
            '#include <stdint.h>\n#include "../common/snd.h"\nextern const snd_tune_t tunes[3];\n')
    for nm, _ in SFX:
        f.write('extern const uint8_t fx_%s[];\n' % nm)
    f.write('#endif\n')
print('tunes: %s' % ', '.join('%s %d B' % (t, len(m) + len(b)) for t, m, b, _ in TUNES))
