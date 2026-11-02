/* The Opus 2 stand-in for Kennel's op1_plugin.h: just what cross/cross.c uses, mapped onto Opus's documented calls
 * (opus_plugin.h), so cross.c builds for Opus unchanged. cross_opus.c includes cross.c and supplies the state below.
 *   voices   Opus hands each voice its own state; cross_opus.c keeps the voice's index (0-5) as its first word
 *   pitch    the render's per-sample phase steps, set by cross_opus.c around each render
 *   velocity the OS applies it to the level (Opus engines render one note's raw sound): full here
 *   screen   cross.c's lines become opus_page_stroke_line in the engine's T1 page; places stay in units (Q10 -> x256),
 *            widths in panel pixels as the OS's raster takes them (Q10 -> x256) */
#ifndef OP1_PLUGIN_H
#define OP1_PLUGIN_H
#include <stdint.h>
#include "opus_plugin.h"

#define OP1_BLOCK        128
#define OP1_VOICES       6

static inline int op1_voice_index(const void *self) { return *(const int *)self; }
static inline int32_t op1_voice_vel_gain(const void *self) { (void)self; return 32767; }

static const int32_t *cx_pitch;                        /* the voice being rendered: a phase step a sample */
#define OP1_PITCH_INC    cx_pitch

typedef struct {
    void (*note_on)(void *self, const int16_t *params);
    void (*render)(void *self, int16_t *out, const int16_t *params);
    void (*note_off)(void *self);
    uint8_t press[4];
} op1_engine_t;
enum { OP1_PRESS_NONE = 0, OP1_PRESS_PAGE = 1 };
static volatile uint8_t op1_eng_page[4];               /* page 2 on per knob (blue..red), kept by cross_opus.c */

/* ---- the screen ---- */
typedef struct { int32_t x, y; } op1_pt;                /* Q10 units */
typedef struct {
    void (*init)(void *scr);
    void (*update)(void *scr, int dt);
} op1_screen_t;

static int16_t cx_sine[8192];                          /* sin(2 pi k / 8192), Q15, filled by cross_opus.c */
#define OP1_SINE_TABLE   ((const int16_t *)cx_sine)

#define OP1_SCREEN_SLOT  0
#define op1_screen_draw(scr, dt) ((void)(scr), (void)(dt))   /* the core clears the page; cross.svg is empty */
#define op1_raster()     ((void *)0)

static inline void op1_raster_line(void *r, const op1_pt *a, const op1_pt *b, int32_t w_q10, const uint8_t *rgba)
{
    (void)r;
    opus_page_stroke_line(a->x >> 2, a->y >> 2, b->x >> 2, b->y >> 2, w_q10 >> 2,
                          OPUS_RGB((unsigned)rgba[0] << 16 | (unsigned)rgba[1] << 8 | rgba[2]));
}

/* the current sound's knobs, the LFO in while a note sounds (opus_engine_live) */
static short cx_knobs[8];
static inline void op1_synth_knobs_live(int slot, int16_t out[4])
{
    (void)slot;
    opus_engine_live(cx_knobs);
    for (int i = 0; i < 4; i++) out[i] = cx_knobs[i];
}
static inline const int16_t *op1_synth_knobs(int slot) { (void)slot; return cx_knobs; }

#endif
