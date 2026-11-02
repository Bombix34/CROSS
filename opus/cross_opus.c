/* CROSS for Opus 2: cross/cross.c as it is (its mono voice handling and its screen), built against op1_plugin.h here,
 * the Opus stand-in for Kennel's header, and registered with Opus's engine service. What Kennel's core did for CROSS,
 * this file does:
 *   voices  each Opus voice keeps its index (0-5), which cross.c's mono hand-off goes by
 *   page 2  a push on blue, green or white on T1 swaps that encoder to WAVE, DETUNE or DECAY (knobs 5, 6, 7), 256 a
 *           detent as Kennel's; another push swaps it back; a slot change starts on page 1 again
 *   screen  the T1 page is cross.c's screen, drawn every frame with opus_page_stroke_line
 * The DSP is cross/cross_dsp.c, compiled by cross_dsp_opus.c. */
#include "../cross/cross.c"

#define CX_PAGE_STEP 256                               /* page 2: knob units a detent */

struct cx_voice {
    int index;                                         /* first: op1_voice_index reads it */
};

static void v_init(void *voice, int index)
{
    ((struct cx_voice *)voice)->index = index;
    if (index >= 0 && index < OP1_VOICES) cx_held[index] = 0;   /* built again: its key is no longer down */
}

static void v_note_on(void *voice, const short *knobs) { tc_note_on(voice, knobs); }

static void v_release(void *voice, const short *knobs)
{
    (void)knobs;
    tc_note_off(voice);
}

/* cross.c writes a silent block as 32-bit words; Opus does not promise out is 4-byte aligned, and a misaligned word
   store stops the Blackfin: such a block goes through an aligned buffer */
static void v_render(void *voice, short *out, const short *par, const unsigned *pitch)
{
    cx_pitch = (const int32_t *)pitch;
    if (!((uintptr_t)out & 3)) {
        tc_render(voice, out, par);
        return;
    }
    int32_t tmp[OP1_BLOCK / 2];
    tc_render(voice, (int16_t *)tmp, par);
    for (int i = 0; i < OP1_BLOCK; i++) out[i] = ((const int16_t *)tmp)[i];
}

/* page 2 is one set of flags for the slot shown; it starts over when the slot changes, as Kennel's did */
static void page_follow(void)
{
    static int shown = -1;
    const int s = opus_sound_slot(0);
    if (s == shown) return;
    shown = s;
    for (int i = 0; i < 4; i++) op1_eng_page[i] = 0;
}

static int e_key(int key, int pressed, int page)
{
    if (page != 0 || key < 0x3B || key > 0x3D) return 0;   /* blue, green, white push on T1; orange's stays the OS's */
    page_follow();
    if (pressed) op1_eng_page[key - 0x3B] ^= 1;
    return 1;
}

static int e_turn(int encoder, int detents, int page)
{
    if (page != 0 || encoder < 0 || encoder > 2) return 0;
    page_follow();
    if (!op1_eng_page[encoder]) return 0;              /* page 1: the OS turns knobs 0-2 */
    if (detents) {
        short k[8];
        opus_engine_live(k);
        int32_t v = k[5 + encoder] + detents * CX_PAGE_STEP;
        opus_engine_knob_set(5 + encoder, v < 0 ? 0 : v > 32767 ? 32767 : v);
    }
    return 1;
}

static void page(void)
{
    page_follow();
    tc_screen_update(0, 0);
}

static const struct opus_engine CROSS = {
    .size = sizeof(struct opus_engine),
    .name = "cross",
    /* a new sound: CUTOFF, RESONANCE, ENV, DRIVE; knob 4 the core's; WAVE, DETUNE, DECAY (cross/plugin.json) */
    .knobs = { 11450, 13000, 19115, 11500, 0, 10923, 13900, 22400 },
    .voice = sizeof(struct cx_voice),
    .init = v_init,
    .note_on = v_note_on,
    .release = v_release,
    .render = v_render,
    .page = page,
    .turn = e_turn,
    .key = e_key,
};

int opl_init(struct opl_self *self)
{
    for (int k = 0; k < 8192; k++) {                   /* Kennel's firmware sine table, from opus_sincos */
        int a = k >> 1, s, c;
        if (a > 2048) a -= 4096;
        opus_sincos(a, &s, &c);
        s *= 8;
        cx_sine[k] = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
    }
    return opus_engine_add(self, &CROSS) < 0 ? -1 : 0;
}
