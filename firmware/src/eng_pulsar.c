/* SPDX-License-Identifier: GPL-3.0-only */
/* PULSAR: big, driven drums after the sound of SOMA's Pulsar-23 (original DSP: no circuit model, no samples).
 * Four voices, chosen by the note as the General MIDI drum map plays them, so the KO II and the keys both work:
 *   KICK   35 36           a sine with a fast pitch sweep and a click, a long sub tail
 *   SNARE  37 38 39 40     two pitched tones and a band of noise, SNAP between them
 *   METAL  42 44 (closed), 46 (open), 49 51 52 55 57 59 (cymbal): six square waves at inharmonic ratios and noise,
 *                          high-passed (the classic metal), the open and cymbal ring for METAL's time
 *   BASS   every other note: a pitched percussive bass (triangle and square with a pitch drop, a low-pass that
 *                          closes with it): playable as a bass line from the KO II
 * SUST (the Pulsar's sustaining envelopes): above 0, a held key keeps its voice at that level, so any drum becomes a
 * drone (a kick held is a sub drone, a cymbal held a hiss bed); 0 = plain hits, the key's length does not matter.
 * DRIVE saturates each voice and the part's sum (post): from warm to pulverised.
 * Voice state: s[0] the amplitude (Q24), s[1] the pitch sweep (Q24), s[2] noise, s[3] the voice (PL_*),
 * s[4] s[5] the voice's filter. */
enum { PL_KICK, PL_BASS, PL_SNARE, PL_HATC, PL_HATO, PL_CYM };

static uint32_t pulsar_voice_of(uint32_t note)
{
    switch (note) {
    case 35: case 36: return PL_KICK;
    case 37: case 38: case 39: case 40: return PL_SNARE;
    case 42: case 44: return PL_HATC;
    case 46: return PL_HATO;
    case 49: case 51: case 52: case 55: case 57: case 59: return PL_CYM;
    default: return PL_BASS;
    }
}

static inline uint32_t pulsar_hz(int32_t hz10)         /* phase increment of hz10 / 10 Hz at 44.1 kHz */
{
    return (uint32_t)hz10 * 9739u;                         /* 2^32 / 44100 / 10 = 9739.1 */
}

/* the trapezoidal SVF of dsp.c, one sample: the low, band and high outputs */
static inline __attribute__((always_inline)) void pulsar_svf(const tsvf_t *f, int32_t k, int32_t in, int32_t *ic1,
                                                            int32_t *ic2, int32_t *lp, int32_t *bp, int32_t *hp)
{
    int32_t v3 = in - *ic2;
    int32_t b = (f->a1 * *ic1 + f->a2 * v3) >> 13;
    int32_t l = *ic2 + ((f->a2 * *ic1 + f->a3 * v3) >> 13);
    *ic1 = clamp(2 * b - *ic1, -150000, 150000);
    *ic2 = clamp(2 * l - *ic2, -150000, 150000);
    *lp = l;
    *bp = b;
    *hp = in - ((k * b) >> 12) - l;
}

static void pulsar_note_on(track_t *t, voice_t *v)
{
    uint32_t lane = (uint32_t)(v - t->v);
    v->s[3] = (int32_t)pulsar_voice_of(v->note);
    v->s[0] = 1 << 24;                                    /* every hit starts at full level */
    v->s[1] = 1 << 24;                                    /* and at the top of its sweep */
    if (!v->s[2])
        v->s[2] = (int32_t)(0x1234567u + lane * 0x9E3779B9u);
    v->s[4] = v->s[5] = 0;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;                   /* drums start in phase: the kick's first cycle is its punch */
    (void)t;
}

/* the decay time (ENV_EXP index) of a voice: at most about 3 s, so every hit dies away */
static uint32_t pulsar_decay(const int16_t *p, uint32_t pv)
{
    switch (pv) {
    case PL_KICK: return (uint32_t)clamp(p[P_E1], 0, 115);
    case PL_SNARE: return (uint32_t)clamp(p[P_E3], 0, 115);
    case PL_HATC: return (uint32_t)clamp(p[P_E4] / 2 + 22, 0, 115);
    case PL_HATO: return (uint32_t)clamp(p[P_E4] + 12, 0, 115);
    case PL_CYM: return (uint32_t)clamp(p[P_E4] + 34, 0, 115);
    default: return (uint32_t)clamp(p[P_E5], 0, 115);
    }
}

/* the voice's amplitude, once a control tick: its own decay (or SUST's hold while the key is down); the voice ends
 * when it has died away. The host scales it by the velocity. */
static int32_t pulsar_amp(track_t *t, voice_t *v, int32_t adsr)
{
    const int16_t *p = t->p;
    int32_t e = v->s[0], hold = (int32_t)p[P_E7] << 17;   /* SUST 127 = 1.0 (Q24) */
    (void)adsr;
    if (!v->active)
        return 0;
    if (v->gate && hold && e <= hold)
        e = hold;                                          /* a drone while held */
    else
        e -= mulq16(e, ENV_EXP[pulsar_decay(p, (uint32_t)v->s[3])]);
    if (e < (1 << 15)) {                                /* -54 dB: done */
        v->active = v->gate = 0;
        v->stage = 0;
        v->env = 0;
        v->s[0] = 0;
        return 0;
    }
    v->s[0] = e;
    v->env = 1 << 24;
    return e >> 9;                                         /* Q15 */
}

/* the metal: six square waves at the classic inharmonic ratios (Hz x 10), summed */
static const uint16_t PL_METAL[6] = {2053, 3044, 3696, 5227, 5400, 8000};

static void pulsar_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t pv = (uint32_t)v->s[3], i;
    int32_t sweep = v->s[1], ic1 = v->s[4], ic2 = v->s[5];
    int32_t drive = 4096 + p[P_E6] * p[P_E6] / 2;          /* Q12: x1 .. x9, a squared knob */
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    tsvf_t f;
    switch (pv) {
    case PL_KICK: {
        /* base 30..80 Hz (KICK), the sweep starts 4 octaves up and falls in about 40 ms */
        int32_t base10 = 300 + p[P_E0] * 4;                /* Hz x 10 */
        uint32_t inc0 = pulsar_hz(base10), incs;
        int32_t sw = sweep >> 12;                           /* Q12 */
        incs = inc0 + (uint32_t)(((uint64_t)inc0 * 15u * (uint32_t)((sw * sw) >> 12)) >> 12);
        for (i = 0; i < n; i++) {
            int32_t s = osc_sine(ph0);
            if (sweep > (15 << 20)) {                       /* the click: the first few ms */
                int32_t c = (int32_t)(noise32(&v->s[2]) >> 18) - 8192;
                s += mulq15(c, (sweep >> 9) - 30720 > 0 ? (sweep >> 9) - 30720 : 0);
            }
            s = softclip((s * drive) >> 12);
            ph0 += incs;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 3300);                       /* about 40 ms to -24 dB */
        break;
    }
    case PL_SNARE: {
        /* two tones (185 and 330 Hz) dropping a little, and noise band-passed around 2.5 kHz; SNAP mixes them */
        int32_t snap = p[P_E2] * 258, tone = 32767 - snap, sw = sweep >> 13;
        uint32_t inc0 = pulsar_hz(1850 + (sw * 900 >> 11)), inc1 = pulsar_hz(3300 + (sw * 1300 >> 11));
        int32_t k = 4096 + 2048;
        tsvf_coef_k(&f, 106 << 8, k);
        for (i = 0; i < n; i++) {
            int32_t nz = (int32_t)(noise32(&v->s[2]) >> 16) - 32768, lp, bp, hp, s;
            pulsar_svf(&f, k, nz, &ic1, &ic2, &lp, &bp, &hp);
            s = mulq15((osc_sine(ph0) + osc_sine(ph1)) >> 1, tone) + mulq15(clamp(bp + hp / 2, -40000, 40000), snap);
            s = softclip((s * drive) >> 12);
            ph0 += inc0;
            ph1 += inc1;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 2000);
        break;
    }
    case PL_HATC: case PL_HATO: case PL_CYM: {
        /* the six squares need no stored phases: ph[0] counts the samples since the hit, and a square's phase is that
         * count times its increment (mod 2^32, exactly as an accumulator would hold it). High-passed, with noise */
        uint32_t inc[6], j, c = ph0;
        int32_t k = 4096 + 1024;
        for (j = 0; j < 6; j++)
            inc[j] = pulsar_hz(PL_METAL[j] * (pv == PL_CYM ? 3u : 4u) / 4u);
        tsvf_coef_k(&f, (pv == PL_CYM ? 112 : 118) << 8, k);
        for (i = 0; i < n; i++, c++) {
            int32_t sum = 0, nz = (int32_t)(noise32(&v->s[2]) >> 17) - 16384, lp, bp, hp, s;
            for (j = 0; j < 6; j++)
                sum += (int32_t)((c * inc[j]) >> 31) ? -5000 : 5000;
            pulsar_svf(&f, k, sum + nz, &ic1, &ic2, &lp, &bp, &hp);
            s = softclip((hp * drive) >> 12);
            out[i] += voice_amp(s, m, i);
        }
        ph0 = c;
        break;
    }
    default: {
        /* BASS: the note's pitch, an octave drop in the first ~20 ms; triangle and square through a low-pass that
         * opens with the sweep and the drive */
        int32_t sw = sweep >> 12, cut, k = 4096 + 1500;
        uint32_t inc0 = m->inc + (uint32_t)(((uint64_t)m->inc * (uint32_t)((sw * sw) >> 12)) >> 12);
        cut = clamp(40 * 256 + (sweep >> 11) * 5 + p[P_E6] * 64, 0, 127 * 256);
        tsvf_coef_k(&f, cut, k);
        for (i = 0; i < n; i++) {
            int32_t sq = (int32_t)(ph0 >> 31) ? -20000 : 20000, s, lp, bp, hp;
            s = (osc_tri(ph0) + sq) >> 1;
            pulsar_svf(&f, k, s, &ic1, &ic2, &lp, &bp, &hp);
            s = softclip((lp * drive) >> 12);
            ph0 += inc0;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 5000);
        break;
    }
    }
    v->s[1] = sweep;
    v->s[4] = ic1;
    v->s[5] = ic2;
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
}

/* the part's sum, after its voices: the glue. DRIVE also pushes the whole kit into a soft clipper, so hits
 * squash together the way a hot analog bus does; quiet passages stay clean */
static void pulsar_post(track_t *t, int32_t *out, uint32_t n, uint32_t nr)
{
    int32_t g = 4096 + t->p[P_E6] * 24, i;                  /* Q12: x1 .. x1.75 */
    (void)nr;
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = (int32_t)(((int64_t)out[i] * g) >> 13);   /* half scale into the clipper */
        out[i] = softclip(x) * 2;
    }
}

static const preset_t PULSAR_PRESETS[] = {
    /* name, {KICK, K.DEC, SNAP, S.DEC, METAL, B.DEC, DRIVE, SUST}, {A D S R}, fenv, mono */
    {"PULSAR", {48, 94, 70, 74, 66, 84, 60, 0}, {0, 127, 127, 127}, 0, 0},
    {"PULVERISE", {40, 98, 90, 80, 72, 88, 118, 0}, {0, 127, 127, 127}, 0, 0},
    {"DEEP SUB", {20, 108, 50, 70, 60, 100, 46, 0}, {0, 127, 127, 127}, 0, 0},
    {"TIGHT", {64, 80, 60, 62, 46, 70, 40, 0}, {0, 127, 127, 127}, 0, 0},
    {"DRONE", {30, 100, 80, 82, 84, 96, 80, 90}, {0, 127, 127, 127}, 0, 0},
    {"NOISE BED", {50, 92, 120, 92, 104, 84, 96, 60}, {0, 127, 127, 127}, 0, 0},
};

static const engine_t ENG_PULSAR = {
    .name = "PULSAR",
    .page_title = {"KICK SNARE", "METAL BASS"},
    .edit = {
        {"KICK", F_PCT, 0, 127, 48, 0, 0},
        {"K.DEC", F_TIME, 0, 127, 94, 0, 0},
        {"SNAP", F_PCT, 0, 127, 70, 0, 0},
        {"S.DEC", F_TIME, 0, 127, 74, 0, 0},
        {"METAL", F_TIME, 0, 127, 66, 0, 0},
        {"B.DEC", F_TIME, 0, 127, 84, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 60, 0, 0},
        {"SUST", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = PULSAR_PRESETS,
    .npresets = NELEM(PULSAR_PRESETS),
    .knob = {P_E6, P_E1, P_E2, P_E7},
    .poly = 8,
    .oneshot = 1,
    .note_on = pulsar_note_on,
    .render = pulsar_render,
    .amp = pulsar_amp,
    .post = pulsar_post,
};
