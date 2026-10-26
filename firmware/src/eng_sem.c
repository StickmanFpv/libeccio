/* SPDX-License-Identifier: GPL-3.0-only */
/* SEM-inspired virtual analogue, ported from Caravel (bonszaimusic, GPL-3.0-only, a Felucca fork).
 * Original DSP, not a circuit-identical model: two BLEP oscillators into a two-pole TPT
 * state-variable filter whose STATE knob blends LP -> notch -> HP, as the Oberheim SEM's MODE knob.
 * Port notes: Melodee's voice holds six state words (Caravel's eight), so the STATE ramp and the
 * drift counter share s[3], and the fixed per-slot calibration is recomputed from the slot each
 * block instead of being stored. Caravel's BP / HP TYPE switch is left out: STATE covers LP, notch
 * and HP on one knob. The sound of STATE mode is unchanged.
 * Voice state: s[0] ic1, s[1] ic2 (filter), s[2] RNG, s[3] STATE ramp (low 16 bits) | drift counter
 * (high bits), s[4] smoothed drift, s[5] drift target. */
static const char *const N_SEM_WAVE[] = {"SAW", "PULSE", "TRI", "PWM"};

static inline int32_t sem_saw(uint32_t ph, uint32_t inc)
{
    return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc);
}

static inline int32_t sem_pulse(uint32_t ph, uint32_t inc, uint32_t pw)
{
    return (sem_saw(ph, inc) - sem_saw(ph + pw, inc)) >> 1;
}

static int32_t sem_state(int32_t v)
{
    return v <= 64 ? v * 256 : 16384 + (v - 64) * 16384 / 63;
}

/* the fixed calibration of a physical voice slot (part, lane): -128..127, as Caravel's s[7] */
static inline int32_t sem_calib(const track_t *t, const voice_t *v)
{
    uint32_t lane = (uint32_t)(v - t->v), part = (uint32_t)(t - trk);
    uint32_t seed = 0x6D2B79F5u ^ ((part * NVOICE + lane + 1u) * 0x9E3779B9u);
    return (int32_t)(seed >> 24) - 128;
}

/* fork: CUT and RES glide (about 60 ms a block's one-pole, exact landing; silent: snap), so the knob's detents and the
 * sweep knobs' bigger steps become ramps. Per part, written once a block before its voices (read-only in them). */
static int32_t sem_sm[NPART][2];                   /* Q16 */
static uint8_t sem_sm_ok;
static void sem_block(track_t *t)
{
    uint32_t part = (uint32_t)(t - trk), i, j;
    static const uint8_t KN[2] = {P_E4, P_E5};
    if (part >= NPART)
        return;
    for (i = 0; i < NVOICE && !t->v[i].active; i++)
        ;
    for (j = 0; j < 2; j++) {
        int32_t want = (int32_t)clamp(t->p[KN[j]], 0, 127) << 16, d = want - sem_sm[part][j], st;
        if (i == NVOICE || !(sem_sm_ok & (1u << part))) {
            sem_sm[part][j] = want;
            continue;
        }
        st = d >= 0 ? (int32_t)(((int64_t)d * 790) >> 16) : -(int32_t)(((int64_t)-d * 790) >> 16);
        if (!st)
            st = d > 0 ? 1 : d < 0 ? -1 : 0;
        sem_sm[part][j] += st;
    }
    if (i == NVOICE)
        sem_sm_ok &= (uint8_t)~(1u << part);
    else
        sem_sm_ok |= (uint8_t)(1u << part);
}

static void sem_note_on(track_t *t, voice_t *v)
{
    uint32_t lane = (uint32_t)(v - t->v), part = (uint32_t)(t - trk);
    uint32_t seed = 0x6D2B79F5u ^ ((part * NVOICE + lane + 1u) * 0x9E3779B9u);
    v->ph[1] = v->ph[0] + 0x40000000u;
    v->s[0] = v->s[1] = v->s[4] = 0;
    v->s[2] = (int32_t)(seed ^ (v->age * 0x85EBCA6Bu));
    if (!v->s[2]) v->s[2] = 1;
    v->s[3] = sem_state(t->p[P_E6]);   /* counter 0 */
    v->s[5] = (int32_t)(noise32(&v->s[2]) >> 16) - 32768;
}

static inline __attribute__((always_inline)) int32_t sem_osc(uint32_t wave, uint32_t ph, uint32_t inc, uint32_t pw)
{
    if (wave == 2u) return osc_tri(ph);
    if (wave == 1u || wave == 3u)
        return sem_pulse(ph, inc, wave == 3u ? pw : 0x80000000u);
    return sem_saw(ph, inc);
}

/* Small tuning errors as a relative Q20 increment correction. At full VINT,
 * fixed calibration is about +/-2 cents and slow drift about +/-2 cents.
 * The multiply is split so even MIDI 127 stays inside signed 32-bit range. */
static uint32_t sem_tune(uint32_t inc, int32_t q20)
{
    return inc + (uint32_t)((int32_t)(inc >> 11) * q20 / 512);
}

static void sem_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t vint = p[P_E7], drift, fixed, cut, k, calib = sem_calib(t, v);
    int32_t state0 = v->s[3] & 0xFFFF, count = (int32_t)((uint32_t)v->s[3] >> 16), state1, delta;
    int32_t ic1 = v->s[0], ic2 = v->s[1];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], i;
    uint32_t w0 = (uint32_t)p[P_E0], w1 = (uint32_t)p[P_E1];
    uint32_t pw = (uint32_t)clamp(m->shape, 8 * 256, 120 * 256) << 17;
    int32_t mix = p[P_E3] * 258, gain0 = 32767 - mix;
    tsvf_t f;
    if (++count >= 1024) {
        count = 0;
        v->s[5] = (int32_t)(noise32(&v->s[2]) >> 16) - 32768;
    }
    v->s[4] += (v->s[5] - v->s[4]) / 256;
    drift = (v->s[4] * vint / 127) / 27;
    fixed = calib * vint * 10 / 127;
    uint32_t inc0 = sem_tune(m->inc, fixed + drift);
    uint32_t inc1 = sem_tune(cents_inc(m->pitch16, p[P_E2], m->fine), -fixed + drift / 2);
    {
        uint32_t part = (uint32_t)(t - trk);
        int32_t c16 = part < NPART ? sem_sm[part][0] : p[P_E4] << 16, r16 = part < NPART ? sem_sm[part][1] : p[P_E5] << 16;
        cut = (c16 >> 8) + m->cutoff + (calib * vint / 64);
        k = 8192 - (int32_t)(((int64_t)r16 * 6500 / 127) >> 16);  /* finite resonance: no self oscillation */
    }
    tsvf_coef_k(&f, cut, k);
    /* Bound a live STATE jump to three knob steps per block and ramp each sample. */
    state1 = state0 + clamp(sem_state(p[P_E6]) - state0, -768, 768);
    v->s[3] = (int32_t)(((uint32_t)count << 16) | (uint32_t)state1);
    delta = (state1 - state0) / (int32_t)(n ? n : 1u);
    for (i = 0; i < n; i++) {
        int32_t a = sem_osc(w0, ph0, inc0, pw), b = sem_osc(w1, ph1, inc1, pw);
        int32_t in = (mulq15(a, gain0) + mulq15(b, mix)) >> 1;
        int32_t v3 = in - ic2;
        int32_t bp = (f.a1 * ic1 + f.a2 * v3) >> 13;
        int32_t lp = ic2 + ((f.a2 * ic1 + f.a3 * v3) >> 13);
        int32_t hp = in - ((k * bp) >> 12) - lp, s;
        int32_t state = state0 + delta * (int32_t)(i + 1u);
        ic1 = clamp(2 * bp - ic1, -150000, 150000);
        ic2 = clamp(2 * lp - ic2, -150000, 150000);
        ph0 += inc0; ph1 += inc1;
        /* Q14: exact LP at 0, LP+HP notch at 64, exact HP at 127. */
        s = state <= 16384 ? lp + ((hp * state) >> 14)
                           : hp + ((lp * (32768 - state)) >> 14);
        out[i] += voice_amp(soft_knee(s, 16000) * 2, m, i) * 2;
    }
    v->ph[0] = ph0; v->ph[1] = ph1;
    v->s[0] = ic1; v->s[1] = ic2;
}

static const preset_t SEM_PRESETS[] = {
    /* name, {OSC1, OSC2, DTN, MIX, CUT, RES, STATE, VINT}, {A D S R}, fenv, mono */
    {"SEM BRASS", {0, 0, 8, 64, 48, 24, 0, 28}, {12, 76, 88, 48}, 36, 0},
    {"WARM PAD", {0, 0, 14, 60, 64, 14, 0, 44}, {76, 90, 108, 94}, 12, 0},
    {"OB STRING", {3, 0, 10, 64, 82, 12, 0, 38}, {58, 88, 112, 84}, 8, 0},
    {"ROUND BAS", {1, 0, 3, 38, 38, 34, 0, 18}, {0, 70, 58, 34}, 42, 1},
    {"NOTCH PAD", {0, 3, 12, 64, 70, 32, 64, 32}, {72, 90, 110, 96}, 14, 0},
    {"SEM LEAD", {0, 1, 6, 58, 76, 38, 0, 24}, {2, 72, 100, 44}, 22, 1},
    {"HIGH AIR", {0, 0, 18, 64, 60, 20, 127, 48}, {82, 92, 112, 98}, -10, 0},
    {"SOFT KEY", {2, 0, 5, 34, 72, 16, 0, 16}, {2, 76, 38, 62}, 18, 0},
};

static const engine_t ENG_SEM = {
    .name = "SEM",
    .page_title = {"OSC", "SEM FLT"},
    .edit = {
        {"OSC1", F_ENUM, 0, 3, 0, N_SEM_WAVE, 0},
        {"OSC2", F_ENUM, 0, 3, 0, N_SEM_WAVE, 0},
        {"DTN", F_INT, 0, 50, 8, 0, "ct"},
        {"MIX", F_PCT, 0, 127, 64, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 24, 0, 0},
        {"STATE", F_PCT, 0, 127, 0, 0, 0},
        {"VINT", F_PCT, 0, 127, 24, 0, 0},
    },
    .presets = SEM_PRESETS,
    .npresets = NELEM(SEM_PRESETS),
    .knob = {P_E4, P_E5, P_ED_FLT, P_REL},    /* fork: cutoff, resonance, envelope amount, release */
    .note_on = sem_note_on,
    .render = sem_render,
    .block = sem_block,
    .keep = 0x33,                /* s[0] s[1] s[4] s[5]: filter and drift over a retrigger */
};
