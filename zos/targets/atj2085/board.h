/* zos target ATJ2085, this board (Ximpro MP4 / LISONG LSD_501): calibration values that differ from
 * one player to another (register meanings are in regs.h).
 *
 * ---- battery icon --------------------------------------------------------------------------------
 * The battery ADC (regs.h P_BATT & BATT_LEVEL: 0.90 V + 0.04 V/step, a new value about once a second)
 * reads differently on each board, and HIGHER while USB power is present: then it measures the
 * charger, not the cell. So there are two scales, picked by VBUS (regs.h P_USB_STAT & USBST_VBUS):
 *   level <  BATT_EMPTY_* : empty icon (no segments)
 *   level >= BATT_FULL_*  : full icon (all segments, green)
 *   between: proportional; colour red up to 1/3, yellow up to 2/3, green above (common/draw.c).
 * Measured on the user's player (2026-11): 14-15 with USB connected and the cell full; on battery a
 * full cell reads lower (the old single scale, full >= 13, showed it yellow = levels 9..11). Chosen:
 * on battery full >= 11 (green from 10), on USB full >= 13 (the old value); empty < 6 on both (the
 * stock firmware's empty icon). To tune: SYSINFO shows the raw level, VBUS and the debounced state
 * the icon uses (USBCON console: "batt"); set FULL to the level a charged cell shows a few minutes
 * after unplugging, EMPTY to the level shortly before the player switches itself off.
 * BATT_OFF / BATT_STANDBY_MIN (power-off thresholds) stay in regs.h: they are not icon calibration.
 *
 * The core (targets/atj2085/platform.c) samples level + VBUS twice a second and posts EV_BATT when the
 * debounced state changes: VBUS at once (2 equal samples), the level when it moved by
 * BATT_HYST_STEPS or more (2 samples), a 1-step move only after BATT_SLOW_SAMPLES equal samples (so a
 * level wobbling between two values does not redraw the icon every second). */
#ifndef ZOS_BOARD_H
#define ZOS_BOARD_H
#define BATT_EMPTY_BAT    6
#define BATT_FULL_BAT     11
#define BATT_EMPTY_USB    6
#define BATT_FULL_USB     13
#define BATT_HYST_STEPS   2
#define BATT_SLOW_SAMPLES 7       /* repeats: 1 + 7 equal samples = 4 s (max 7: 3-bit counter) */
#endif
