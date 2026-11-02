/*
 * CROSS - mono bass voice in the spirit of a classic three-oscillator
 * ladder synth. Portable fixed-point core: 32-bit integers only, no
 * allocation and no division inside the sample loop. The Kennel wrapper
 * (cross.c) owns the OP-1 descriptor and calls these two functions.
 */
#ifndef CROSS_DSP_H
#define CROSS_DSP_H

#include <stdint.h>

/* Parameter words, each 0-32767, as the OP-1 hands them to render. */
enum {
    TCH_P_CUTOFF = 0,    /* blue   CUTOFF    */
    TCH_P_RESONANCE = 1, /* green  RESONANCE */
    TCH_P_ENV = 2,       /* white  ENV       */
    TCH_P_DRIVE = 3,     /* red    DRIVE     */
    TCH_P_ENGINE = 4,    /* engine selector, owned by the firmware */
    TCH_P_WAVE = 5,      /* blue page 2:  WAVE   */
    TCH_P_DETUNE = 6,    /* green page 2: DETUNE */
    TCH_P_DECAY = 7      /* white page 2: DECAY  */
};

/* The voice's whole state. CROSS is monophonic, so the Kennel wrapper keeps
   a single one and lends it to whichever OP-1 voice plays. All zero is a
   valid start. */
typedef struct {
    uint32_t ph1;      /* VCO1 phase */
    uint32_t ph2;      /* VCO2 phase */
    int32_t lp[4];     /* ladder stages, Q15 */
    uint32_t env;      /* filter contour, Q23 */
    uint32_t flags;    /* TCH_F_* */
    uint32_t rng;      /* the drift's noise */
    int32_t wand[4];   /* each VCO's slow wander, two smoothing poles, Q24 ratio */
    int32_t nudge[2];  /* each VCO's offset for this note, Q24 ratio */
} tch_voice;

#define TCH_F_SUB 1u   /* sub-oscillator half-cycle */
#define TCH_F_DECAY 2u /* contour has passed its peak */
#define TCH_F_STILL 4u /* no pitch drift (measurements) */

#define TCH_BLOCK_STEP 16 /* samples between contour/cutoff updates */
#define TCH_ENV_ONE (1u << 23)

/* Restart the filter contour and draw this note's small pitch offsets.
   Phases run free, like analogue oscillators. */
void tch_note_on(tch_voice *v);

/*
 * Render n samples (a multiple of TCH_BLOCK_STEP, at most 128).
 * inc:  VCO1 phase increment for each sample (2^32 = one cycle).
 * w:    the eight parameter words.
 * gain: velocity gain, Q15: the level, and how far ENV opens the filter
 *       (30 % of it for the softest note, all of it at full velocity).
 */
void tch_render(tch_voice *v, int16_t *out, int n, const int32_t *inc,
                const int16_t *w, int32_t gain);

#endif
