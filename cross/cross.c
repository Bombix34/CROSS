/* "cross": a mono bass engine in the spirit of a classic three-oscillator ladder synth, for French-touch lines.
 * Page 1
 *   blue  (knob 0) CUTOFF:    ladder cutoff, 20 Hz .. 14 kHz, with 2/3 key tracking
 *   green (knob 1) RESONANCE: ladder resonance, bass kept
 *   white (knob 2) ENV:       how far the filter envelope opens the cutoff, 0 .. 6 octaves
 *   red   (knob 3) DRIVE:     mixer overload up to the middle, then post-filter drive; level-compensated
 * Page 2 (encoder press on blue, green, white; preset words 5, 6, 7)
 *   blue  WAVE:   saw -> saw + sub -> square + sub -> narrow pulse
 *   green DETUNE: VCO2 detune 0 .. 25 cents, then a fifth, then an octave
 *   white DECAY:  filter envelope decay, 5 ms .. 2 s
 * Monophonic in every play mode: one voice of DSP, lent to the newest key held (last-note priority); letting go of it
 * hands it back to the newest key still held, legato, without restarting the filter envelope. The amp envelope, glide
 * (the OP-1's mono / legato play modes) and the rest of the play modes are the OP-1's own. The DSP lives in
 * cross_dsp.c. */
#include "op1_plugin.h"
#include "cross_dsp.h"

/* ---- mono: the firmware still runs up to six voice objects, but only the owner renders the one tch_voice; the
 * others give silence, except that the voice which a new key just took it from fades its last sample out over a block
 * (the new voice's amp envelope starts from nothing), so cutting it never clicks. note_on / note_off run on the key / sequencer thread and the render preempts them: they only
 * write single words, and the restart of the envelope is left to the owner's next render. */
static tch_voice cx_v;                         /* the one voice */
static volatile int32_t cx_owner = -1;         /* voice object that renders it */
static volatile int32_t cx_retrig;             /* a note_on asks the owner's next render to restart the envelope */
static volatile uint32_t cx_held[OP1_VOICES];  /* per voice object: the order its key went down in, 0 = up */
static uint32_t cx_order;
static volatile uint32_t cx_tick, cx_seen[OP1_VOICES];   /* owner renders, and each voice object's latest render */
static volatile int32_t cx_prev = -1;          /* the voice object that rendered last ... */
static int32_t cx_fade = -1;                   /* ... and the one fading out */
static int16_t cx_last, cx_fade_from;          /* the owner's last sample, and where the fade starts */

volatile int32_t cross_env;                    /* the voice's filter envelope, Q15, for the screen */
volatile uint32_t cross_notes;                 /* one more per note played, for the screen's flash */

static void tc_note_on(void *self, const int16_t *params)
{
    const int v = op1_voice_index(self);
    (void)params;
    if (v < 0 || v >= OP1_VOICES) return;
    cx_held[v] = ++cx_order;
    cx_owner = v;
    cx_retrig = 1;
    cross_notes++;
}

static void tc_note_off(void *self)
{
    const int v = op1_voice_index(self);
    if (v < 0 || v >= OP1_VOICES) return;
    cx_held[v] = 0;
    if (cx_owner != v) return;
    /* back to the newest key still held and still sounding, legato; none: the owner keeps it for its release */
    uint32_t best = 0;
    int to = -1;
    for (int i = 0; i < OP1_VOICES; i++)
        if (cx_held[i] > best && cx_tick - cx_seen[i] <= 2) { best = cx_held[i]; to = i; }
    if (to >= 0) {                             /* its amp envelope is already up: the same wave goes on, no fade */
        cx_owner = to;
        cx_prev = to;
    }
}

static void tc_render(void *self, int16_t *out, const int16_t *params)
{
    const int v = op1_voice_index(self);
    if (v != cx_owner) {
        cx_seen[v] = cx_tick;
        if (v == cx_prev) {                    /* lost the voice before its new owner rendered */
            cx_fade = v;
            cx_fade_from = cx_last;
            cx_prev = -1;
        }
        if (v == cx_fade) {
            const int32_t a = cx_fade_from;
            for (int i = 0; i < OP1_BLOCK; i++) out[i] = (int16_t)((a * (OP1_BLOCK - 1 - i)) >> 7);
            cx_fade = -1;
        } else {
            uint32_t *o = (uint32_t *)out;
            for (int i = 0; i < OP1_BLOCK / 2; i++) o[i] = 0;
        }
        return;
    }
    if (cx_prev != v) {                        /* a new owner: the previous one fades from here */
        cx_fade = cx_prev;
        cx_fade_from = cx_last;
        cx_prev = v;
    }
    if (cx_retrig) {
        cx_retrig = 0;
        tch_note_on(&cx_v);
    }
    cx_seen[v] = ++cx_tick;
    tch_render(&cx_v, out, OP1_BLOCK, OP1_PITCH_INC, params, op1_voice_vel_gain(self));
    cx_last = out[OP1_BLOCK - 1];
    cross_env = (int32_t)(cx_v.env >> 8);
}

const op1_engine_t ENGINE_cross = { tc_note_on, tc_render, tc_note_off,
                                     { OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_NONE } };

/* ---- screen: cross.svg holds the floor (tools/gen_svg.mjs); everything else is drawn here
 * every frame. Each encoder owns its objects and paints them in its own colour, on both pages; nothing travels across
 * the screen.
 *   blue   an upside-down solid cross mid-screen, standing still on the floor, seen from straight below in
 *          perspective: wide near the floor, narrowing upwards, its depth showing under both arms of the crossbar.
 *          It fills with blue light (scan lines, one every third row) from the floor up to the cutoff (CUTOFF), the
 *          level's bright edge marking it, still while notes play; its front outline, alone, lights towards white
 *          with each note's filter envelope, and flashes white for a few frames on every note; the depth faces fill
 *          on the same rows, a deeper blue. WAVE (page 2) is the red wave's shape, and the sub's square under it
 *   green  one halo round the cross, following its back face, still, from faint to full with RESONANCE; DETUNE
 *          (page 2) brings in two paler, dashed copies of it that slide apart along the floor, one each way, as far
 *          as the screen's edges at full detune
 *   white  a solid line inside the cross, as far above the light as the envelope opens the filter (ENV). Between
 *          the light and the line, solid white rows on the scan lines' rows (DECAY, page 2) fade upward from the
 *          light (65 % white at most), reaching further up the longer the decay, all the way at its longest; the
 *          depth faces too
 *   red    behind everything, a wave of WAVE's shape standing still across the screen (DRIVE): squaring off and
 *          brightening up to the middle (mixer overload), then its flattened crests crackling more and more (post
 *          drive); while WAVE is being turned it is clean and blue, back to red and DRIVE's at once. Under it, an
 *          octave down, a still square in its colour shows the sub, as high and bright as the sub is loud in WAVE's mix
 *   top    no text: only the page dots (3 px), above the cross (the OP-1's bezel hides the bottom rows), two per
 *          paged knob in its colour (the filled one on the side of the page shown) and one red dot for DRIVE */
#pragma GCC push_options
#pragma GCC optimize("Os")                             /* GUI code: size, not speed */
#define QP(v) ((int32_t)((v) * 1024))
#define TC_CROSS_X 160                                 /* the cross's centre line, mid-screen */
#define TC_FOOT 125                                    /* the cross's foot, raised to show its underside */
#define TC_HEIGHT 104                                  /* the cross's height on screen, its top at y 21 */
#define TC_WAVE_N 127                                  /* the drive wave: points 5 px apart ... */
#define TC_WAVE_HALF 315                               /* ... over this much either side, before perspective */
#define TC_WAVE_Y 69                                   /* the drive wave's middle, centred behind the cross ... */
#define TC_WAVE_H 25                                   /* ... its half height ... */
#define TC_WAVE_TOP 20                                 /* ... and the rows its crackle and tears stay within */
#define TC_WAVE_LOW 134
#define TC_SUB_Y 103                                   /* the sub's square under the wave: its middle ... */
#define TC_SUB_H 5                                     /* ... and half height at full sub */
/* the cross, flat: half-widths in px at the foot, heights v as fractions of its length (Q12, 0 = foot) */
#define TC_STEM 22
#define TC_BAR 50
#define TC_BAR_V0 614                                  /* 0.15 */
#define TC_BAR_V1 1311                                 /* 0.32 */
#define TC_GROW_V (4096 / TC_HEIGHT)                   /* v per px a halo grows by */
#define TC_SHIFT_BASE 7                                /* the back face: px below the front at the foot ... */
#define TC_SHIFT_Y 20                                  /* ... and this much more at the top */
#define TC_SCAN 3                                      /* the light's scan lines: one on every third pixel row */
#define TC_HALO_O 4                                    /* px the halo's outline is grown by ... */
#define TC_HALO_DROP 6                                 /* ... and how much lower its foot sits, off the cross's */
#define TC_DOT_Y 12                                    /* the page dots' row, above the cross */
static const uint8_t C_BLUE[4] = { 0x69, 0x8E, 0xFF, 0xFF }, C_GREEN[4] = { 0x00, 0xED, 0x95, 0xFF },
                     C_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, C_RED[4] = { 0xFF, 0x42, 0x3F, 0xFF },
                     C_DIM[4] = { 0x62, 0x5D, 0x77, 0xFF }, C_BLACK[4] = { 0, 0, 0, 0xFF };

static uint32_t tc_frame;
static int32_t tc_dx, tc_dy;                          /* DRIVE's glitch: the whole picture's shake, Q10 ... */
static int32_t tc_band0, tc_band1, tc_band_dx;         /* ... and a band of rows torn sideways */
static int32_t tc_shake_left;                          /* frames the shake still holds */
static uint32_t tc_vg_seed;                            /* the shapes' vertex glitch: which vertices jump where ... */
static int32_t tc_vg_amp, tc_vg_left;                  /* ... how far (Q10, 0 = none) and for how many more frames */
static int32_t tc_w5_last = -1, tc_w5_touch;          /* WAVE's last word, and frames left of showing it was turned */
static uint32_t tc_notes_seen;                         /* cross_notes at the last frame ... */
static int32_t tc_strike;                              /* ... and the outline's strike, Q8, 256 on a new note */

/* every line goes through here, shaken and torn by DRIVE's glitch */
static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    const int32_t my = (y0 + y1) >> 1, ox = tc_dx + (my >= tc_band0 && my < tc_band1 ? tc_band_dx : 0);
    const op1_pt a = { x0 + ox, y0 + tc_dy }, b = { x1 + ox, y1 + tc_dy };
    op1_raster_line(r, &a, &b, w, c);
}

/* a well-stirred 32-bit hash of two counters, for the drive wave's chaos */
static uint32_t tc_hash(uint32_t a, uint32_t b)
{
    uint32_t x = a * 2654435761u ^ b * 2246822519u;
    x ^= x >> 15;
    x *= 2654435761u;
    return x ^ (x >> 13);
}

/* a + (b - a) * t / 256 per channel */
static void mix(uint8_t *o, const uint8_t *a, const uint8_t *b, int32_t t)
{
    for (int i = 0; i < 3; i++) o[i] = (uint8_t)(a[i] + (((b[i] - a[i]) * t) >> 8));
    o[3] = 0xFF;
}

/* ---- the cross in perspective: a point v along its length (Q12, 0 at the foot) is 1 + v times further away
 * than the foot, so it is drawn 1 / (1 + v) as wide and the upper part is foreshortened ---- */
static int32_t persp_depth(int32_t v) { return 4096 + v; }                          /* Q12, 1 + v */

static int32_t persp_y(int32_t v)                       /* screen y, Q10 */
{
    const int32_t f = ((v * 2) << 12) / persp_depth(v);                             /* 2 v / (1 + v), Q12 */
    return QP(TC_FOOT) - (((QP(TC_HEIGHT) >> 6) * f) >> 6);
}

static int32_t persp_x(int32_t cx, int32_t hw, int32_t v)   /* hw: flat half-width, Q10 */
{
    return cx + (((hw >> 2) * ((4096 << 12) / persp_depth(v))) >> 10);
}

/* DRIVE's vertex glitch: while it is on, about 3 vertices in 5 jump by up to the glitch's reach across and half of
   it up or down, each its own way (id tells the vertices apart), the same for every frame of the glitch */
static void glitch_pt(int32_t *x, int32_t *y, uint32_t id)
{
    if (!tc_vg_amp) return;
    const uint32_t h = tc_hash(tc_vg_seed, id);
    if ((h >> 24) < 150) {
        *x += ((int32_t)((h >> 8) & 255) - 128) * tc_vg_amp >> 7;
        *y += ((int32_t)((h >> 16) & 255) - 128) * tc_vg_amp >> 8;
    }
}

/* the outline grown by o px, clockwise from the top left (the crossbar near the foot: the cross stands upside down),
   with DRIVE's vertex glitch */
static void cross_points(int32_t cx, int32_t o, int32_t *px, int32_t *py)
{
    const int32_t s = QP(TC_STEM + o), b = QP(TC_BAR + o), top = 4096 + o * TC_GROW_V, v1 = TC_BAR_V1 + o * TC_GROW_V;
    int32_t v0 = TC_BAR_V0 - o * TC_GROW_V;
    if (v0 < 0) v0 = 0;
    const int32_t x[12] = { -s, s, s, b, b, s, s, -s, -s, -b, -b, -s };
    const int32_t v[12] = { top, top, v1, v1, v0, v0, 0, 0, v0, v0, v1, v1 };
    for (int i = 0; i < 12; i++) {
        px[i] = persp_x(cx, x[i], v[i]);
        py[i] = persp_y(v[i]);
        glitch_pt(&px[i], &py[i], (uint32_t)(i + 16 * o));
    }
}

/* the back face of the point (px, py), seen from straight below: further away, so drawn in towards the cross's
   centre line, and lower, the more so the higher it stands; even the foot shows its underside */
static void back_point(int32_t cx, int32_t *px, int32_t *py)
{
    const int32_t t = QP(TC_FOOT) - *py;               /* height above the foot, Q10 */
    *px = cx + (((*px - cx) * 3768) >> 12);
    *py += QP(TC_SHIFT_BASE) + (t * TC_SHIFT_Y) / TC_HEIGHT;
}

/* where row y (Q10) crosses a polygon whose every row is a single run (all of ours): its ends, or 0 if it misses */
static int row_span(const int32_t *px, const int32_t *py, int n, int32_t y, int32_t *x0, int32_t *x1)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const int32_t a = py[i], b = py[j];
        if ((a <= y && b > y) || (b <= y && a > y)) {
            const int32_t dy = (b - a) >> 8;
            const int32_t x = dy ? px[i] + ((px[j] - px[i]) >> 4) * ((y - a) >> 4) / dy : px[i];
            if (!k || x < *x0) *x0 = x;
            if (!k || x > *x1) *x1 = x;
            k++;
        }
    }
    return k >= 2;
}

/* the cross's runs on each pixel row (Q10, sampled mid-row; a > b: none): its front face, and its whole silhouette
   (front, back and side faces). What lies behind is drawn only outside them, instead of under black fills */
static int32_t tc_fa[160], tc_fb[160], tc_sa[160], tc_sb[160];

static void solid_spans(int32_t cx)
{
    int32_t fx[12], fy[12], bx[12], by[12], a, b;
    cross_points(cx, 0, fx, fy);
    for (int i = 0; i < 12; i++) { bx[i] = fx[i]; by[i] = fy[i]; back_point(cx, &bx[i], &by[i]); }
    for (int row = 0; row < 160; row++) {
        const int32_t y = QP(row) + 512;
        int32_t sa = 1, sb = 0;
        int any = row_span(fx, fy, 12, y, &sa, &sb);
        tc_fa[row] = any ? sa : 1;
        tc_fb[row] = any ? sb : 0;
        for (int i = -1; i < 12; i++) {                /* the back face, then each side face */
            const int j = (i + 1) % 12;
            const int32_t qx[4] = { fx[i < 0 ? 0 : i], fx[j], bx[j], bx[i < 0 ? 0 : i] },
                          qy[4] = { fy[i < 0 ? 0 : i], fy[j], by[j], by[i < 0 ? 0 : i] };
            if (i < 0 ? row_span(bx, by, 12, y, &a, &b) : row_span(qx, qy, 4, y, &a, &b)) {
                if (!any || a < sa) sa = a;
                if (!any || b > sb) sb = b;
                any = 1;
            }
        }
        tc_sa[row] = any ? sa : 1;
        tc_sb[row] = any ? sb : 0;
    }
}

static int outside(const int32_t *ta, const int32_t *tb, int32_t x, int32_t y)
{
    const int row = y >> 10;
    return row < 0 || row >= 160 || x <= ta[row] || x >= tb[row];
}

/* a line passing behind runs ta .. tb: only its stretches outside them, sampled about every 2 px, each stretch's
   ends found by bisection */
static void ln_behind(void *r, const int32_t *ta, const int32_t *tb, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1, int32_t w, const uint8_t *c)
{
    const int32_t dx = x1 - x0, dy = y1 - y0;
    int n = ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) >> 11;
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    const int32_t step = 4096 / n;
    int was = outside(ta, tb, x0, y0);
    int32_t s = 0, t0 = 0;
    for (int k = 1; k <= n; k++) {
        const int32_t t1 = k == n ? 4096 : k * step;
        const int is = outside(ta, tb, x0 + ((dx >> 4) * t1 >> 8), y0 + ((dy >> 4) * t1 >> 8));
        if (is != was) {
            int32_t lo = t0, hi = t1;                  /* the crossing, to 1/16 of the step */
            for (int b = 0; b < 4; b++) {
                const int32_t m = (lo + hi) >> 1;
                if (outside(ta, tb, x0 + ((dx >> 4) * m >> 8), y0 + ((dy >> 4) * m >> 8)) == was) lo = m;
                else hi = m;
            }
            if (was) ln(r, x0 + ((dx >> 4) * s >> 8), y0 + ((dy >> 4) * s >> 8), x0 + ((dx >> 4) * lo >> 8),
                        y0 + ((dy >> 4) * lo >> 8), w, c);
            else s = hi;
            was = is;
        }
        t0 = t1;
    }
    if (was) ln(r, x0 + ((dx >> 4) * s >> 8), y0 + ((dy >> 4) * s >> 8), x1, y1, w, c);
}

/* the halo: the outline grown by TC_HALO_O px, set on the back face (its shape, not a solid), widened by 10 % and then
   grown by a quarter about the foot, its foot dropped TC_HALO_DROP px more (its top stays). slide (Q15, 0: the halo
   itself, solid) makes a dashed copy slid to the right (to the left when negative), the whole slide at the foot and
   half at the top as if sliding along the floor in perspective, the full slide bringing its edge to the screen's */
static void halo_outline(void *r, int32_t cx, int32_t slide, const uint8_t *c)
{
    int32_t px[12], py[12], pt[12];
    cross_points(cx, TC_HALO_O, px, py);
    for (int i = 0; i < 12; i++) {
        back_point(cx, &px[i], &py[i]);
        px[i] = cx + (((px[i] - cx) * 5632) >> 12);
        py[i] = QP(TC_FOOT) - (((QP(TC_FOOT) - py[i]) * 5) >> 2);
        int32_t t = (py[i] - QP(TC_FOOT - TC_HEIGHT)) / TC_HEIGHT;     /* Q10: 0 at the cross's top, 1 at its foot */
        if (t < 0) t = 0;
        if (t > 1024) t = 1024;
        py[i] += (QP(TC_HALO_DROP) * t) >> 10;
        pt[i] = 512 + (t >> 1);                                        /* Q10: the share of the slide here */
    }
    if (slide) {
        const int left = slide < 0;
        int32_t most = QP(400);                                        /* the slide that meets the screen's edge */
        for (int i = 0; i < 12; i++) {
            const int32_t m = ((left ? px[i] - QP(2) : QP(318) - px[i]) << 10) / pt[i];
            if (m < most) most = m;
        }
        most = ((most >> 5) * ((left ? -slide : slide) >> 5)) >> 5;
        for (int i = 0; i < 12; i++) px[i] += ((left ? -most : most) * pt[i]) >> 10;
    }
    int32_t phase = 0;                                                 /* the dashes run on round the corners */
    for (int i = 0; i < 12; i++) {
        const int j = (i + 1) % 12;
        if (!slide) {
            ln_behind(r, tc_sa, tc_sb, px[i], py[i], px[j], py[j], QP(1.5), c);
            continue;
        }
        const int32_t dx = px[j] - px[i], dy = py[j] - py[i], ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        const int32_t len = ax > ay ? ax + (ay >> 1) : ay + (ax >> 1);  /* near enough */
        if (len < QP(0.5)) continue;
        for (int32_t d = -phase; d < len; d += QP(9)) {                /* 5 px dashes, 4 px gaps */
            const int32_t a = d < 0 ? 0 : d, b = d + QP(5) > len ? len : d + QP(5);
            if (b <= a) continue;
            ln_behind(r, tc_sa, tc_sb, px[i] + ((dx >> 4) * (a >> 4)) / (len >> 8),
                      py[i] + ((dy >> 4) * (a >> 4)) / (len >> 8), px[i] + ((dx >> 4) * (b >> 4)) / (len >> 8),
                      py[i] + ((dy >> 4) * (b >> 4)) / (len >> 8), QP(1.5), c);
        }
        phase = (phase + len) % QP(9);
    }
}

/* fill the front face (Q10 points) from row y_from down with the light's scan lines: one raster line on every
   TC_SCAN-th pixel row of the screen. Solid fills would cost the core their whole area every frame */
static void fill_lines(void *r, const int32_t *px, const int32_t *py, int n, int32_t y_from, const uint8_t *c)
{
    int32_t y0 = py[0], y1 = py[0], a, b;
    for (int i = 1; i < n; i++) { if (py[i] < y0) y0 = py[i]; if (py[i] > y1) y1 = py[i]; }
    if (y0 < y_from) y0 = y_from;
    for (int row = ((y0 >> 10) + TC_SCAN - 1) / TC_SCAN * TC_SCAN; QP(row) + 512 < y1; row += TC_SCAN) {
        const int32_t y = QP(row) + 512;
        if (y >= y0 && row_span(px, py, n, y, &a, &b)) ln(r, a, y, b, y, QP(1.2), c);
    }
}

/* one scan line of the depth faces, on pixel row row: where the cross's silhouette shows round its front face */
static void depth_row(void *r, int row, const uint8_t *c)
{
    if (row < 0 || row >= 160 || tc_sa[row] > tc_sb[row]) return;
    const int32_t y = QP(row) + 512, a = tc_sa[row], b = tc_sb[row];
    if (tc_fa[row] > tc_fb[row]) {
        ln(r, a, y, b, y, QP(1.2), c);
        return;
    }
    if (tc_fa[row] - a > QP(0.25)) ln(r, a, y, tc_fa[row], y, QP(1.2), c);
    if (b - tc_fb[row] > QP(0.25)) ln(r, tc_fb[row], y, b, y, QP(1.2), c);
}

/* the solid cross, drawn as light and edges only: what lies behind it (the drive wave, the halo) is clipped to its
   silhouette (ln_behind), so it needs no black fill. The light is a level inside it, at the height of row light_y:
   its scan lines fill the front face below that height, with a bright edge on top, and the depth faces on the same
   rows in the deeper fill_d. The envelope's line sits at row env_y, solid (env_c); between it and the light, DECAY's
   lines on the same rows, front and depth, white (65 % at most) fading upward from the light, reaching a tenth of
   the way at no decay (Q15), about all of it at half, and nearly its brightest all the way at full decay; glow and
   edge paint the outline */
static void cross_solid(void *r, int32_t cx, int32_t light_y, int32_t env_y, int32_t decay, const uint8_t *fill,
                        const uint8_t *fill_d, const uint8_t *top, const uint8_t *env_c, const uint8_t *side, const uint8_t *glow,
                        const uint8_t *edge)
{
    int32_t fx[12], fy[12], bx[12], by[12], a, b;
    cross_points(cx, 0, fx, fy);
    for (int i = 0; i < 12; i++) { bx[i] = fx[i]; by[i] = fy[i]; back_point(cx, &bx[i], &by[i]); }
    for (int i = 0; i < 12; i++) {                     /* the depth's edges, where the front face leaves them showing */
        const int j = (i + 1) % 12;
        ln_behind(r, tc_fa, tc_fb, bx[i], by[i], bx[j], by[j], QP(1), side);
        ln_behind(r, tc_fa, tc_fb, fx[i], fy[i], bx[i], by[i], QP(1), side);
    }
    for (int row = ((light_y >> 10) + TC_SCAN - 1) / TC_SCAN * TC_SCAN; row < 160; row += TC_SCAN)
        if (QP(row) + 512 >= light_y) depth_row(r, row, fill_d);
    fill_lines(r, fx, fy, 12, light_y, fill);
    if (row_span(fx, fy, 12, light_y, &a, &b)) ln(r, a, light_y, b, light_y, QP(2), top);
    if (env_y < light_y - QP(2) && row_span(fx, fy, 12, env_y, &a, &b))
        ln(r, a + QP(2), env_y, b - QP(2), env_y, QP(1.65), env_c);
    const int32_t d2 = (decay * decay) >> 15, d6 = (((d2 * d2) >> 15) * d2) >> 15;
    const int32_t span = light_y - env_y, reach = 410 + ((decay * 3686) >> 15) + ((d6 * 28672) >> 15);   /* Q12 */
    for (int row = (env_y >> 10) / TC_SCAN * TC_SCAN + TC_SCAN; span > QP(4) && QP(row) + 512 < light_y - QP(2);
         row += TC_SCAN) {
        const int32_t y = QP(row) + 512;
        if (y < env_y + QP(2)) continue;
        const int32_t f = ((light_y - y) << 2) / (span >> 10);        /* Q12: 0 at the light, 1 at ENV's line */
        const int32_t lvl = 166 - (f * 166) / reach;                   /* 65 % white at most */
        if (lvl < 8) continue;                                         /* out of DECAY's reach: not drawn */
        uint8_t c[4];
        mix(c, C_BLACK, C_WHITE, lvl);
        if (row_span(fx, fy, 12, y, &a, &b)) ln(r, a + QP(2), y, b - QP(2), y, QP(1.2), c);
        depth_row(r, row, c);
    }
    for (int i = 0; i < 12; i++) {
        const int j = (i + 1) % 12;
        ln(r, fx[i], fy[i], fx[j], fy[j], QP(4), glow);
    }
    for (int i = 0; i < 12; i++) {
        const int j = (i + 1) % 12;
        ln(r, fx[i], fy[i], fx[j], fy[j], QP(1.5), edge);
    }
}

/* the oscillator shape the DSP plays, one cycle per 8192 units, Q12: a saw minus m (Q15) of a saw shifted by pw */
static int32_t osc_shape(int32_t t, int32_t m, int32_t pw)
{
    const int32_t a = (t & 8191) - 4096, b = ((t + pw) & 8191) - 4096;
    return a - ((m * b) >> 15);
}

/* soft clip for the drive waves: x (Q12, |x| <= 3) -> tanh-like Q12 up to 1.0, x (27 + x^2) / (27 + 9 x^2) */
static int32_t soft(int32_t x)
{
    if (x > 3 * 4096) x = 3 * 4096;
    if (x < -3 * 4096) x = -3 * 4096;
    const int32_t x2 = (x * x) >> 12;
    return x * (27 * 4096 + x2) / (27 * 4096 + 9 * x2);
}

/* where a point of the drive wave lands on the plane behind the halos: at row y it is as far away as the cross's
   back face there, so drawn in towards the cross's centre line by the same perspective (Q10 in and out) */
static int32_t wave_plane_x(int32_t x, int32_t y)
{
    const int32_t u = ((QP(TC_FOOT) - y) * 4) / TC_HEIGHT;            /* Q12, the height as the cross sees it */
    const int32_t v = (u << 12) / (8192 - u);                          /* back to a point along the cross */
    const int32_t sc = (((4096 << 12) / persp_depth(v)) * 3768) >> 12; /* its scale, and the back face's */
    return QP(TC_CROSS_X) + ((((x - QP(TC_CROSS_X)) >> 2) * sc) >> 10);
}

/* the end (x, y) of a segment towards (ox, oy), pulled back along it to the drive wave's margins, x 8 .. 312 */
static void cut_x(int32_t *x, int32_t *y, int32_t ox, int32_t oy)
{
    const int32_t xe = *x < QP(8) ? QP(8) : *x > QP(312) ? QP(312) : *x, d = (ox - *x) >> 8;
    if (xe == *x) return;
    if (d) *y += ((oy - *y) >> 4) * ((xe - *x) >> 4) / d;
    *x = xe;
}

/* a segment of the drive wave's plane (points already in perspective), behind the cross, cut at the margins; what
   a cut leaves under 2 px long is dropped, not left as a stray speck at the screen's edge */
static void wave_seg(void *r, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t w, const uint8_t *c)
{
    if ((ax < QP(8) && bx < QP(8)) || (ax > QP(312) && bx > QP(312))) return;
    const int32_t ax0 = ax, bx0 = bx;
    cut_x(&ax, &ay, bx, by);
    cut_x(&bx, &by, ax, ay);
    if ((ax != ax0 || bx != bx0) && (bx > ax ? bx - ax : ax - bx) + (by > ay ? by - ay : ay - by) < QP(2)) return;
    ln_behind(r, tc_sa, tc_sb, ax, ay, bx, by, w, c);
}

/* a dot of radius 3 px in colour c: an outline (twelve strokes, 1.5 px), filled by one square stroke inside it */
static void dot(void *r, int32_t x, int32_t y, int filled, const uint8_t *c)
{
    if (filled) ln(r, x - QP(2.1), y, x + QP(2.1), y, QP(4.2), c);
    int32_t px = x + QP(3), py = y;
    for (int s = 1; s <= 12; s++) {
        const int32_t a = s * 8192 / 12;
        const int32_t qx = x + ((QP(3) >> 5) * OP1_SINE_TABLE[(a + 2048) & 8191] >> 10);
        const int32_t qy = y + ((QP(3) >> 5) * OP1_SINE_TABLE[a & 8191] >> 10);
        ln(r, px, py, qx, qy, QP(1.5), c);
        px = qx; py = qy;
    }
}

/* the page dots, one group per encoder, centred in its quarter of the screen (x 40, 120, 200, 280): a knob with a
   page 2 gets two dots in its colour, the filled one on the side of the page shown (left page 1, right page 2);
   DRIVE, with none, one filled red dot */
static void page_dots(void *r)
{
    static const uint8_t *const col[3] = { C_BLUE, C_GREEN, C_WHITE };
    for (int i = 0; i < 3; i++) {
        const int32_t x = QP(40 + i * 80), pg = op1_eng_page[i] != 0;
        dot(r, x - QP(5.5), QP(TC_DOT_Y), !pg, col[i]);
        dot(r, x + QP(5.5), QP(TC_DOT_Y), pg, col[i]);
    }
    dot(r, QP(280), QP(TC_DOT_Y), 1, C_RED);
}

static void tc_screen_init(void *scr) { (void)scr; }

static void tc_screen_update(void *scr, int dt)
{
    int16_t kn[4];
    op1_synth_knobs_live(OP1_SCREEN_SLOT & 7, kn);
    const int16_t *words = op1_synth_knobs(OP1_SCREEN_SLOT & 7);
    int32_t k[4], p2[3];
    for (int i = 0; i < 4; i++) k[i] = kn[i] < 0 ? 0 : kn[i];
    for (int i = 0; i < 3; i++) p2[i] = words[5 + i] < 0 ? 0 : words[5 + i];
    const int32_t env = cross_env < 0 ? 0 : (cross_env > 32768 ? 32768 : cross_env);
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    uint8_t c[4];

    /* WAVE's morph, as cross_dsp.c: m blends in the shifted saw, pw is its shift; subl is the sub's level (Q15),
       coming in over the first third, full, then down to half as the pulse narrows */
    int32_t m, pw = 4096, subl;
    {
        const int32_t p = p2[0];
        if (p < 10923) { m = 0; subl = p * 3; }
        else if (p < 21845) { m = (p - 10923) * 3; subl = 32767; }
        else { m = 32767; pw = 4096 - (((p - 21845) * 3 * 95) >> 10); subl = 32767 - (((p - 21845) * 3) >> 1); }
        if (m > 32767) m = 32767;
        if (subl > 32767) subl = 32767;
    }

    /* DETUNE's slide of the halo's copy, following cross_dsp.c's detune table: 0 .. 25 cents, then a fifth, then an
       octave */
    const int32_t ds = p2[1] >> 8;
    const int32_t apart = ds >= 112 ? 32767 : ds >= 96 ? 26214 : (ds * 19661) / 96;  /* Q15 of the way to the edge */

    tc_frame++;

    /* DRIVE's glitch on the shapes and the whole picture. Its level gl (Q12) is nothing below 50 %, creeps up (as
       a square) to 0.4 at 85 % and then climbs steeply to 0.9 at 100 %. Every other frame a glitch may start, the
       more often the higher gl (from 4 in 256 to 204 in 256): for 1 .. 4 frames the vertices of the cross, its
       halo jump by up to 1 + 9 gl px. Four glitches in five also shake the whole picture
       (up to 1.8 gl px across, half that up or down, for 1 .. 2 frames, sometimes with a band of rows torn up to
       3 gl px sideways); one shake in five comes on its own */
    const int32_t pre = k[3] < 16384 ? k[3] : 16384, post = ((k[3] - pre) * 16) / 25; /* drawn: 100 % looks like 82 % */
    const int32_t mess = post < 7208 ? post : 7208;                    /* the wave's disorder: DRIVE's 72 % at most */
    int32_t gl = 0;
    if (post) {
        const int32_t a = post >> 2;                                   /* Q12 of the way from 50 % to 100 % */
        if (a < 2867) {                                                /* up to 85 % */
            const int32_t q = (a << 12) / 2867;
            gl = (1843 * ((q * q) >> 12)) >> 12;
        } else {
            gl = 1843 + ((a - 2867) * 2253) / 1229;
        }
        if (gl > 4096) gl = 4096;
        gl = (gl * 3686) >> 12;                                        /* 90 % of the full curve */
    }
    if (tc_vg_left) tc_vg_left--;
    else tc_vg_amp = 0;
    if (tc_shake_left) tc_shake_left--;
    else tc_dx = tc_dy = tc_band0 = tc_band1 = 0;
    if (gl && !(tc_frame & 1)) {
        const uint32_t h = tc_hash(tc_frame, 40);
        const int32_t chance = 4 + ((gl * 200) >> 12);
        int shake = 0;
        if (!tc_vg_left && (int32_t)(h >> 24) < chance) {
            tc_vg_seed = h;
            tc_vg_amp = QP(1) + ((gl * QP(9)) >> 12);
            tc_vg_left = 1 + (int32_t)((h >> 8) & 3);
            shake = (int32_t)((h >> 12) & 255) < 205;                  /* 80 % of the glitches shake too */
        } else if (!tc_vg_left) {
            shake = (int32_t)((h >> 16) & 255) < chance / 4;           /* and some shakes come alone */
        }
        if (shake && !tc_shake_left) {
            const uint32_t h2 = tc_hash(tc_frame, 41);
            const int32_t amp = (QP(1.8) * gl) >> 12;
            tc_shake_left = 1 + (int32_t)((h2 >> 24) & 1);
            tc_dx = ((int32_t)(h2 & 255) - 128) * amp >> 7;
            tc_dy = ((int32_t)((h2 >> 8) & 255) - 128) * (amp >> 1) >> 7;
            if (gl > 1024 && ((h2 >> 16) & 1)) {
                tc_band0 = QP(8 + (int32_t)((h2 >> 17) % 120));
                tc_band1 = tc_band0 + QP(6 + (int32_t)((h2 >> 4) % 10));
                tc_band_dx = ((int32_t)((h2 >> 20) & 255) - 128) * ((QP(3) * gl) >> 12) >> 7;
            }
        }
    }
    solid_spans(QP(TC_CROSS_X));                       /* this frame's cross, glitch and all, for what passes behind */

    /* red, behind everything, on the plane behind the halo: one cycle after another of WAVE's shape, standing still
       and unbroken (blue while WAVE is being turned, red again 8 frames after the last step, at
       once, never faded), 50 px high about y 69 (centred behind the cross, the sub's square under it, above the
       halo), drawn in the cross's perspective (narrowing towards the top, about the cross's centre line),
       spanning the screen. While WAVE is being turned it is clean whatever DRIVE: WAVE's own
       shape, no crackle, tears, flicker, shake, lightning or static. Otherwise it follows DRIVE's two
       stages. Up to the middle (mixer overload) its soft clip gain rises 0.5 .. 6, squaring it off, and it turns
       from dark to bright red; past the middle (post drive) it falls apart, the disorder growing with the knob up
       to 72 % and holding there, while the peaks keep rising and the light keeps brightening to 100 %:
       - its crests crackle, the jagged part spreading from 7/8 of the height to about a half, teeth up to 8 px;
       - it thickens to 2.5 px and its red heats towards white, flickering every frame;
       - 32 px stretches tear up or down by up to 10 px, re-torn every 4 frames.
       Its lightning and static are drawn last, in front of everything. It stays between y 20 and 134 */
    if (p2[0] != tc_w5_last) {                                         /* WAVE turned: blue while it is set */
        if (tc_w5_last >= 0) tc_w5_touch = 8;                         /* holds about 0.13 s past the last step */
        tc_w5_last = p2[0];
    } else if (tc_w5_touch) {
        tc_w5_touch--;
    }
    const int clean = tc_w5_touch != 0;
    const int32_t wpost = clean ? 0 : post, wmess = clean ? 0 : mess;
    uint8_t dc[4];
    int32_t wx[TC_WAVE_N], wy[TC_WAVE_N];
    {
        const int32_t g = clean ? 128 : 128 + ((pre * 1408) >> 14);  /* Q8 */
        const int32_t full = soft(g << 4);                             /* the peak, to keep the height steady */
        const int32_t grit = wpost >> 1;                               /* Q10, 0 .. 8 px */
        const int32_t edge_q12 = 3584 - ((wmess * 2150) >> 14);        /* where the crackle starts, of the height */
        const int32_t tear = wmess >> 7;                               /* chance of a torn stretch, of 256 */
        const uint32_t now = tc_frame * 40503u;
        mix(dc, C_BLACK, C_RED, 70 + ((pre * 130) >> 14) + ((wpost * 56) >> 14));
        mix(dc, dc, C_WHITE, ((wpost * 120) >> 14) + (((tc_hash(tc_frame, 3) >> 24) * wmess) >> 16));
        if (tc_w5_touch) mix(dc, dc, C_BLUE, 256);
        for (int j = 0; j < TC_WAVE_N; j++) {
            int32_t base = osc_shape(j * 426, m, pw);                  /* WAVE's shape, Q12 */
            if (base > 4096) base = 4096;
            if (base < -4096) base = -4096;
            const int32_t v = soft((base * g) >> 8);
            int32_t y = QP(TC_WAVE_Y) - (v * QP(TC_WAVE_H)) / full;
            if (grit && (v < 0 ? -v : v) * 4096 > edge_q12 * full)
                y += ((int32_t)(((uint32_t)j * 2654435761u + now) >> 24) - 128) * grit >> 7;
            if (tear) {
                const uint32_t slice = tc_hash((uint32_t)j / 6, tc_frame >> 2);
                if ((int32_t)(slice >> 24) < tear)
                    y += ((int32_t)((slice >> 8) & 255) - 128) * QP(10) >> 7;
            }
            if (y > QP(TC_WAVE_LOW)) y = QP(TC_WAVE_LOW);
            if (y < QP(TC_WAVE_TOP)) y = QP(TC_WAVE_TOP);
            wy[j] = y;
            wx[j] = wave_plane_x(QP(TC_CROSS_X - TC_WAVE_HALF + 5 * j), y);
        }
        /* one unbroken line: the perspective pulls a crest further in than the trough beside it, so a segment can
           cross the margins (x 8, 312) and is cut there, not dropped; DRIVE's torn band of rows leaves it whole */
        const int32_t keep[4] = { tc_band0, tc_band1, tc_dx, tc_dy };
        tc_band0 = tc_band1 = 0;
        if (clean) tc_dx = tc_dy = 0;
        for (int j = 1; j < TC_WAVE_N; j++)
            wave_seg(r, wx[j - 1], wy[j - 1], wx[j], wy[j], QP(1.5) + (wpost >> 4), dc);

        /* the sub, an octave down, under the wave: a square flipping at each of the wave's cycles (where VCO1
           wraps), standing still and clean, in the wave's colour; its height and light follow the sub's level in
           WAVE's mix; below a tenth (half a pixel, barely heard) it is not drawn at all, never as a flat line */
        if (subl >= 3277) {
            const int32_t h = (QP(TC_SUB_H) >> 5) * subl >> 10, x0 = QP(TC_CROSS_X - TC_WAVE_HALF);
            uint8_t sc[4];
            mix(sc, C_BLACK, dc, 96 + ((subl * 160) >> 15));
            int32_t xa = x0, ya = QP(TC_SUB_Y) - h;
            for (int k = 1;; k++) {
                const int32_t xe = x0 + (k * QP(5) * 8192) / 426;               /* the wave's k-th wrap */
                const int last = xe >= QP(TC_CROSS_X + TC_WAVE_HALF);
                const int32_t xb = last ? QP(TC_CROSS_X + TC_WAVE_HALF) : xe, yb = 2 * QP(TC_SUB_Y) - ya;
                wave_seg(r, wave_plane_x(xa, ya), ya, wave_plane_x(xb, ya), ya, QP(1.2), sc);
                if (last) break;
                wave_seg(r, wave_plane_x(xe, ya), ya, wave_plane_x(xe, yb), yb, QP(1.2), sc);
                xa = xe;
                ya = yb;
            }
        }
        tc_band0 = keep[0]; tc_band1 = keep[1]; tc_dx = keep[2]; tc_dy = keep[3];
    }

    /* blue: only the cross's front outline (glow, edge) lights up with each note; the depth's edges, the light and
       ENV's and DECAY's lines keep their colours. It follows the filter envelope, and every note also strikes it
       to full white for a few frames, so close notes each show even while the envelope stays high */
    if (cross_notes != tc_notes_seen) {
        tc_notes_seen = cross_notes;
        tc_strike = 256;
    } else {
        tc_strike = (tc_strike * 5) >> 3;
    }
    const int32_t fl = env >> 7;                       /* 0 .. 256 */
    int32_t glow_t = 70 + ((fl * 120) >> 8), edge_t = (fl * 170) >> 8;
    glow_t += ((256 - glow_t) * tc_strike) >> 8;
    edge_t += ((255 - edge_t) * tc_strike) >> 8;
    uint8_t glow[4], edge[4], side[4];
    mix(glow, C_BLACK, C_BLUE, glow_t);
    mix(edge, C_BLUE, C_WHITE, edge_t);
    mix(side, C_BLACK, C_BLUE, 170);

    /* the light's level: CUTOFF fills the cross's height (9.5 octaves) from the floor, still while notes play; ENV's
       6 octaves reach 5/8 of the height above it, no higher than the top */
    const int32_t x = QP(TC_CROSS_X);
    const int32_t light_y = QP(TC_FOOT) - ((k[0] * TC_HEIGHT) >> 5);
    int32_t env_y = light_y - ((k[2] * (TC_HEIGHT * 5 / 8)) >> 5);
    if (env_y < QP(TC_FOOT - TC_HEIGHT + 1)) env_y = QP(TC_FOOT - TC_HEIGHT + 1);

    /* green: one halo round the cross, still, from faint to full with RESONANCE; with DETUNE two paler, dashed copies
       of it slide apart along the floor, one to the right and one to the left, at full DETUNE touching the screen's
       edges */
    {
        const int32_t lvl = 40 + ((k[1] * 216) >> 15);
        if (apart) {
            mix(c, C_BLACK, C_GREEN, lvl >> 1);
            halo_outline(r, x, apart, c);
            halo_outline(r, x, -apart, c);
        }
        mix(c, C_BLACK, C_GREEN, lvl);
        halo_outline(r, x, 0, c);
    }

    /* blue: the solid cross, on top, holding the light, ENV's line and DECAY's lines (white) */
    {
        uint8_t fill[4], top[4], dash[4];
        mix(fill, C_BLACK, C_BLUE, 210);                       /* scan lines: brighter than a solid fill was */
        mix(top, C_BLUE, C_WHITE, 150);
        mix(dash, C_DIM, C_WHITE, 200);
        uint8_t fill_d[4];
        mix(fill_d, C_BLACK, C_BLUE, 150);
        cross_solid(r, x, light_y, env_y, p2[2], fill, fill_d, top, dash, side, glow, edge);
    }

    page_dots(r);

    /* red, in front of everything: DRIVE's lightning, a spike up to 16 px off the wave (longer up to 100 %), and
       its static, up to 7 specks anywhere on the screen */
    for (int sp = 0; sp < (wmess >> 12); sp++) {
        const uint32_t h = tc_hash(tc_frame, 10 + sp);
        const int j = (int)(h % TC_WAVE_N);
        int32_t to = wy[j] + ((h >> 8) & 1 ? 1 : -1) * (QP(6) + (int32_t)(((h >> 9) & 255) * (20 + ((post * 20) >> 14))));
        if (to > QP(TC_WAVE_LOW)) to = QP(TC_WAVE_LOW);
        if (to < QP(TC_WAVE_TOP)) to = QP(TC_WAVE_TOP);
        if (wx[j] >= QP(8) && wx[j] <= QP(310)) ln(r, wx[j], wy[j], wx[j] + QP(2), to, QP(1.2), C_WHITE);
    }
    for (int sp = 0; sp < (wmess >> 10); sp++) {
        const uint32_t h = tc_hash(tc_frame, 20 + sp);
        const int32_t sx = QP(8) + (int32_t)(h % 300) * 1024, sy = QP(8) + (int32_t)((h >> 9) % 142) * 1024;
        ln(r, sx, sy, sx + QP(3), sy, QP(1), dc);
    }
}
#pragma GCC pop_options

const op1_screen_t SCREEN_cross = { tc_screen_init, tc_screen_update };
