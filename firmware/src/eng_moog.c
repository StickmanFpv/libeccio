/* SPDX-License-Identifier: GPL-3.0-only */
/* MOOG: a three-oscillator Minimoog-style synth. The DSP (oscillators, the mixer's soft overload, the transistor-ladder
 * filter solved without a delay in its loop, the filter contour, the knob glide) is FMini-1's (Guilherme Ruppert,
 * GPL-3.0-only, partly based on Felucca); see eng_moog_filter.h for the ladder's laws.
 * Port notes, for this fork:
 *   - FMini-1 keeps a 32-byte patch per track (ranges, waves, mixer, switches) beside the EDIT values. Here the
 *     oscillator section is one curated choice, OSC (E7: MOOG_OSC below), so every sound lives in the eight EDIT
 *     values and saves with projects and presets like any engine's.
 *   - Melodee's voice holds six state words (FMini-1 used eight): the ladder s[0..3], the feedback's lag pole s[4],
 *     the filter contour s[5] (stage << 24 | level Q23). The VCOs' slow drift, stored per voice in FMini-1, is a fixed
 *     calibration per voice slot plus a slow wobble from the part's block counter: no state.
 *   - The modulation section (LFO / noise to pitch and filter, the MOD depth) is left to the host's LFO and matrix;
 *     its EDIT slot is DRIVE, the mixer's overload (the "Minimoog trick" of the mixer feeding the filter hot).
 * The loudness contour is the host's ADSR; the filter contour is the engine's own (CNTR amount, F.ATK, F.DEC, F.SUS;
 * as on the original, the release takes the DECAY time). Keyboard tracking is fixed at 2/3.
 * POLY plays up to 6 voices; MONO / LEGATO / UNISON as the host gives them. */
#include "eng_moog_filter.h"
#define MOOG_POLY 6u
#define MOOG_KBD 85                 /* keyboard tracking of the cutoff, 0..127 = 0..1 */

/* the oscillator section, one of a few curated setups: ranges (0 LO, 1 32', 2 16', 3 8', 4 4', 5 2'), waves
 * (0 TRI, 1 SHARK, 2 SAW, 3 SQR, 4 PLS W, 5 PLS N), OSC 2 / 3 offsets in cents, mixer levels (0..127, squared),
 * the noise's level */
typedef struct { const char *name; uint8_t r[3], w[3]; int16_t c2, c3; uint8_t l[3], ln; } moog_osc_t;
static const moog_osc_t MOOG_OSC[] = {
    {"CLASSIC", {2, 2, 3}, {2, 2, 2}, 7, -5, {110, 104, 80}, 0},       /* three saws, a touch apart */
    {"FAT", {2, 2, 1}, {2, 3, 2}, 9, 3, {112, 100, 78}, 0},            /* saw, square, saw an octave down */
    {"SUB", {2, 1, 2}, {3, 0, 2}, 0, 4, {112, 104, 0}, 0},             /* square over a triangle sub */
    {"LEAD", {3, 3, 4}, {2, 2, 3}, 8, -4, {108, 100, 64}, 0},
    {"FIFTH", {3, 3, 2}, {2, 2, 2}, 702, 0, {104, 92, 84}, 0},         /* OSC 2 a fifth up */
    {"BRASS", {3, 3, 3}, {2, 2, 4}, 6, -9, {108, 104, 88}, 0},
    {"HOLLOW", {3, 3, 2}, {3, 5, 0}, 5, 0, {104, 88, 72}, 0},
    {"FLUTE", {3, 4, 3}, {0, 0, 0}, 3, 0, {116, 64, 0}, 30},           /* triangles and a breath of noise */
    {"SHARK", {2, 2, 3}, {1, 1, 1}, 10, -6, {112, 108, 72}, 0},
    {"WIDE", {2, 2, 1}, {4, 5, 2}, 11, -8, {108, 104, 76}, 0},
    {"OCTAVES", {1, 2, 3}, {2, 2, 2}, 4, -4, {104, 100, 92}, 0},
    {"SAW", {2, 2, 2}, {2, 2, 2}, 0, 0, {116, 0, 0}, 0},               /* one oscillator */
    {"SQUARE", {2, 2, 2}, {3, 3, 3}, 0, 0, {116, 0, 0}, 0},
    {"THIN", {3, 3, 3}, {5, 5, 5}, 6, 0, {112, 90, 0}, 0},
    {"NOISE", {3, 3, 3}, {0, 0, 0}, 0, 0, {40, 0, 0}, 120},
};
#define MOOG_NOSC NELEM(MOOG_OSC)
static const char *const N_MOOG_OSC[MOOG_NOSC] = {"CLASSIC", "FAT", "SUB", "LEAD", "FIFTH", "BRASS", "HOLLOW", "FLUTE",
                                                  "SHARK", "WIDE", "OCTAVES", "SAW", "SQUARE", "THIN", "NOISE"};

static int32_t moog_sm[NPART][3];                 /* the smoothed CUT, EMPH, CNTR knobs (Q16): the encoder's steps become ramps */
static uint8_t moog_sm_ok;                        /* bit p: moog_sm[p] follows the knobs (clear = snap to them) */
static uint32_t moog_tick[NPART];                 /* blocks rendered by part p: the VCOs' slow wobble */
static int32_t moog_nst[NPART] = {0x2F6E2B1, 0x1A2B3C5, 0x3C4D5E7, 0x0F1E2D3};   /* a part's noise generator */
static int32_t moog_nbuf[NPART][CTL];             /* the part's noise for this block: one source, all its voices */

static inline int32_t moog_saw(uint32_t ph, uint32_t inc) { return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc); }
static inline int32_t moog_pulse(uint32_t ph, uint32_t inc, uint32_t pw)
{
    return (moog_saw(ph, inc) - moog_saw(ph + pw, inc)) >> 1;
}
static inline int32_t moog_shark(uint32_t ph)             /* a triangle that rises for 3/4 of the cycle */
{
    uint32_t x = ph >> 16;
    return x < 49152u ? (int32_t)((x * 43691u) >> 15) - 32768 : 32767 - (int32_t)((x - 49152u) * 4u);
}

/* once a block and part, before its voices: the knobs' glide, the noise, the wobble's clock */
static void moog_block(track_t *t)
{
    uint32_t part = (uint32_t)(t - trk), i, j;
    static const uint8_t KN[3] = {P_E0, P_E1, P_E2};
    if (part >= NPART)
        return;
    moog_tick[part]++;
    for (i = 0; i < NVOICE && !t->v[i].active; i++)
        ;
    /* one encoder click = 1/127: a one-pole per block, about 60 ms (FMini-1 used 200; the fork's sweep knobs take
     * bigger steps a detent and want a quicker follow), exact landing; silent: snap */
    for (j = 0; j < 3; j++) {
        int32_t want = (int32_t)clamp(t->p[KN[j]], 0, 127) << 16, d = want - moog_sm[part][j], st;
        if (i == NVOICE || !(moog_sm_ok & (1u << part))) {
            moog_sm[part][j] = want;
            continue;
        }
        st = d >= 0 ? (int32_t)(((int64_t)d * 790) >> 16) : -(int32_t)(((int64_t)-d * 790) >> 16);
        if (!st)
            st = d > 0 ? 1 : d < 0 ? -1 : 0;
        moog_sm[part][j] += st;
    }
    if (i == NVOICE) {
        moog_sm_ok &= (uint8_t)~(1u << part);
        return;
    }
    moog_sm_ok |= (uint8_t)(1u << part);
    if (MOOG_OSC[clamp(t->p[P_E7], 0, MOOG_NOSC - 1)].ln)
        for (i = 0; i < CTL; i++)
            moog_nbuf[part][i] = (int32_t)(noise32(&moog_nst[part]) >> 16) - 32768;
}

/* one oscillator of the mix: mix[i] += its sample at level g (Q15) */
static void moog_wave(uint32_t w, uint32_t *php, uint32_t inc, int32_t g, int32_t *mix, uint32_t n)
{
    uint32_t ph = *php, i;
    switch (w) {
    case 0: for (i = 0; i < n; i++) { mix[i] += mulq15(osc_tri(ph), g); ph += inc; } break;
    case 1: for (i = 0; i < n; i++) { mix[i] += mulq15(moog_shark(ph), g); ph += inc; } break;
    case 2: for (i = 0; i < n; i++) { mix[i] += mulq15(moog_saw(ph, inc), g); ph += inc; } break;
    case 3: for (i = 0; i < n; i++) { mix[i] += mulq15(moog_pulse(ph, inc, 0x80000000u), g); ph += inc; } break;
    case 4: for (i = 0; i < n; i++) { mix[i] += mulq15(moog_pulse(ph, inc, 0x55555555u), g); ph += inc; } break;
    default: for (i = 0; i < n; i++) { mix[i] += mulq15(moog_pulse(ph, inc, 0x1C71C71Cu), g); ph += inc; } break;
    }
    *php = ph;
}

static int32_t moog_range16(uint32_t r)            /* range in 1/16 semitones (LO: very low) */
{
    static const int16_t R[6] = {-48 * 16, -24 * 16, -12 * 16, 0, 12 * 16, 24 * 16};
    return R[r % 6u];
}

/* a voice slot's fixed calibration, -128..127 (part, lane) */
static inline int32_t moog_calib(const track_t *t, const voice_t *v, uint32_t salt)
{
    uint32_t lane = (uint32_t)(v - t->v), part = (uint32_t)(t - trk);
    uint32_t seed = (0x6D2B79F5u + salt) ^ ((part * NVOICE + lane + 1u) * 0x9E3779B9u);
    return (int32_t)(seed >> 24) - 128;
}

static void moog_note_on(track_t *t, voice_t *v)
{
    if (!v->env && !v->env_out) {                          /* from silence: the ladder and the contour from 0 */
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = v->s[4] = v->s[5] = 0;
        if (t->p[P_E1] >= 96 && (uint32_t)(t - trk) < NPART)   /* it will sing: a nudge to start from */
            v->s[0] = (noise32(&moog_nst[(uint32_t)(t - trk)]) & 0x80000000u) ? 700 * 256 : -700 * 256;
    }
    v->s[5] = (int32_t)(1u << 24) | (v->s[5] & 0xFFFFFF);  /* the contour attacks from where it is */
}

/* the filter contour, once a block: stage 1 attack, 2 decay to the sustain, 3 release (the DECAY time); level Q23 */
static int32_t moog_contour(const int16_t *p, voice_t *v)
{
    uint32_t st = (uint32_t)v->s[5] >> 24;
    int32_t e = v->s[5] & 0xFFFFFF, sus = (int32_t)p[P_E5] << 16, k = (int32_t)ENV_EXP[p[P_E4] & 127];
    if (!v->gate && st && st != 3u)
        st = 3;
    switch (st) {
    case 1:
        {   /* the capacitor charges toward a level above full: a concave rise that ends in a corner, as in the Minimoog */
            int32_t step = (int32_t)(ENV_LIN[p[P_E3] & 127] >> 1), d = (int32_t)(((int64_t)((5 << 21) - e) * step * 824) >> 32);
            e += d < 1 ? 1 : d;
        }
        if (e >= (1 << 23)) {
            e = 1 << 23;
            st = 2;
        }
        break;
    case 2:
        e += mulq16(sus - e, (uint32_t)k);
        break;
    case 3:
        e -= mulq16(e, (uint32_t)k);
        break;
    default:
        break;
    }
    v->s[5] = (int32_t)(st << 24) | (e & 0xFFFFFF);
    return e >> 8;                                          /* Q15 */
}

/* a * k / 65536 rounded to the nearest (four floored stages leave a bias a quiet, low ladder cannot sing against) */
static inline int32_t moog_mulr(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k + 32768u) >> 16));
}

/* The ladder, after the Minimoog's (FMini-1's tools/fmini_filter.py has the laws): four equal one-pole stages, each a
 * differential pair (tanh in front), the last stage's output fed back to the first through a lag pole (the emphasis
 * network), solved without a delay in the loop. It sings at 1.578 times its -3 dB corner when the feedback reaches
 * 2.92. CUT and the pitch work in 1/16 semitones of the singing pitch. */
static void moog_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t part = (uint32_t)(t - trk);
    const moog_osc_t *o = &MOOG_OSC[clamp(p[P_E7], 0, MOOG_NOSC - 1)];
    int32_t mix[CTL];
    int32_t env15, lc, fine, k12, G16, gx16, c0, c1, c2, c3, inv14, kinv12, trim12, drive;
    int32_t l1, l2, l3, ln, dr1, dr2, dr3, sm0, sm1, sm2;
    uint32_t i, inc1, inc2, inc3;
    int32_t s0 = v->s[0], s1 = v->s[1], s2 = v->s[2], s3 = v->s[3], sx = v->s[4];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    if (n > CTL)
        n = CTL;
    if (part < NPART) {
        sm0 = moog_sm[part][0]; sm1 = moog_sm[part][1]; sm2 = moog_sm[part][2];
    } else {
        sm0 = clamp(p[P_E0], 0, 127) << 16; sm1 = clamp(p[P_E1], 0, 127) << 16; sm2 = clamp(p[P_E2], 0, 127) << 16;
    }
    env15 = moog_contour(p, v);

    fine = m->fine;
    {   /* the VCOs wander a few cents: a fixed calibration per voice slot and a slow wobble (about +-2 cents) */
        uint32_t tick = part < NPART ? moog_tick[part] : 0u;
        int32_t ca = moog_calib(t, v, 0), cb = moog_calib(t, v, 77);
        /* periods of about 6 to 14 s (a block is 32 samples): triangles of +-32768 */
        int32_t wa = osc_tri(tick * (uint32_t)(420000 + ca * 1200)), wb = osc_tri(tick * (uint32_t)(300000 + cb * 900) + 0x40000000u);
        dr1 = ca / 80 + wa / 21000;                                  /* cents: about +-1.6 fixed, +-1.6 wandering */
        dr2 = cb / 80 + wb / 21000;
        dr3 = (dr1 - dr2) / 2;
    }
    inc1 = cents_inc(clamp(m->pitch16 + moog_range16(o->r[0]), 0, 2047), dr1, fine);
    inc2 = cents_inc(clamp(m->pitch16 + moog_range16(o->r[1]), 0, 2047), o->c2 + dr2, fine);
    inc3 = cents_inc(clamp(m->pitch16 + moog_range16(o->r[2]), 0, 2047), o->c3 + dr3, fine);

    /* mixer levels: a squared curve; DRIVE pushes the mix into the mixer's soft overload */
    l1 = (int32_t)(o->l[0] * o->l[0] * 2);
    l2 = (int32_t)(o->l[1] * o->l[1] * 2);
    l3 = (int32_t)(o->l[2] * o->l[2] * 2);
    ln = (int32_t)(o->ln * o->ln * 2);
    drive = 4096 + p[P_E6] * 64;                                     /* Q12: x1 .. x3 */
    for (i = 0; i < n; i++)
        mix[i] = 0;
    if (l1) moog_wave(o->w[0], &ph0, inc1, l1, mix, n); else ph0 += inc1 * n;
    if (l2) moog_wave(o->w[1], &ph1, inc2, l2, mix, n); else ph1 += inc2 * n;
    if (l3) moog_wave(o->w[2], &ph2, inc3, l3, mix, n); else ph2 += inc3 * n;
    if (ln && part < NPART)
        for (i = 0; i < n; i++)
            mix[i] += mulq15(moog_nbuf[part][i], ln);

    /* the singing pitch (MIDI x 16): the CUT dial, the host's routings, the contour, the keyboard */
    {
        int32_t i0 = sm0 >> 16, fr = (sm0 >> 8) & 255;
        lc = MOOG_CUT[i0];
        if (i0 < 127)
            lc += ((MOOG_CUT[i0 + 1] - lc) * fr) >> 8;
    }
    lc += (m->cutoff * 55) >> 10;                                                       /* the host's steps (1/14 octave) */
    lc += (int32_t)(((int64_t)env15 * sm2 / (127 * 65536)) * 960) >> 15;                /* the contour: up to 5 octaves */
    lc += ((v->pitch_cur - MOOG_NOTE0 * 16) * MOOG_KBD * 516) >> 16;                    /* the keyboard */
    lc = clamp(lc, MOOG_GM0 * 16, MOOG_FOSC_MAX16);
    {   /* the stage pole G and the lag pole's from the tables (a semitone an entry, linear between), the feedback solved ahead */
        uint32_t ix = (uint32_t)((lc >> 4) - MOOG_GM0), fr = (uint32_t)lc & 15u, gq, gxq, g2, g3, g4, om;
        gq = MOOG_G[ix] + (uint32_t)(((int32_t)(MOOG_G[ix + 1] - MOOG_G[ix]) * (int32_t)fr) >> 4);
        gxq = MOOG_GX[ix] + (uint32_t)(((int32_t)(MOOG_GX[ix + 1] - MOOG_GX[ix]) * (int32_t)fr) >> 4);
        G16 = (int32_t)((gq + 128u) >> 8);
        gx16 = (int32_t)((gxq + 128u) >> 8);
        om = 65536u - (uint32_t)G16;
        g2 = ((uint32_t)G16 * (uint32_t)G16) >> 16;
        g3 = (g2 * (uint32_t)G16) >> 16;
        g4 = (g3 * (uint32_t)G16) >> 16;
        c3 = (int32_t)(om >> 2);
        c2 = (int32_t)(((uint32_t)G16 * om) >> 18);
        c1 = (int32_t)((g2 * om) >> 18);
        c0 = (int32_t)((g3 * om) >> 18);
        {   /* EMPHASIS: the feedback gain k and the passband's loss */
            int32_t i0 = sm1 >> 16, fk = (sm1 >> 8) & 255, den;
            k12 = MOOG_K[i0];
            trim12 = MOOG_TRIM[i0];
            if (i0 < 127) {
                k12 += ((MOOG_K[i0 + 1] - k12) * fk) >> 8;
                trim12 += ((MOOG_TRIM[i0 + 1] - trim12) * fk) >> 8;
            }
            den = 65536 + ((k12 * (int32_t)((((uint32_t)gxq >> 8) * g4) >> 16)) >> 12);
            inv14 = (int32_t)((1u << 30) / (uint32_t)den);
            kinv12 = (k12 * inv14) >> 14;
        }
    }

    for (i = 0; i < n; i++) {
        int32_t x = softclip((mix[i] * drive) >> 12), S, fbc, u, v1, v2, v3, v4, y1, y2, y3, y4, w;
        S = (c0 * ((s0 + 128) >> 8) + c1 * ((s1 + 128) >> 8) + c2 * ((s2 + 128) >> 8) + c3 * ((s3 + 128) >> 8)) >> 14;
        fbc = sx + moog_mulr((S << 8) - sx, (uint32_t)gx16);
        u = ((x * inv14) >> 14) - ((kinv12 * ((fbc + 128) >> 8)) >> 12);
        v1 = moog_mulr((softclip(u) << 8) - s0, (uint32_t)G16);
        y1 = v1 + s0;
        s0 = y1 + v1;
        v2 = moog_mulr((softclip((y1 + 128) >> 8) << 8) - s1, (uint32_t)G16);
        y2 = v2 + s1;
        s1 = y2 + v2;
        v3 = moog_mulr((softclip((y2 + 128) >> 8) << 8) - s2, (uint32_t)G16);
        y3 = v3 + s2;
        s2 = y3 + v3;
        v4 = moog_mulr((softclip((y3 + 128) >> 8) << 8) - s3, (uint32_t)G16);
        y4 = v4 + s3;
        s3 = y4 + v4;
        w = moog_mulr(y4 - sx, (uint32_t)gx16);
        sx += w + w;
        out[i] += voice_amp((((y4 + 128) >> 8) * trim12) >> 12, m, i) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = s0;
    v->s[1] = s1;
    v->s[2] = s2;
    v->s[3] = s3;
    v->s[4] = sx;
}

static uint32_t moog_units(const track_t *t) { (void)t; return 2u; }   /* a ladder voice costs about a Prophet voice */

static const preset_t MOOG_PRESETS[] = {
    /* name, {CUT, EMPH, CNTR, F.ATK, F.DEC, F.SUS, DRIVE, OSC}, {A D S R}, fenv, mono */
    {"MINI BASS", {38, 34, 72, 0, 52, 22, 40, 0}, {0, 64, 100, 22}, 0, 1},
    {"TAURUS SUB", {30, 18, 46, 0, 62, 30, 56, 2}, {0, 70, 110, 30}, 0, 1},
    {"FAT LEAD", {66, 52, 48, 0, 70, 60, 72, 1}, {2, 70, 112, 40}, 0, 1},
    {"SQUELCH", {30, 92, 84, 0, 44, 8, 50, 11}, {0, 60, 96, 20}, 0, 1},
    {"MOOG BRASS", {46, 22, 76, 34, 72, 66, 30, 5}, {22, 80, 100, 48}, 0, 0},
    {"POLY PAD", {54, 26, 40, 84, 92, 82, 18, 9}, {72, 90, 110, 92}, 0, 0},
    {"FLUTE", {70, 12, 22, 22, 60, 92, 8, 7}, {16, 70, 110, 40}, 0, 1},
    {"PLUCK", {44, 40, 92, 0, 40, 0, 30, 0}, {0, 60, 0, 40}, 0, 0},
    {"FIFTH LEAD", {62, 40, 50, 0, 66, 54, 60, 4}, {2, 70, 108, 40}, 0, 1},
    {"OCTAVE BAS", {34, 30, 62, 0, 56, 18, 64, 10}, {0, 66, 104, 24}, 0, 1},
};

static const engine_t ENG_MOOG = {
    .name = "MOOG",
    .page_title = {"FILTER", "CONTOUR/OSC"},
    .edit = {
        {"CUT", F_CUTOFF, 0, 127, 64, 0, 0},
        {"EMPH", F_PCT, 0, 127, 40, 0, 0},
        {"CNTR", F_PCT, 0, 127, 64, 0, 0},
        {"F.ATK", F_TIME, 0, 127, 0, 0, 0},
        {"F.DEC", F_TIME, 0, 127, 64, 0, 0},
        {"F.SUS", F_PCT, 0, 127, 40, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 30, 0, 0},
        {"OSC", F_ENUM, 0, MOOG_NOSC - 1, 0, N_MOOG_OSC, 0},
    },
    .presets = MOOG_PRESETS,
    .npresets = NELEM(MOOG_PRESETS),
    .knob = {P_E0, P_E1, P_E2, P_REL},
    .poly = MOOG_POLY,
    .note_on = moog_note_on,
    .render = moog_render,
    .block = moog_block,
    .units = moog_units,
    .keep = 0x13,                /* s[0] s[1] s[4]: ladder and lag over a retrigger (as FMini-1) */
};
