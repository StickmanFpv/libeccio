/* SPDX-License-Identifier: GPL-3.0-only
 * Portions Copyright (C) 2026 Kerem Kilic (Ellic Studio), Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * GR!MM's VOICE: Copyright (C) 2026 GR!MM */
/* VOICE: sung vowels, GR!MM's VOICE source (GRiMMBIENT/grimm firmware/src/formant.c, GPL-3.0-only), which is Felucca's
 * formant oscillator (eng_formant.c here: Melodee's retired VOICE). After D. H. Klatt (1980): a glottal source (the
 * derivative of a Rosenberg pulse) into four two-pole resonators in cascade, F1 -> F2 -> F3 -> F4; formant data from
 * Klatt (1980) and Hillenbrand et al. (1995). The DSP is eng_formant.c's (Felucca's), its helpers copied here (vx_*):
 * eng_formant.c itself is built only with MELODEE_LEGACY_EXTRAS.
 *
 * As GR!MM plays it, its four on the main knobs:
 *   VOWEL   A -> E -> I -> O -> U, gliding between them
 *   SHIFT   all formants -12 .. +12 semitones (a smaller or a larger throat)
 *   BUZZ    the source: open quotient 0.9 .. 0.35 and its tilt (soft .. bright, buzzy)
 *   BREATH  noise from the glottis; at the top the voicing fades out (a whisper)
 * and Felucca's movement on EDIT 2: VOWL2 and TALK (the vowel glides to VOWL2 over TALK after each note), Q (the
 * bandwidths), RAND (each note a vowel of its own). GR!MM's factory voices come with their slow movement: the
 * track's LFO and a route of the mod matrix (voice_preset_loaded).
 * Port notes: Melodee's voice holds six state words; F1..F3 take them, F4 the voice's ph[1..2] (as Felucca), and the
 * source's tilt and the TALK word live beside the voices (vc_st: a voice slot each). */
#define VOX_POLY 4u
#define VX_FOLOW 30                                     /* formant key tracking: 30 / 127 */
#define VX_TALK_MASK 0x01FFFFFF

/* formants (Hz) of A E I O U: adult male means, Hillenbrand et al., JASA 97 (1995); bandwidths after Klatt (1980) */
static const uint16_t VX_VOWEL_F[5][3] = {
    {768, 1333, 2522}, {580, 1799, 2605}, {342, 2322, 3000}, {497, 910, 2459}, {378, 997, 2343},
};
static const uint8_t VX_VOWEL_B[5][3] = {
    {90, 100, 160}, {70, 100, 170}, {50, 120, 200}, {70, 80, 150}, {60, 80, 150},
};
/* 2^(k/12), Q16, k = 0..12 */
static const uint32_t VX_SEMI_Q16[13] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072,
};

/* 2^(st16 / 192) in Q16 (st16 in 1/16 semitone), clamped to 1/16 .. 16.
 * Divisions by constants are written as multiplies: -Os keeps them as divides */
static uint32_t vx_ratio(int32_t st16)
{
    int32_t o, k;
    uint32_t r;
    st16 = clamp(st16, -4 * 192, 4 * 192 - 1) + 4 * 192;   /* 0 .. 8 octaves */
    o = (st16 * 2731) >> 19;                            /* / 192, exact for 0 .. 1535 */
    k = st16 - o * 192;
    r = VX_SEMI_Q16[k >> 4] + (((VX_SEMI_Q16[(k >> 4) + 1] - VX_SEMI_Q16[k >> 4]) * (uint32_t)(k & 15)) >> 4);
    return o >= 4 ? r << (o - 4) : r >> (4 - o);
}

/* the three formants of vowel position pos (0..127 << 8, A E I O U), Hz * 16 */
static void vx_vowel_at(int32_t pos, int32_t *f, int32_t *b)
{
    int32_t x = (clamp(pos, 0, 127 << 8) * 33027) >> 20, i = x >> 8, fr = x & 255, j;   /* * 4 / 127 */
    if (i >= 4) {
        i = 3;
        fr = 256;
    }
    for (j = 0; j < 3; j++) {
        f[j] = (VX_VOWEL_F[i][j] << 4) + (((VX_VOWEL_F[i + 1][j] - VX_VOWEL_F[i][j]) * fr) >> 4);
        b[j] = (VX_VOWEL_B[i][j] << 4) + (((VX_VOWEL_B[i + 1][j] - VX_VOWEL_B[i][j]) * fr) >> 4);
    }
}

typedef struct { int32_t a, b, c; } vx_res_t;
static int32_t vx_nz = 0x2545F491;                 /* breath noise, one xorshift for all voices */

/* Klatt resonator coefficients (Q30) for F, BW in Hz * 16 */
static void vx_res_coef(vx_res_t *r, uint32_t f16, uint32_t bw16)
{
    int32_t s = sine_i(f16 * 3044u);                    /* sin(pi F / FS), Q15 (2^32 / (2 FS) / 16) */
    int32_t x = (int32_t)(bw16 * 4781u);                /* pi BW / FS, Q30 (<= 0.2) */
    int32_t x2 = (int32_t)(((int64_t)x * x) >> 30), x3 = (int32_t)(((int64_t)x2 * x) >> 30);
    int32_t x4 = (int32_t)(((int64_t)x3 * x) >> 30);
    int32_t e = (1 << 30) - x + (x2 >> 1) - (int32_t)(((int64_t)x3 * 178956971) >> 30)   /* exp(-x): x^3 / 6, */
                + (int32_t)(((int64_t)x4 * 44739243) >> 30);                            /* x^4 / 24 */
    int32_t cs = (1 << 30) - 2 * s * s;                 /* cos(2 pi F / FS) = 1 - 2 sin^2, Q30 */
    r->b = (int32_t)(((int64_t)e * cs) >> 29);          /* 2 r cos */
    r->c = -(int32_t)(((int64_t)e * e) >> 30);          /* -r^2 */
    r->a = (1 << 30) - r->b - r->c;
}


#define VOX_MD_AMP 4                                    /* mod.c MD_AMP, MD_E1 (included after the engines) */
#define VOX_MD_E1 12

static int32_t vc_st[NPART * NVOICE + 1u][2];          /* a voice slot's tilt and TALK word (the last: tracks 5..8) */
static int32_t *vox_st(const track_t *t, const voice_t *v)
{
    uint32_t part = (uint32_t)(t - trk), lane = (uint32_t)(v - t->v);
    return vc_st[part < NPART && lane < NVOICE ? part * NVOICE + lane : NPART * NVOICE];
}

static void vox_note_on(track_t *t, voice_t *v)
{
    int32_t *st = vox_st(t, v);
    st[1] = (int32_t)((noise32(&vx_nz) >> 25) << 25);   /* TALK starts over; a random vowel (RAND) */
    if (!v->env && !v->env_out) {                       /* a fresh voice (not a retrigger): from rest */
        uint32_t i;
        v->ph[0] = v->ph[1] = v->ph[2] = 0;             /* the opening starts at zero: no click */
        for (i = 0; i < 6u; i++)
            v->s[i] = 0;
        st[0] = 0;
    }
}

static void vox_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t *st = vox_st(t, v);
    vx_res_t r1, r2, r3, r4;
    int32_t fa[3], ba[3], fb[3], bb[3], k, j, buzz, oq, tilt, gv, gn, og, ogk, br = clamp(p[P_E3], 0, 127);
    uint32_t inc = m->inc, i, te, tp, rp, rn, ratio, bws, f0;
    uint32_t ph = v->ph[0];
    int32_t y1 = v->s[0], y2 = v->s[1], y3 = v->s[2], y4 = v->s[3], y5 = v->s[4], y6 = v->s[5];
    int32_t y7 = (int32_t)v->ph[1], y8 = (int32_t)v->ph[2], lp = st[0], nst = vx_nz;

    k = 0;                                              /* VOWEL -> VOWL2 over TALK after the note-on */
    if (p[P_E5]) {
        uint32_t g = ((uint32_t)st[1] & VX_TALK_MASK) + ENV_LIN[p[P_E5] & 127];
        g = g > (1u << 24) ? 1u << 24 : g;
        st[1] = (int32_t)(((uint32_t)st[1] & ~(uint32_t)VX_TALK_MASK) | g);
        k = (int32_t)(g >> 9);                          /* Q15 */
        k = (k * (65536 - k)) >> 15;                    /* ease out: the mouth opens fast, then settles */
    }
    {   /* RAND: both vowels toward this note's random vowel; the host's FLT moves both */
        int32_t rv = (int32_t)(((uint32_t)st[1] >> 25) << 8), v0 = p[P_E0] << 8, v1 = p[P_E4] << 8;
        v0 += ((rv - v0) * p[P_E7] * 258) >> 15;
        v1 += ((rv - v1) * p[P_E7] * 258) >> 15;
        vx_vowel_at(v0 + m->cutoff, fa, ba);
        vx_vowel_at(v1 + m->cutoff, fb, bb);
    }
    ratio = vx_ratio(clamp(p[P_E1], -12, 12) * 16 + ((VX_FOLOW * (v->pitch_cur - 60 * 16) * 516) >> 16));
    bws = vx_ratio((64 - p[P_E6]) * 4);                /* Q: bandwidth x2.5 .. x0.4 */
    f0 = (uint32_t)(((uint64_t)inc * 705600u) >> 32);   /* F0 in Hz * 16 */
    for (j = 0; j < 3; j++) {
        int32_t f = fa[j] + (((fb[j] - fa[j]) * k) >> 15), b = ba[j] + (((bb[j] - ba[j]) * k) >> 15);
        f = (int32_t)(((int64_t)f * ratio) >> 16);
        b = (int32_t)(((((int64_t)b * ratio) >> 16) * bws) >> 16);
        if (j == 0 && (uint32_t)f < f0 + (f0 >> 4) && f > 0) {   /* sung high: F1 follows F0, the same Q (GR!MM's: a
                                                                  * 32-bit division) */
            uint32_t q = ((f0 + (f0 >> 4)) << 8) / (uint32_t)f;
            b = (int32_t)(((int64_t)b * q) >> 8);
            f = (int32_t)(f0 + (f0 >> 4));
        }
        fa[j] = clamp(f, 60 * 16, 7000 * 16);
        ba[j] = clamp(b, 20 * 16, 1500 * 16);
    }
    vx_res_coef(&r1, (uint32_t)fa[0], (uint32_t)ba[0]);
    vx_res_coef(&r2, (uint32_t)fa[1], (uint32_t)ba[1]);
    vx_res_coef(&r3, (uint32_t)fa[2], (uint32_t)ba[2]);
    vx_res_coef(&r4, (uint32_t)clamp((int32_t)((3300u * 16u * (uint64_t)ratio) >> 16), 60 * 16, 7000 * 16),
              (uint32_t)clamp((int32_t)((((250u * 16u * (uint64_t)ratio) >> 16) * bws) >> 16), 20 * 16, 1500 * 16));

    /* the source: BUZZ (+ the host's SHP, + velocity) -> open quotient and tilt */
    buzz = clamp((p[P_E2] << 8) + m->shape - (64 << 8) + (v->vel - 96) * 40, 0, 127 << 8);
    oq = 58982 - ((buzz * 284) >> 8);                   /* Q16: 0.90 .. 0.35 */
    tilt = 2500 + ((buzz * 109) >> 8);                  /* one-pole coefficient Q14: dark .. open */
    te = (uint32_t)oq << 16;
    tp = (te >> 8) * 166u;                              /* opening: 65 % of it, closing 35 % */
    rp = (1u << 30) / ((tp >> 16) | 1u);
    rn = (1u << 30) / (((te - tp) >> 16) | 1u);
    gv = 32767 - br * br * 2;                           /* BREATH: voicing fades at the top */
    gn = br * 258;
    og = 13000 - ((p[P_E6] - 64) * 50);                 /* narrower bands ring louder */
    og = (int32_t)(((int64_t)og * vx_ratio((60 * 16 - v->pitch_cur) / 4)) >> 16);   /* 1.5 dB / oct */
    ogk = (og >> 2) * (4 + (buzz >> 10));               /* presence k = 1 .. 4.9 */

    for (i = 0; i < n; i++) {
        int32_t e, x, a, s;
        if (ph < tp)
            e = (sine_i(((ph >> 16) * rp) << 1) * 17644) >> 15;
        else if (ph < te)
            e = -sine_i(((ph - tp) >> 16) * rn);
        else
            e = 0;
        e += blep(ph - te, inc) >> 1;
        lp += ((e - lp) * tilt) >> 14;
        x = (lp * gv) >> 15;
        if (gn) {
            int32_t nz = (((int32_t)(noise32(&nst) >> 16) - 32768) * gn) >> 15;
            x += ph < te ? nz : nz >> 1;                /* aspiration follows the glottal opening */
        }
        a = (int32_t)(((int64_t)r1.a * (x << 6) + (int64_t)r1.b * y1 + (int64_t)r1.c * y2 + (1 << 29)) >> 30);
        y2 = y1;
        y1 = a;
        a = (int32_t)(((int64_t)r2.a * a + (int64_t)r2.b * y3 + (int64_t)r2.c * y4 + (1 << 29)) >> 30);
        y4 = y3;
        y3 = a;
        a = (int32_t)(((int64_t)r3.a * a + (int64_t)r3.b * y5 + (int64_t)r3.c * y6 + (1 << 29)) >> 30);
        y6 = y5;
        y5 = a;
        a = (int32_t)(((int64_t)r4.a * a + (int64_t)r4.b * y7 + (int64_t)r4.c * y8 + (1 << 29)) >> 30);
        y8 = y7;
        y7 = clamp(a, -(1 << 28), 1 << 28);
        s = soft_knee(clamp((int32_t)(((int64_t)y7 * og + (int64_t)(y7 - y8) * ogk) >> 21), -200000, 200000), 24000);
        ph += inc;
        out[i] += voice_amp(s, m, i) << 1;
    }
    v->ph[0] = ph;
    v->ph[1] = (uint32_t)y7;
    v->ph[2] = (uint32_t)y8;
    vx_nz = nst;
    v->s[0] = y1; v->s[1] = y2; v->s[2] = y3; v->s[3] = y4; v->s[4] = y5; v->s[5] = y6;
    st[0] = lp;
}

static uint32_t vox_units(const track_t *t) { (void)t; return 2u; }   /* four resonators a voice: as a ladder voice */

static const preset_t VOX_PRESETS[] = {
    /* name, {VOWEL, SHIFT, BUZZ, BRTH, VOWL2, TALK, Q, RAND}, {A D S R}, fenv, mono; GR!MM's first six */
    {"CHOIR GHOST", {0, 2, 30, 40, 0, 0, 64, 0}, {85, 90, 115, 95}, 0, 0},
    {"MONKS", {120, -8, 90, 45, 120, 0, 64, 0}, {75, 80, 100, 90}, 0, 0},
    {"WHISPERS", {20, 2, 20, 110, 20, 0, 60, 0}, {80, 90, 115, 95}, 0, 0},
    {"VIGIL", {64, -2, 45, 30, 64, 0, 70, 0}, {90, 95, 100, 100}, 0, 0},
    {"STATIC PRAY", {96, -6, 100, 90, 96, 0, 64, 0}, {90, 90, 115, 100}, 0, 0},
    {"WIND STONE", {96, 0, 0, 127, 96, 0, 100, 0}, {100, 90, 120, 105}, 0, 0},
    {"VOX LEAD", {32, 2, 90, 8, 32, 0, 72, 0}, {6, 70, 105, 55}, 0, 1},
    {"WOW BASS", {95, 0, 100, 0, 0, 68, 80, 0}, {0, 70, 70, 30}, 0, 1},
    {"TALKER", {0, 0, 70, 12, 110, 70, 70, 60}, {2, 70, 100, 40}, 0, 0},
    {"AAH PAD", {0, -2, 40, 22, 0, 0, 60, 0}, {85, 90, 115, 95}, 0, 0},
};
#define VOX_NPRESETS NELEM(VOX_PRESETS)
/* each factory voice's slow movement (GR!MM's LFO): RATE, WAVE (0 SIN .. 4 S&H), vibrato (the LFO's PIT), and one
 * route of the mod matrix (LFO -> destination, amount): 0 none, MD_AMP, MD_E1 + k (EDIT k + 1) */
static const int8_t VOX_MOVE[VOX_NPRESETS][5] = {
    {64, 0, 2, 0, 0},  {40, 0, 0, 0, 0},  {35, 4, 0, VOX_MD_E1 + 0, 10}, {25, 0, 0, VOX_MD_E1 + 0, 40},
    {115, 4, 0, VOX_MD_AMP, 20}, {30, 0, 0, VOX_MD_E1 + 1, 30}, {70, 0, 4, 0, 0}, {60, 0, 0, 0, 0},
    {60, 0, 0, 0, 0},  {64, 0, 2, 0, 0}};

/* a factory voice loaded (ui.c apply_preset_to): its movement, which lives outside the EDIT values */
static void voice_preset_loaded(track_t *t, uint32_t pi)
{
    const int8_t *mv;
    if (t->eng_req != ENGI_VOICE || pi >= VOX_NPRESETS)
        return;
    mv = VOX_MOVE[pi];
    t->p[P_LRATE] = mv[0];
    t->p[P_LWAVE] = mv[1];
    t->p[P_LD_PIT] = mv[2];
    if (mv[3]) {
        t->p[P_M1SRC] = 1;                              /* LFO */
        t->p[P_M1DST] = mv[3];
        t->p[P_M1AMT] = mv[4];
    }
}

_Static_assert(VOX_MD_AMP == 4 && VOX_MD_E1 == 12, "mod.c's destinations");
static const engine_t ENG_VOICE = {
    .name = "VOICE",
    .page_title = {"VOICE", "MOVE"},
    .edit = {
        {"VOWEL", F_INT, 0, 127, 0, 0, 0},
        {"SHIFT", F_SEMI, -12, 12, 0, 0, 0},
        {"BUZZ", F_PCT, 0, 127, 64, 0, 0},
        {"BRTH", F_PCT, 0, 127, 10, 0, 0},
        {"VOWL2", F_INT, 0, 127, 64, 0, 0},
        {"TALK", F_TIME, 0, 127, 0, 0, 0},
        {"Q", F_PCT, 0, 127, 64, 0, 0},
        {"RAND", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = VOX_PRESETS,
    .npresets = VOX_NPRESETS,
    .note_on = vox_note_on,
    .render = vox_render,
    .knob = {P_E0, P_E1, P_E2, P_E3},   /* GR!MM's four: VOWEL SHIFT BUZZ BREATH */
    .poly = VOX_POLY,
    .units = vox_units,
    .keep = 0x00,
};
