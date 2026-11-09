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

SFR(BP_PLL, P_PLLCTL);
SFR(BP_DACCTL, P_DACCTL);
SFR(BP_DACRATE, P_DACRATE);
SFR(BP_DACFIFO, P_DACFIFO);
SFR(BP_DAC_L, P_DAC_L);
SFR(BP_DAC_M, P_DAC_M);
SFR(BP_DAC_H, P_DAC_H);
SFR(BP_PAVOL, P_PAVOL);
SFR(BP_PACTL, P_PACTL);

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
    BP_DACCTL = DAC_INIT | DAC_MUTE;              /* DAC: 8 kHz, 12.288 MHz family */
    BP_PLL = BP_PLL & ~PLL_AUDIO_MASK;
    BP_DACRATE = DACRATE_8K;
    BP_DACFIFO = DACFIFO_INIT;
    BP_DAC_L = 0;
    BP_DACCTL = DAC_INIT;
    if (!(BP_PACTL & PA_EN)) {                    /* first beep: amplifier up, silent */
        BP_PAVOL &= ~PAVOL_MASK;
        BP_PACTL |= PA_DAC | PA_EN;
        BP_PACTL |= PA_ON;
        bp_wait(20);
    }
    BP_PACTL |= PA_DAC;
    v = BP_PAVOL;
    BP_PAVOL = (v & ~PAVOL_MASK) | BEEP_VOL;
    for (i = 0; i < BEEP_FRAMES; i++) {
        hi = (i / BEEP_HALF) & 1 ? BEEP_AMP : (uint8_t)-BEEP_AMP;
        while (BP_DACFIFO & DACFIFO_FULL) ;
        BP_DAC_M = 0; BP_DAC_H = hi;              /* left */
        while (BP_DACFIFO & DACFIFO_FULL) ;
        BP_DAC_M = 0; BP_DAC_H = hi;              /* right */
    }
    for (i = 0; i < 8; i++) {                     /* end on silence */
        while (BP_DACFIFO & DACFIFO_FULL) ;
        BP_DAC_M = 0; BP_DAC_H = 0;
    }
    bp_wait(1);
    BP_PAVOL = v & ~PAVOL_MASK;                   /* amplifier stays on, silent */
}
