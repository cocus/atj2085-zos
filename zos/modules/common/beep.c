/* Key beep through the DAC (include from one module source; setting ZF_BEEP in core/config.h).
 * The CPU writes a short square wave into the DAC FIFO at 8 kHz (L and R words, see
 * modules/usbaudio/dacfeed.s: the DAC takes one word per channel). The headphone amplifier is
 * powered up once, silently (volume 0, ~200 ms), and then left on with the volume at 0 between
 * beeps: switching it per beep would pop. MP3PLAY's analog_on() finds it on and skips its own
 * power-up; its mp3_off() switches it off again (the next beep powers it up once more).
 * Not for modules where the DSP or an audio stream owns the DAC (MP3PLAY, USB speaker). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/config.h"
#include "regs.h"
#include "beep.h"


#define BEEP_FRAMES  200                  /* 25 ms at 8 kHz */
#define BEEP_HALF    2                    /* frames per half period: 8 kHz / 4 = 2 kHz */
#define BEEP_AMP     0x18                 /* square wave amplitude, high byte (~ -14 dBFS) */
#define BEEP_VOL     14                   /* amplifier volume while beeping (0..31) */

static void bp_wait(uint8_t n)
{
    uint16_t t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < n) hal_watchdog();
}

void beep(void)
{
    uint16_t i;
    uint8_t v, hi;
    if (!ZCFG_BEEP) return;
    DACCTL = DAC_INIT | DAC_MUTE;              /* DAC: 8 kHz, 12.288 MHz family */
    PLLCTL = PLLCTL & ~PLL_AUDIO_MASK;
    DACRATE = DACRATE_8K;
    DACFIFO = DACFIFO_INIT;
    DAC_L = 0;
    DACCTL = DAC_INIT;
    if (!(PACTL & PA_EN)) {                    /* first beep: amplifier up, silent */
        PAVOL &= ~PAVOL_MASK;
        PACTL |= PA_DAC | PA_EN;
        PACTL |= PA_ON;
        bp_wait(20);
    }
    PACTL |= PA_DAC;
    v = PAVOL;
    PAVOL = (v & ~PAVOL_MASK) | BEEP_VOL;
    for (i = 0; i < BEEP_FRAMES; i++) {
        hi = (i / BEEP_HALF) & 1 ? BEEP_AMP : (uint8_t)-BEEP_AMP;
        while (DACFIFO & DACFIFO_FULL) ;
        DAC_M = 0; DAC_H = hi;              /* left */
        while (DACFIFO & DACFIFO_FULL) ;
        DAC_M = 0; DAC_H = hi;              /* right */
    }
    for (i = 0; i < 8; i++) {                     /* end on silence */
        while (DACFIFO & DACFIFO_FULL) ;
        DAC_M = 0; DAC_H = 0;
    }
    bp_wait(1);
    PAVOL = v & ~PAVOL_MASK;                   /* amplifier stays on, silent */
}
