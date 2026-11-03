/*
 * CROSS fixed-point voice. Signal path per sample:
 *   VCO1 + VCO2 + SUB -> mixer overload -> 4-pole ladder -> post drive -> level
 * Formats: audio Q15 (32768 = 1.0), saturator input Q13, pitch/cutoff in
 * octaves Q16, contour Q23.
 *
 * Rate: the OP-1 renders a voice at 88.2 kHz (128 samples a 1.45 ms block,
 * which the OS then halves to 44.1 kHz); the times and frequencies here and
 * in the tables (tools/gen_tables.mjs) are at that rate.
 *
 * Cost: a block runs in three passes, so each loop keeps its values in
 * registers: the contour, cutoff and output level once per 16 samples; the
 * oscillators and the mixer overload into out[]; the ladder and post drive
 * in place. Every product in the two sample loops is a 16 x 16 multiply
 * (MUL16: one cycle on the Blackfin, where a 32-bit one takes several), the
 * clamps are one saturating shift (SAT16), the pitch is read once per 16
 * samples and the knob-dependent branches (pulse blend, post drive) are
 * taken once per block. A 16-sample step that no oscillator wraps in (most of
 * them for a bass note) runs a loop without step corrections or sub flips:
 * no branch at all. The analogue drift costs a few operations per block.
 */
#include "cross_dsp.h"
#include "cross_tables.h"

#define ENV_TARGET 10905190u /* attack aims at 1.3 so it reaches 1.0 quickly */
#define OCT_MAX ((160 << 12) - 1) /* top of the cutoff table, 41 kHz (open past TCH_G_MAX's 26 kHz) */
#define INC_MIN 4096u
#define INC_MAX 0x7f000000u
#define MAX_STEPS 8 /* 128 samples */

#if defined(__bfin__)
/* a * b with both operands known to fit 16 signed bits: one multiply of the
   low halves (GCC otherwise drops the casts and emits a 32-bit multiply) */
typedef short tch_v2hi __attribute__((vector_size(4)));
#define MUL16(a, b) __builtin_bfin_mulhisill((tch_v2hi)(int32_t)(a), (tch_v2hi)(int32_t)(b))
/* x clamped to [-32768, 32767]: a saturating shift up and back */
#define SAT16(x) (__builtin_bfin_shl_fr1x32((x), 16) >> 16)
#else
#define MUL16(a, b) ((int32_t)(int16_t)(a) * (int32_t)(int16_t)(b))
#define SAT16(x) clamp32((x), -32768, 32767)
#endif

#if defined(__GNUC__)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define RARELY(c) __builtin_expect(!!(c), 0)
#else
#define ALWAYS_INLINE inline
#define RARELY(c) (c)
#endif

/* polyBLEP window for one oscillator, set once per block: phase / inc in
   Q15 is ((phase >> sh) * rdt) >> 15, with inc >> sh held to 15 bits. */
typedef struct {
    uint32_t sh, incs, rdt;
} blep_win;

static inline int32_t clamp32(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline uint32_t clampu(uint32_t x, uint32_t lo, uint32_t hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Soft saturation: Q13 in [-4, 4] -> Q15 tanh. */
static ALWAYS_INLINE int32_t sat(int32_t x)
{
    const int16_t *t;

    x = SAT16(x);
    t = tch_tanh + 128 + (x >> 8);
    return t[0] + (MUL16(t[1] - t[0], x & 255) >> 8);
}

/* Interpolated lookup of a 129-entry table indexed by a 0-32767 knob word. */
#define KNOB_LUT(tab, p) \
    ((tab)[(p) >> 8] + ((((int32_t)(tab)[((p) >> 8) + 1] - (int32_t)(tab)[(p) >> 8]) * ((p) & 255)) >> 8))

static inline int clz32(uint32_t x)
{
#if defined(__GNUC__)
    return __builtin_clz(x);
#else
    int n = 0;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}

/* log2(x) in Q16 for x > 0. Called once per block. */
static int32_t log2_q16(uint32_t x)
{
    int n = 31 - clz32(x);
    uint32_t m = x << (31 - n);
    uint32_t i = (m >> 25) & 63;
    int32_t fr = (int32_t)((m >> 17) & 255);

    return (n << 16) + tch_log2[i] + (((tch_log2[i + 1] - tch_log2[i]) * fr) >> 8);
}

/* Ladder stage coefficient for a cutoff in octaves above 40 Hz (Q16). */
static inline int32_t cutoff_coef(int32_t oct)
{
    int32_t idx, fr, a;

    oct = clamp32(oct, 0, OCT_MAX);
    idx = oct >> 12;
    fr = (oct >> 4) & 255;
    a = tch_gcoef[idx];
    return a + (((tch_gcoef[idx + 1] - a) * fr) >> 8);
}

/* inc * (1 + up / 65536) in 32 bits, for up <= 65650 (VCO2's interval). */
static inline uint32_t raise(uint32_t inc, uint32_t up)
{
    return inc + (inc >> 16) * up + (((inc & 0xFFFFu) * (up >> 1)) >> 15);
}

static void blep_setup(blep_win *bw, uint32_t inc)
{
    int sh = 31 - clz32(inc) - 14;

    bw->sh = sh > 0 ? (uint32_t)sh : 0;
    bw->incs = inc >> bw->sh;
    bw->rdt = (1u << 30) / bw->incs;
}

/* phase / inc in Q15, saturating at 1.0 outside the block's window. */
static inline int32_t blep_x(uint32_t ph, const blep_win *bw)
{
    uint32_t q = ph >> bw->sh;

    return q >= bw->incs ? 32768 : (int32_t)((q * bw->rdt) >> 15);
}

/* The phase lies within one increment of a wrap, on either side: the only
   samples a step correction touches. One compare instead of two. */
#define NEAR_WRAP(ph, inc, inc2) ((uint32_t)((ph) + (inc)) < (inc2))

/* polyBLEP correction (Q15) for a falling step of 2.0 at the wrap next to
   ph (NEAR_WRAP holds). */
static ALWAYS_INLINE int32_t blep(uint32_t ph, uint32_t inc, const blep_win *bw)
{
    int32_t x;

    if (ph < inc) {
        x = blep_x(ph, bw);
        return 2 * x - ((x * x) >> 15) - 32768;
    }
    x = blep_x(0u - ph, bw);
    return ((x * x) >> 15) - 2 * x + 32768;
}

/* A naive saw, Q15: (ph >> 16) - 32768. */
#define RAW_SAW(ph) ((int32_t)((ph) ^ 0x80000000u) >> 16)

/* A band-limited saw, Q15, within [-32768, 32767]. */
static ALWAYS_INLINE int32_t saw(uint32_t ph, uint32_t inc, uint32_t inc2, const blep_win *bw)
{
    int32_t s = RAW_SAW(ph);

    if (RARELY(NEAR_WRAP(ph, inc, inc2)))
        s -= blep(ph, inc, bw);
    return s;
}

/* The largest phase - inc that keeps 16 samples from ph clear of any wrap
   window, and with no wrap after them (0: never, for notes above 5.2 kHz). */
static inline uint32_t clear_lim(uint32_t inc)
{
    return inc < 0x0F0F0F0Fu ? 0u - 17 * inc : 0;
}

#define CLEAR(ph, inc, lim) ((uint32_t)((ph) - (inc)) < (lim))

/* ---- analogue drift: each VCO wanders on its own, about 0.7 cent RMS
   (2 cents at most, now and then) over several seconds, plus a fixed
   offset of up to 0.4 cent drawn at each note. Ratios in Q24: 1 cent is
   about 9690. ---- */
#define DRIFT_POLE 8    /* the wander's two smoothing poles: about 0.4 Hz */
#define NUDGE_SHIFT 19  /* a note's offset: +-4096, 0.4 cent */

static inline int32_t noise(uint32_t *rng)
{
    *rng = *rng * 1664525u + 1013904223u;
    return (int32_t)*rng;
}

/* inc * (1 + d / 2^24), for |d| < 32768 */
static inline uint32_t drift(uint32_t inc, int32_t d)
{
    return inc + (uint32_t)(((int32_t)(inc >> 15) * d) >> 9);
}

void tch_note_on(tch_voice *v)
{
    int i;

    /* The state may hold stale memory: sanitise, don't zero, so a mono
       retrigger stays click-free. */
    v->flags &= TCH_F_SUB | TCH_F_STILL;
    if (v->env > TCH_ENV_ONE)
        v->env = 0;
    for (i = 0; i < 4; i++)
        v->lp[i] = clamp32(v->lp[i], -32767, 32767);
    for (i = 0; i < 2; i++)
        v->nudge[i] = noise(&v->rng) >> NUDGE_SHIFT;
}

/* Everything the oscillator pass needs, set once per block. */
typedef struct {
    blep_win bw1, bw2;
    uint32_t up, pw;
    int32_t m, m9, sub, pre;
    int32_t d1, d2; /* each VCO's drift, Q24 ratio */
} osc_set;

/* Pass 1: VCO1 + VCO2 + SUB, mixer overload, into out[] (Q15). pulse is a
   constant at each call site, so the compiler builds loops without the
   second pair of saws for SAW .. SQR+SUB's first third. */
static ALWAYS_INLINE void osc_pass(tch_voice *v, int16_t *out, int steps, const int32_t *inc,
                                   const osc_set *o, const int pulse)
{
    uint32_t ph1 = v->ph1, ph2 = v->ph2;
    /* the sub square, +32767 or -32768, and its share of the mix: it only
       changes at VCO1's wraps */
    int32_t sq = (v->flags & TCH_F_SUB) ? -32768 : 32767;
    const int16_t sub = (int16_t)o->sub, pre = (int16_t)o->pre, m = (int16_t)o->m, m9 = (int16_t)o->m9;
    const uint32_t pw = o->pw;
    int32_t sv = MUL16(sq, sub) >> 15;
    int st, i;

    for (st = 0; st < steps; st++) {
        /* the pitch once per 16 samples (0.18 ms): glides stay smooth */
        const uint32_t i0 = clampu((uint32_t)inc[st * TCH_BLOCK_STEP], INC_MIN, INC_MAX);
        const uint32_t i1 = drift(i0, o->d1), i2 = drift(raise(i0, o->up), o->d2);
        const uint32_t lim1 = clear_lim(i1), lim2 = clear_lim(i2);
        int16_t *y = out + st * TCH_BLOCK_STEP;

        if (CLEAR(ph1, i1, lim1) && CLEAR(ph2, i2, lim2) &&
            (!pulse || (CLEAR(ph1 + pw, i1, lim1) && CLEAR(ph2 + pw, i2, lim2)))) {
            /* no wrap near: the naive saws are exact, the sub holds */
            for (i = 0; i < TCH_BLOCK_STEP; i++) {
                int32_t mix = RAW_SAW(ph1) + (MUL16(RAW_SAW(ph2), 29491) >> 15) + sv;
                if (pulse) {
                    mix -= (MUL16(m, RAW_SAW(ph1 + pw)) + MUL16(m9, RAW_SAW(ph2 + pw))) >> 15;
                    y[i] = (int16_t)sat(MUL16(mix >> 3, pre) >> 13);
                } else {
                    y[i] = (int16_t)sat(MUL16(mix >> 2, pre) >> 14);
                }
                ph1 += i1;
                ph2 += i2;
            }
            continue;
        }

        {
            const uint32_t w1 = 2 * i1;
            const uint32_t w2 = i2 < 0x80000000u ? 2 * i2 : 0xFFFFFFFFu; /* VCO2 past Nyquist */

            for (i = 0; i < TCH_BLOCK_STEP; i++) {
                int32_t s1 = RAW_SAW(ph1), mix, sb = sv;
                uint32_t next;

                /* VCO1, and the sub's step correction reusing VCO1's: the
                   square falls or rises by 2.0 at the wrap as the saw falls */
                if (NEAR_WRAP(ph1, i1, w1)) {
                    const int32_t b1 = blep(ph1, i1, &o->bw1);
                    s1 -= b1;
                    sb = MUL16(sq + ((ph1 < i1) == (sq < 0) ? -b1 : b1), sub) >> 15;
                }

                /* VCO2 at 0.9; a pulse is a saw minus m of a shifted saw */
                mix = s1 + (MUL16(saw(ph2, i2, w2, &o->bw2), 29491) >> 15) + sb;
                if (pulse) {
                    mix -= (MUL16(m, saw(ph1 + pw, i1, w1, &o->bw1)) +
                            MUL16(m9, saw(ph2 + pw, i2, w2, &o->bw2))) >> 15;
                    /* up to +-154000: one bit less before the 16-bit product */
                    y[i] = (int16_t)sat(MUL16(mix >> 3, pre) >> 13);
                } else {
                    y[i] = (int16_t)sat(MUL16(mix >> 2, pre) >> 14);
                }

                next = ph1 + i1;
                if (next < ph1) {
                    sq = ~sq;
                    sv = MUL16(sq, sub) >> 15;
                }
                ph1 = next;
                ph2 += i2;
            }
        }
    }
    v->ph1 = ph1;
    v->ph2 = ph2;
    v->flags = (v->flags & ~TCH_F_SUB) | (sq < 0 ? TCH_F_SUB : 0);
}

/* Pass 2: the ladder, post drive and level, in place. g and og hold one
   value per 16 samples; drive is a constant at each call site. */
static ALWAYS_INLINE void filter_pass(tch_voice *v, int16_t *out, int steps, const int16_t *gs,
                                      const int16_t *ogs, int16_t k, int16_t comp, int16_t post,
                                      const int drive)
{
    int32_t l0 = v->lp[0], l1 = v->lp[1], l2 = v->lp[2], l3 = v->lp[3];
    int st, i;

    for (st = 0; st < steps; st++) {
        const int16_t g = gs[st], og = ogs[st];
        int16_t *y = out + st * TCH_BLOCK_STEP;

        for (i = 0; i < TCH_BLOCK_STEP; i++) {
            int32_t u, z;

            /* saturated feedback input, then four one-pole stages; g (a - b)
               as g a - g b keeps both products 16 x 16 */
            u = (MUL16(y[i], comp) >> 12) - (MUL16(k, l3) >> 12);
            u = sat(u >> 2);
            l0 += (MUL16(g, u) - MUL16(g, l0) + 16384) >> 15;
            l1 += (MUL16(g, l0) - MUL16(g, l1) + 16384) >> 15;
            l2 += (MUL16(g, l1) - MUL16(g, l2) + 16384) >> 15;
            l3 += (MUL16(g, l2) - MUL16(g, l3) + 16384) >> 15;

            /* post drive and level: |z| < 32768 and og <= 24576, so the
               product needs no clamp */
            z = drive ? sat(MUL16(l3, post) >> 10) : l3;
            y[i] = (int16_t)(MUL16(z, og) >> 15);
        }
    }
    v->lp[0] = l0;
    v->lp[1] = l1;
    v->lp[2] = l2;
    v->lp[3] = l3;
}

void tch_render(tch_voice *v, int16_t *out, int n, const int32_t *inc,
                const int16_t *w, int32_t gain)
{
    uint32_t inc0, env = v->env, dc;
    osc_set o;
    int16_t gs[MAX_STEPS], ogs[MAX_STEPS];
    int32_t p, base, amt, key, pq, k, comp, post, outg, punch, d;
    int st, steps = n / TCH_BLOCK_STEP;

    if (steps > MAX_STEPS)
        steps = MAX_STEPS;

    /* ---- once per block: everything that only depends on knobs and pitch ---- */
    inc0 = clampu((uint32_t)inc[0], INC_MIN, INC_MAX);
    o.up = tch_detune[((uint32_t)w[TCH_P_DETUNE] >> 8) & 127] - 65536u;
    blep_setup(&o.bw1, inc0);
    blep_setup(&o.bw2, raise(inc0, o.up));

    /* the drift: noise through two slow poles per VCO, then the note's offset */
    o.d1 = o.d2 = 0;
    if (!(v->flags & TCH_F_STILL)) {
        int32_t *a = v->wand;
        a[0] += ((noise(&v->rng) >> 14) * 3 - a[0]) >> DRIFT_POLE;
        a[1] += (a[0] - a[1]) >> DRIFT_POLE;
        a[2] += ((noise(&v->rng) >> 14) * 3 - a[2]) >> DRIFT_POLE;
        a[3] += (a[2] - a[3]) >> DRIFT_POLE;
        o.d1 = clamp32(a[1] + v->nudge[0], -32767, 32767);
        o.d2 = clamp32(a[3] + v->nudge[1], -32767, 32767);
    }

    /* WAVE morph: SAW -> SAW+SUB -> SQR+SUB -> narrow PULSE. A pulse is a saw
       minus a shifted saw; m blends that second saw in, pw sets the shift. */
    p = clamp32(w[TCH_P_WAVE], 0, 32767);
    o.pw = 0x80000000u;
    if (p < 10923) {
        o.m = 0;
        o.sub = p * 3;
    } else if (p < 21845) {
        o.m = (p - 10923) * 3;
        o.sub = 32767;
    } else {
        int32_t t = (p - 21845) * 3;
        o.m = 32767;
        o.pw = 0x80000000u - (uint32_t)t * 49807u; /* 0.50 -> 0.12 of a cycle */
        o.sub = 32767 - (t >> 1);
    }
    o.m = clamp32(o.m, 0, 32767);
    o.m9 = (o.m * 29491) >> 15;                        /* VCO2's share of it */
    o.sub = (clamp32(o.sub, 0, 32767) * 29491) >> 15; /* sub peaks at 0.9 */

    base = clamp32(w[TCH_P_CUTOFF], 0, 32767) * 19;  /* 0 - 9.5 octaves */
    gain = clamp32(gain, 0, 32767);
    amt = clamp32(w[TCH_P_ENV], 0, 32767) * 12;  /* 0 - 6 octaves */
    /* velocity opens the filter too: full velocity is ENV as set, the
       softest note 30 % of it, so accents bite and ghost notes stay dark */
    amt = (((amt >> 4) * (9830 + ((gain * 22937) >> 15))) >> 15) << 4;
    d = clamp32(log2_q16(inc0) - TCH_LOG2_REF, -(8 << 16), 8 << 16);
    key = ((d >> 4) * 43254) >> 12;                  /* 2/3 key tracking */

    p = clamp32(w[TCH_P_RESONANCE], 0, 32767);
    pq = (int32_t)(((uint32_t)p * (uint32_t)(p + 32768)) >> 16); /* gentle curve */
    k = (pq * 16000) >> 15;                          /* feedback, Q12, max 3.9 */
    comp = 4096 + ((3 * k) >> 2);                    /* restore lost bass */

    p = clamp32(w[TCH_P_DRIVE], 0, 32767);
    o.pre = KNOB_LUT(tch_drive_pre, p);
    post = KNOB_LUT(tch_drive_post, p);
    outg = KNOB_LUT(tch_drive_out, p);
    punch = (outg * KNOB_LUT(tch_drive_punch, p)) >> 15;

    p = clamp32(w[TCH_P_DECAY], 0, 32767);
    dc = (uint32_t)KNOB_LUT(tch_decay, p);

    /* ---- every 16 samples: contour, then cutoff and output level ---- */
    for (st = 0; st < steps; st++) {
        int32_t e15;

        if (!(v->flags & TCH_F_DECAY)) {
            env += ((ENV_TARGET - env) >> 8) * TCH_ATTACK_COEF >> 8;
            if (env >= TCH_ENV_ONE) {
                env = TCH_ENV_ONE;
                v->flags |= TCH_F_DECAY;
            }
        } else {
            env -= ((env >> 8) * dc) >> 8;
        }
        e15 = (int32_t)(env >> 8);
        gs[st] = (int16_t)cutoff_coef(base + key + (((e15 >> 3) * (amt >> 4)) >> 8));
        /* past mid DRIVE the attack punches through; velocity folded in */
        ogs[st] = (int16_t)(((outg + ((punch * e15) >> 15)) * gain) >> 15);
    }
    v->env = env;

    /* ---- the two sample passes, each specialised on its knob branch ---- */
    if (o.m)
        osc_pass(v, out, steps, inc, &o, 1);
    else
        osc_pass(v, out, steps, inc, &o, 0);
    if (post)
        filter_pass(v, out, steps, gs, ogs, (int16_t)k, (int16_t)comp, (int16_t)post, 1);
    else
        filter_pass(v, out, steps, gs, ogs, (int16_t)k, (int16_t)comp, 0, 0);
}
