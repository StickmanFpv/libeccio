/* SPDX-License-Identifier: GPL-3.0-only */
/* HADES: a dark bass / sub synth, ported from Caravel (bonszaimusic, GPL-3.0-only, a Felucca fork).
 * Original DSP, not a circuit emulation: saw or pulse with two divider sub-octaves (one phase at f/4,
 * so the subs never detune), a three-pole low-pass with solved feedback, a 120 Hz BODY shelf and a
 * post-filter DRIVE.
 * Port notes: Melodee's voice holds six state words (Caravel's eight). The filter (three poles) and
 * the shelf keep their four words; the remaining two hold the smoothing of the four controls that
 * zipper audibly (CUT, RES, DRV, WAVE). SUB1, SUB2, PW and BODY follow the knob once a block.
 * Voice state: s[0] s[1] s[4] filter poles, s[5] bass shelf, s[2] CUT | RES, s[3] DRV | WAVE (16 bits each). */
static const char *const N_HADES_WAVE[] = {"OFF", "SAW", "PULSE"};

typedef struct { int32_t g, g2, g3, k, inv; } hd_lp3_t;

static inline int32_t hd_q12(int32_t x, int32_t a) { return (x * a) >> 12; }
static inline int32_t hd_pole(int32_t x, int32_t g, int32_t *z)
{
    int32_t d = hd_q12(x - *z, g), y = *z + d;
    *z = clamp(y + d, -49152, 49152);
    return y;
}
static void hd_lp3_coef(hd_lp3_t *f, int32_t cut, int32_t res)
{
    cut = clamp(cut, 0, 127 * 256);
    int32_t i = cut >> 8, g = SVF_G[i];
    if (i < 127) g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    f->g = (g * 4096) / (4096 + g);
    f->g2 = hd_q12(f->g, f->g);
    f->g3 = hd_q12(f->g2, f->g);
    f->k = clamp(res, 0, 32767) * 28672 / 32767; /* 7.0: below the 3-pole oscillation threshold 8 */
    f->inv = (4096 * 4096) / (4096 + hd_q12(f->g3, f->k));
}
static inline int32_t hd_lp3(const hd_lp3_t *f, int32_t in, int32_t z[3])
{
    int32_t s = hd_q12(hd_q12(z[0], f->g2) + hd_q12(z[1], f->g) + z[2], 4096 - f->g);
    in = hd_q12(in, 4096 + f->k);            /* bass stays useful as resonance rises */
    int32_t u = mulq16(in - hd_q12(s, f->k), (uint32_t)f->inv << 4);
    u = soft_knee(u, 20000);
    return hd_pole(hd_pole(hd_pole(u, f->g, z), f->g, z + 1), f->g, z + 2);
}
static inline int32_t hd_saw(uint32_t ph, uint32_t inc)
{
    return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc);
}
static inline int32_t hd_pulse(uint32_t ph, uint32_t inc, uint32_t pw)
{
    return (hd_saw(ph, inc) - hd_saw(ph + pw, inc)) >> 1;
}
static inline int32_t hd_divider(uint32_t phase, uint32_t inc, uint32_t octave)
{
    return hd_pulse(phase << octave, inc << octave, 0x80000000u) * 2;
}
static inline int32_t hd_slew(int32_t was, int32_t target) { return was + clamp(target - was, -768, 768); }
static inline int32_t hd_pair(int32_t a, int32_t b) { return (int32_t)((uint32_t)a | (uint32_t)b << 16); }
static inline int32_t hd_lo(int32_t pair) { return (int32_t)((uint32_t)pair & 65535u); }
static inline int32_t hd_hi(int32_t pair) { return (int32_t)((uint32_t)pair >> 16); }
static inline int32_t hd_ramp(int32_t a, int32_t b, uint32_t i, uint32_t n)
{
    return a + (((b - a) * (int32_t)(i + 1u) * (int32_t)n) >> 16);
}
static inline int32_t hd_drive(int32_t x, int32_t amount)
{
    int32_t a = amount >> 8, gain = 4096 + a * a * 2;
    int32_t wet = softclip(hd_q12(clamp(x, -49152, 49152), gain));
    return x + mulq15(wet - x, amount);
}

static void hades_note_on(track_t *t, voice_t *v)
{
    /* a sounding retrigger keeps the filter, the shelf and the smoothing */
    if (v->env_out) return;
    v->s[0] = v->s[1] = v->s[4] = v->s[5] = 0;
    v->s[2] = hd_pair(t->p[P_E4] * 256, t->p[P_E5] * 258);
    v->s[3] = hd_pair(t->p[P_E6] * 258, t->p[P_E0] * 16384);
}

static void hades_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    if (!n) return;
    const int16_t *p = t->p;
    int32_t z[3] = {v->s[0], v->s[1], v->s[4]}, bass = v->s[5];
    /* smoothed: CUT RES DRV WAVE */
    int32_t a_cut = hd_lo(v->s[2]), a_res = hd_hi(v->s[2]), a_drv = hd_lo(v->s[3]), a_wav = hd_hi(v->s[3]);
    int32_t b_cut = hd_slew(a_cut, clamp(p[P_E4] * 256 + m->cutoff, 0, 127 * 256));
    int32_t b_res = hd_slew(a_res, p[P_E5] * 258);
    int32_t b_drv = hd_slew(a_drv, p[P_E6] * 258);
    int32_t b_wav = hd_slew(a_wav, p[P_E0] * 16384);
    /* once a block: SUB1 SUB2 PW BODY */
    int32_t sub1 = p[P_E2] * 258, sub2 = p[P_E3] * 258, body = p[P_E7] * 258;
    uint32_t pw = (uint32_t)clamp(p[P_E1] * 256 + m->shape - 64 * 256, 8 * 256, 120 * 256) << 17;
    hd_lp3_t f;
    hd_lp3_coef(&f, (a_cut + b_cut) / 2, (a_res + b_res) / 2);
    uint32_t phase = v->ph[0], inc = m->inc >> 2, reciprocal = 65536u / n;
    for (uint32_t i = 0; i < n; i++) {
        int32_t wave = hd_ramp(a_wav, b_wav, i, reciprocal), s = 0;
        uint32_t ph = phase << 2;
        if (wave) {
            int32_t saw = hd_saw(ph, inc << 2);
            if (wave <= 16384) s = (saw * wave) >> 14;
            else {
                int32_t pulse = hd_pulse(ph, inc << 2, pw) * 2;
                s = saw + ((pulse - saw) * (wave - 16384) >> 14);
            }
        }
        if (sub1) s += mulq15(hd_divider(phase, inc, 1), sub1);
        if (sub2) s += mulq15(hd_divider(phase, inc, 0), sub2);
        phase += inc;
        s = hd_lp3(&f, s / 4, z);
        /* 120 Hz one-pole shelf, Q12 coefficient at FS=44100: BODY adds up to 75% of its low band */
        int32_t low = hd_pole(s, 35, &bass);
        s += mulq15(low, body * 3 / 4);
        s = hd_drive(s, hd_ramp(a_drv, b_drv, i, reciprocal));
        out[i] += voice_amp(soft_knee(s, 16000) * 2, m, i) * 2;
    }
    v->ph[0] = phase;
    v->s[0] = z[0]; v->s[1] = z[1]; v->s[4] = z[2]; v->s[5] = bass;
    v->s[2] = hd_pair(b_cut, b_res);
    v->s[3] = hd_pair(b_drv, b_wav);
}

static const preset_t HADES_PRESETS[] = {
    /* name, {WAVE, PW, SUB1, SUB2, CUT, RES, DRV, BODY}, {A D S R}, fenv, mono */
    {"DEEP SUB", {1, 64, 88, 64, 44, 12, 8, 80}, {0, 70, 96, 34}, 18, 1},
    {"DARK SAW", {1, 64, 68, 28, 54, 30, 38, 64}, {0, 68, 64, 28}, 36, 1},
    {"PULSE BAS", {2, 48, 76, 34, 48, 42, 28, 48}, {0, 62, 72, 24}, 32, 1},
    {"SUB ONLY", {0, 64, 100, 68, 72, 8, 0, 64}, {0, 74, 112, 38}, 0, 1},
    {"DRIVEN", {1, 64, 90, 44, 70, 46, 100, 40}, {0, 58, 86, 24}, 22, 1},
    {"RESO BASS", {1, 64, 54, 20, 42, 108, 22, 72}, {0, 74, 24, 32}, 52, 1},
    {"DARK PLK", {2, 32, 52, 18, 62, 58, 44, 24}, {0, 58, 0, 30}, 42, 0},
    {"RAW LEAD", {2, 80, 28, 0, 88, 58, 62, 0}, {2, 66, 104, 36}, 16, 1},
};

static const engine_t ENG_HADES = {
    .name = "HADES",
    .page_title = {"OSC SUB", "FLT DRV"},
    .edit = {
        {"WAVE", F_ENUM, 0, 2, 1, N_HADES_WAVE, 0},
        {"PW", F_PCT, 0, 127, 64, 0, 0},
        {"SUB1", F_PCT, 0, 127, 72, 0, 0},
        {"SUB2", F_PCT, 0, 127, 40, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 64, 0, 0},
        {"RES", F_PCT, 0, 127, 24, 0, 0},
        {"DRV", F_PCT, 0, 127, 20, 0, 0},
        {"BODY", F_PCT, 0, 127, 48, 0, 0},
    },
    .presets = HADES_PRESETS,
    .npresets = NELEM(HADES_PRESETS),
    .knob = {P_E4, P_E5, P_ED_FLT, P_REL},    /* fork: cutoff, resonance, envelope amount, release */
    .note_on = hades_note_on,
    .render = hades_render,
    .keep = 0x33,                /* s[0] s[1] s[4] s[5]: filter and shelf over a retrigger (note_on keeps s[2] s[3]) */
};
