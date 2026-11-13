/* snd: non-blocking DAC sound engine for page modules (music + sound effects while the module runs).
 * See snd.c for how it works; the rules for a module that uses it:
 *   - include snd.c from ONE source of the module (next to skin.c), this header from the others;
 *     do not link beep.c as well (both own the DAC): define beep() yourself as snd_key() if a shared
 *     helper (game.c's gm_key) wants it
 *   - snd_init() once, snd_exit() before mod_main returns (also on EV_POWER)
 *   - call snd_pump() OFTEN: the DAC has no DMA / IRQ, the CPU refills its 8-word FIFO (= 0.5 ms of
 *     audio at 8 kHz). The event wait must not HALT while sound is on (ev_wait / hal_idle sleep until
 *     the next 10 ms tick): poll with snd_pump() + skin_get(), HALT only while !snd_busy(). Drawing is
 *     done in pieces of at most ~0.3 ms with a snd_pump() between them (snd_wait() for delays).
 *   - cost: ~1.7 K of code (incl. the 124-byte note table), ~380 T-states per sample = 12 % of the CPU at
 *     24.576 MHz while a tune plays (3 voices), nothing when idle; ~90 bytes of RAM */
#ifndef ZOS_SND_H
#define ZOS_SND_H
#include <stdint.h>

/* a tune: two voices (melody 25 % pulse, bass 50 % square). Each voice is a list of {note, length}
 * byte pairs, length in sixteenths, note 0 = rest, 1..61 = C2..C7 (MIDI - 35); 0xFF ends the list
 * (the tune loops). Both voices must add up to the same length. unit = length of a sixteenth in
 * 1/16 ticks (10 ms ticks) at level 0 */
typedef struct { const uint8_t *mel, *bass; uint16_t unit; } snd_tune_t;

/* a sound effect: {amp, then triples {note, ticks, slide} ..., 0xFF}: amp = 1..127 (0x3C is a
 * normal loudness; music 0x22 melody / 0x16 bass; at most 0x44: the three voices add up to 127 at most), note as above (0 = rest), ticks of 10 ms,
 * slide = int8 change of the phase increment (note frequency x 8.192) per tick, in steps of 16 */

void    snd_init(void);                 /* DAC 8 kHz, amplifier up (silently, first time only), volume from SETTINGS */
void    snd_exit(void);                 /* fades everything out, amplifier volume to 0 (the amplifier stays on) */
void    snd_pump(void);                 /* refill the DAC FIFO + advance the sequencers; cheap when nothing to do */
uint8_t snd_busy(void);                 /* 1 = sound is running (do not HALT) */
void    snd_wait(uint8_t ticks);        /* delay that keeps pumping (events stay queued) */

void    snd_music(const snd_tune_t *t, uint8_t level);   /* start from the beginning; 0 = stop (fades out) */
void    snd_music_pause(uint8_t pause); /* 1 = silence now (keeps the position), 0 = go on */
void    snd_tempo(uint8_t level);       /* speed-up: sixteenth = unit * 16 / (16 + level) */
void    snd_sfx(const uint8_t *fx, uint8_t prio);   /* starts fx unless a running effect has a higher prio */
void    snd_sfx_stop(void);
uint8_t snd_sfx_busy(void);             /* an effect is still playing */
void    snd_key(void);                  /* key blip, only when ZF_BEEP (SETTINGS) is on */
#endif
