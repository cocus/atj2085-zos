/* snd: non-blocking DAC sound engine (API and usage rules: snd.h). Include from one module source.
 *
 * Hardware: the DAC runs at 8 kHz and takes one 16-bit word per channel (M = low byte, H = high byte;
 * the H write pushes the word) from an 8-word FIFO (DACFIFO_FULL); there is no DMA and no interrupt,
 * so the CPU has to keep the FIFO filled: 8 words = 4 stereo frames = 0.5 ms. snd_pump() writes words
 * while the FIFO is not full (at most 12 per call: a flag that never sets cannot hang it) and returns.
 * It must be called more often than every 0.5 ms while sound is on - the module's loops and drawing
 * helpers call it between small pieces of work. If it is late, the FIFO runs dry (a short dropout; the
 * synthesis continues with the next sample - never garbage).
 *
 * Synthesis (per sample ~380 T-states = 12 % of the CPU at 24.576 MHz, measured in the emulator): three
 * phase-accumulator pulse voices summed into one mono sample written to both channels:
 *   melody  25 % pulse, bass  50 % square   (the tune's two lists, one shared sixteenth-note clock),
 *   sfx     50 % square                      (one effect at a time, mixed over the music)
 * Each voice has an amplitude that slews to its target every 32 samples (4 ms, ~16 ms ramps): notes
 * start and end without clicks, and when everything is silent the output is exactly 0. When nothing is
 * playing nothing is written (the last words were zeros) and the CPU can HALT.
 *
 * Time: the tune sequencer and the effects advance on hal_ticks() (10 ms), so the tempo does not
 * depend on how fast samples are produced. The amplifier: powered up silently once (as beep.c) and
 * left ON; its volume follows ZCFG->vol (a bit lower: square waves are loud), is set when sound starts
 * and ramps down to 0 after 1.5 s of silence; snd_exit() ramps it to 0 (like MP3PLAY's mp3_off, no
 * pop) and leaves it powered, the state beep.c / MP3PLAY expect. */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/config.h"
#include "regs.h"
#include "snd.h"

SFR(SN_PLL, P_PLLCTL);
SFR(SN_DACCTL, P_DACCTL);
SFR(SN_DACRATE, P_DACRATE);
SFR(SN_DACFIFO, P_DACFIFO);
SFR(SN_DAC_L, P_DAC_L);
SFR(SN_DAC_M, P_DAC_M);
SFR(SN_DAC_H, P_DAC_H);
SFR(SN_PAVOL, P_PAVOL);
SFR(SN_PACTL, P_PACTL);

#include "sndtab.h"                       /* snd_inc[]: phase increment per note */

#define SN_MEL_AMP   0x22
#define SN_BASS_AMP  0x16
#define SN_MAX_WORDS 12                   /* per snd_pump() call */
#define SN_IDLE_TICKS 150                 /* silence before the amplifier volume ramps to 0 */
#define SN_SLEW_DIV  2                    /* amp step per 4 ms = level >> 2 */

/* hot state (separate globals: the sample loop is the expensive part) */
static uint16_t m_ph, m_inc, b_ph, b_inc, s_ph, s_inc;
static int8_t   m_amp, b_amp, s_amp, m_tgt, b_tgt, s_tgt, s_lvl;
static uint8_t  envc, half, lo, hi, active;

/* sequencer */
static const snd_tune_t *tune;
static const uint8_t *mp[2];              /* next event of the melody / bass list */
static uint8_t  left[2], gate_on[2];      /* sixteenths left of the note; release gate armed */
static uint16_t unit, acc, gate[2];       /* sixteenth length, time into it, release time (1/16 ticks) */
static uint8_t  mus_on;
static uint16_t last_tick;
static const uint8_t *fx_p;               /* next triple of the running effect, 0 = none */
static uint8_t  fx_left, fx_prio;
static int8_t   fx_slide;
static uint8_t  pa_cur, pa_tgt, idle_t, pa_pend;

static void sn_pa(uint8_t v)
{
    pa_cur = v;
    SN_PAVOL = (SN_PAVOL & ~PAVOL_MASK) | v;
}

static void sn_wake(void)
{
    idle_t = 0;
    active = 1;
    if (pa_cur != pa_tgt) sn_pa(pa_tgt);
}

/* ---- sequencer ----------------------------------------------------------------------------------------- */
static void mus_load(uint8_t v)
{
    const uint8_t *p = mp[v];
    uint8_t n, d;
    uint16_t t;
    if (p[0] == 0xFF) p = v ? tune->bass : tune->mel;          /* loop */
    n = p[0]; d = p[1];
    mp[v] = p + 2;
    left[v] = d;
    t = (uint16_t)d * unit;
    t /= 5;                                                     /* release: the last fifth, at most 4 ticks */
    gate[v] = t > 64 ? 64 : t;
    if (v) { b_inc = snd_inc[n]; b_tgt = n ? SN_BASS_AMP : 0; }
    else   { m_inc = snd_inc[n]; m_tgt = n ? SN_MEL_AMP : 0; }
    gate_on[v] = n != 0;
}

static void mus_tick(uint8_t d)
{
    uint8_t v;
    acc += (uint16_t)d * 16;
    while (acc >= unit) {
        acc -= unit;
        for (v = 0; v < 2; v++)
            if (!--left[v]) mus_load(v);
    }
    for (v = 0; v < 2; v++)                                     /* release gap before the next note */
        if (gate_on[v] && (uint16_t)left[v] * unit - acc <= gate[v]) {
            gate_on[v] = 0;
            if (v) b_tgt = 0; else m_tgt = 0;
        }
}

static void fx_next(void)
{
    const uint8_t *p = fx_p;
    uint8_t n = p[0];
    if (n == 0xFF) { fx_p = 0; fx_prio = 0; s_tgt = 0; return; }
    s_inc = snd_inc[n];
    s_tgt = n ? s_lvl : 0;
    fx_left = p[1];
    fx_slide = (int8_t)p[2];
    fx_p = p + 3;
}

static void fx_tick(uint8_t d)
{
    while (d-- && fx_p) {
        s_inc += (int16_t)fx_slide * 16;
        if (!--fx_left) fx_next();
        else if (fx_left == 1 && s_tgt && fx_p[-2] >= 3) s_tgt = 0;   /* gap before the next note (notes of 3+ ticks) */
    }
}

static void sn_tick(uint8_t d)
{
    if (mus_on) mus_tick(d);
    if (fx_p) fx_tick(d);
    if (active) {
        if (!mus_on && !fx_p && !m_amp && !b_amp && !s_amp) {   /* silent: the amplifier follows later */
            if (idle_t < 255) idle_t++;
            if (idle_t >= SN_IDLE_TICKS) active = 0;
        } else idle_t = 0;
    } else if (pa_cur && ++idle_t > SN_IDLE_TICKS + 2) {
        idle_t = SN_IDLE_TICKS + 1;
        sn_pa(pa_cur - 1);                                      /* ramp down, one step per tick */
    }
}

static int8_t slew(int8_t a, int8_t t, uint8_t st)
{
    if (a < t) { a += st; if (a > t) a = t; }
    else if (a > t) { a -= st; if (a < t) a = t; }
    return a;
}

static void sn_env(void)                                        /* every 32 samples */
{
    m_amp = slew(m_amp, m_tgt, SN_MEL_AMP >> SN_SLEW_DIV);
    b_amp = slew(b_amp, b_tgt, SN_BASS_AMP >> SN_SLEW_DIV);
    s_amp = slew(s_amp, s_tgt, (uint8_t)s_lvl >> SN_SLEW_DIV);
}

/* One sample (the hot spot, ~170 T-states; SDCC's code for the same expression took ~800):
 *   m_ph += m_inc; b_ph += b_inc; s_ph += s_inc;
 *   sum = (m_ph < 0x4000 ? m_amp : -m_amp) + (b_ph & 0x8000 ? -b_amp : b_amp) + (s_ph & 0x8000 ? -s_amp : s_amp)
 *   word = sum * 128  ->  hi = sum >> 1 (arithmetic), lo = (sum & 1) << 7
 * The amplitudes add up to at most 127 (SN_MEL_AMP + SN_BASS_AMP + the largest effect, 0x44). */
static void sn_synth(void) __naked
{
    __asm
        ld      hl, (_m_ph)
        ld      de, (_m_inc)
        add     hl, de
        ld      (_m_ph), hl
        ld      a, h
        cp      a, #0x40
        ld      a, (_m_amp)
        jr      C, 1$
        neg
1$:     ld      c, a
        ld      hl, (_b_ph)
        ld      de, (_b_inc)
        add     hl, de
        ld      (_b_ph), hl
        ld      a, (_b_amp)
        bit     7, h
        jr      Z, 2$
        neg
2$:     add     a, c
        ld      c, a
        ld      hl, (_s_ph)
        ld      de, (_s_inc)
        add     hl, de
        ld      (_s_ph), hl
        ld      a, (_s_amp)
        bit     7, h
        jr      Z, 3$
        neg
3$:     add     a, c
        ld      c, a
        sra     a
        ld      (_hi), a
        ld      a, c
        rrca
        and     a, #0x80
        ld      (_lo), a
        ret
    __endasm;
}

static void sn_fill(void)
{
    uint8_t n = SN_MAX_WORDS;
    while (n--) {
        if (SN_DACFIFO & DACFIFO_FULL) return;
        if (half) half = 0;
        else {
            if (!active) return;
            sn_synth();
            if (++envc == 32) { envc = 0; sn_env(); }
            half = 1;
        }
        SN_DAC_M = lo;
        SN_DAC_H = hi;
    }
}

/* Order matters: fill the FIFO first, then do the 10 ms bookkeeping (sequencer, effects: up to a few
 * thousand T-states) while the FIFO plays, then fill again. */
void snd_pump(void)
{
    uint16_t now = hal_ticks(), d;
    sn_fill();
    if (now != last_tick) {
        d = now - last_tick;
        last_tick = now;
        sn_tick(d > 100 ? 100 : (uint8_t)d);
        sn_fill();
    }
}

uint8_t snd_busy(void) { return active; }

void snd_wait(uint8_t ticks)
{
    uint16_t t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < ticks) { snd_pump(); hal_watchdog(); }
}

/* ---- control ----------------------------------------------------------------------------------------------- */
void snd_tempo(uint8_t level)
{
    uint16_t u = tune ? (uint16_t)(tune->unit * 16) / (16 + level) : 160;
    unit = u;
}

void snd_music(const snd_tune_t *t, uint8_t level)
{
    if (!t) { mus_on = 0; m_tgt = b_tgt = 0; return; }
    tune = t;
    snd_tempo(level);
    mp[0] = t->mel; mp[1] = t->bass;
    acc = 0;
    mus_load(0); mus_load(1);
    mus_on = 1;
    sn_wake();
}

void snd_music_pause(uint8_t pause)
{
    if (!tune) return;
    if (pause) { mus_on = 0; m_tgt = b_tgt = 0; gate_on[0] = gate_on[1] = 0; }
    else { mus_on = 1; sn_wake(); }       /* the current note's pitch is already set: it sounds again from the next note */
}

void snd_sfx(const uint8_t *fx, uint8_t prio)
{
    if (fx_p && prio < fx_prio) return;
    s_lvl = (int8_t)fx[0];
    fx_p = fx + 1;
    fx_prio = prio;
    fx_next();
    sn_wake();
}

void snd_sfx_stop(void) { fx_p = 0; fx_prio = 0; s_tgt = 0; }
uint8_t snd_sfx_busy(void) { return fx_p != 0; }

static const uint8_t fx_key[] = { 0x24, 60, 3, 0, 0xFF };       /* ~2 kHz, 30 ms, like beep.c */
void snd_key(void)
{
    if (ZCFG_BEEP) snd_sfx(fx_key, 0);
}

/* ---- setup / teardown (same DAC and amplifier sequence as beep.c) -------------------------------------------------- */
static void sn_wait_ticks(uint8_t n)
{
    uint16_t t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < n) hal_watchdog();
}

void snd_init(void)
{
    uint8_t v = ZCFG_OK ? ZCFG->vol : VOL_DEF;
    v = (uint8_t)(v * 3) >> 2;                                  /* a bit below the player's volume: square waves are loud */
    pa_tgt = v < 4 ? 4 : v > 22 ? 22 : v;
    m_amp = b_amp = s_amp = m_tgt = b_tgt = s_tgt = 0;
    mus_on = 0; fx_p = 0; fx_prio = 0; tune = 0; half = 0; active = 0; envc = 0;
    SN_DACCTL = DAC_INIT | DAC_MUTE;                            /* 8 kHz, 12.288 MHz audio clock family */
    SN_PLL = SN_PLL & ~PLL_AUDIO_MASK;
    SN_DACRATE = DACRATE_8K;
    SN_DACFIFO = DACFIFO_INIT;
    SN_DAC_L = 0;
    SN_DACCTL = DAC_INIT;
    if (!(SN_PACTL & PA_EN)) {                                  /* first time: amplifier up, silent */
        SN_PAVOL &= ~PAVOL_MASK;
        SN_PACTL |= PA_DAC | PA_EN;
        SN_PACTL |= PA_ON;
        sn_wait_ticks(20);
    }
    SN_PACTL |= PA_DAC;
    pa_cur = SN_PAVOL & PAVOL_MASK;
    last_tick = hal_ticks();
    idle_t = SN_IDLE_TICKS + 1;
}

void snd_exit(void)
{
    uint8_t i;
    snd_music(0, 0);
    snd_sfx_stop();
    for (i = 0; i < 20 && (m_amp || b_amp || s_amp); i++) snd_wait(1);   /* amplitudes down to 0 (zeros are written) */
    snd_wait(1);
    while (pa_cur) { sn_pa(pa_cur - 1); sn_wait_ticks(1); }      /* fade like mp3_off, then stay powered */
    active = 0;
}
