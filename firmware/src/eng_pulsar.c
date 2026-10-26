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
 * SHAOS (after the Pulsar's SHift + chAOS): a 16-bit shift register per part, stepped once a hit. Its top bit comes
 * back in at the bottom, flipped now and then: rarely at low SHAOS, so the variations repeat as a loop (a few bits
 * change, the loop mutates slowly), often at high SHAOS, where it is noise. Three taps of the register vary each hit,
 * as deep as SHAOS: the kick's pitch and drive, the snare's SNAP, pitch and drive, the metal's tuning and tone, the
 * bass's filter and drive, and every voice's decay. SHAOS 0: every hit the same. The BASS voice decays as the KICK.
 * Voice state: s[0] the amplitude (Q24), s[1] the pitch sweep (Q24), s[2] noise, s[3] the voice (PL_*, bits 0..7)
 * and its three SHAOS values (signed bytes, bits 8..15, 16..23, 24..31), s[4] s[5] the voice's filter. */
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

/* SHAOS: the parts' shift registers and the RNG that flips their bits */
static uint16_t shaos_reg[NPART] = {0xACE1u, 0x5A3Cu, 0xC0DEu, 0x7E57u};
static int32_t shaos_rng[NPART] = {0x2468ACE, 0x13579BD, 0x0FEDCBA, 0x7654321};

/* step part's register for a hit; its three taps scaled by SHAOS (a 0..127), packed as s[3]'s bytes 1..3 */
static uint32_t shaos_step(uint32_t part, int32_t a)
{
    uint32_t r, in;
    int32_t m1, m2, m3;
    if (part >= NPART || a <= 0)
        return 0;
    r = shaos_reg[part];
    in = r >> 15;
    if ((noise32(&shaos_rng[part]) >> 25) < (uint32_t)(a * a / 128))   /* flip odds: 0 .. 126 / 128 */
        in ^= 1u;
    r = ((r << 1) | in) & 0xFFFFu;
    shaos_reg[part] = (uint16_t)r;
    m1 = ((int32_t)(int8_t)(r & 0xFFu)) * a / 127;
    m2 = ((int32_t)(int8_t)((r >> 5) & 0xFFu)) * a / 127;
    m3 = ((int32_t)(int8_t)((r >> 8) & 0xFFu)) * a / 127;
    return ((uint32_t)(uint8_t)m1 << 8) | ((uint32_t)(uint8_t)m2 << 16) | ((uint32_t)(uint8_t)m3 << 24);
}
static inline int32_t shaos_m(const voice_t *v, uint32_t k) { return (int32_t)(int8_t)((uint32_t)v->s[3] >> (8u * k)); }

static void pulsar_note_on(track_t *t, voice_t *v)
{
    uint32_t lane = (uint32_t)(v - t->v);
    v->s[3] = (int32_t)(pulsar_voice_of(v->note) | shaos_step((uint32_t)(t - trk), t->p[P_E5]));
    v->s[0] = 1 << 24;                                    /* every hit starts at full level */
    v->s[1] = 1 << 24;                                    /* and at the top of its sweep */
    if (!v->s[2])
        v->s[2] = (int32_t)(0x1234567u + lane * 0x9E3779B9u);
    v->s[4] = v->s[5] = 0;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;                   /* drums start in phase: the kick's first cycle is its punch */
    (void)t;
}

/* the decay time (ENV_EXP index) of a voice: at most about 3 s, so every hit dies away; SHAOS moves it */
static uint32_t pulsar_decay(const int16_t *p, const voice_t *v)
{
    int32_t d, sh = shaos_m(v, 3) / 6;
    switch ((uint32_t)v->s[3] & 0xFFu) {
    case PL_KICK: d = p[P_E1]; break;
    case PL_SNARE: d = p[P_E3] + 6; break;
    case PL_HATC: d = p[P_E4] / 2 + 22; break;
    case PL_HATO: d = p[P_E4] + 12; break;
    case PL_CYM: d = p[P_E4] + 34; break;
    default: d = p[P_E1] - 10; break;               /* BASS: as the kick */
    }
    return (uint32_t)clamp(d + sh, 0, 115);
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
        e -= mulq16(e, ENV_EXP[pulsar_decay(p, v)]);
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

/* The grit: the Pulsar's channels run hot into analog distortion. a 0..127 (DRIVE, more on some voices):
 *   up to 64   a tanh drive, x1 .. x6 (warm to saturated)
 *   64 .. 127  harder (to x21), a wavefolder blended in (a clipper folding over: the rasp), and a bias that makes it
 *              asymmetric (even harmonics: the industrial edge). The part's DC blocker (post) takes the bias out.
 * x up to +-65536; the result +-32767 */
static inline __attribute__((always_inline)) int32_t pulsar_grit(int32_t x, int32_t a)
{
    int32_t g = 4096 + a * a * 5, d = (int32_t)(((int64_t)x * g) >> 12), y = softclip(d);
    if (a > 64) {
        int32_t f = d + (a - 64) * 160;
        uint32_t u = (uint32_t)(f + 32768) & 0x1FFFFu;         /* a triangle fold, period 4 x 32768 */
        int32_t fold = u < 65536u ? (int32_t)u - 32768 : 98303 - (int32_t)u;
        y += mulq15(fold - y, (a - 64) * 260);                 /* up to half folded */
    }
    return y;
}

static void pulsar_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t pv = (uint32_t)v->s[3] & 0xFFu, i;
    int32_t sweep = v->s[1], ic1 = v->s[4], ic2 = v->s[5], m1 = shaos_m(v, 1), m2 = shaos_m(v, 2);
    int32_t dr = clamp(p[P_E6] + m2 / 3, 0, 127);
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    tsvf_t f;
    switch (pv) {
    case PL_KICK: {
        /* base 30..80 Hz (KICK); the sweep starts 5 octaves up and falls in about 40 ms; a noise click and a
         * square's edge on the attack, then the sine through the grit: the drive turns the tail into a growl */
        int32_t base10 = clamp(300 + p[P_E0] * 4 + m1 * 2, 200, 1100);   /* Hz x 10; SHAOS: +-25 Hz */
        uint32_t inc0 = pulsar_hz(base10), incs;
        int32_t sw = sweep >> 12, click = sweep > (13 << 20) ? (sweep >> 9) - 26624 : 0;   /* Q12; click Q15 */
        incs = inc0 + (uint32_t)(((uint64_t)inc0 * 31u * (uint32_t)((sw * sw) >> 12)) >> 12);
        for (i = 0; i < n; i++) {
            int32_t s = osc_sine(ph0) + (osc_sine(ph0 * 2u) >> 3);   /* (a little second harmonic: the punch) */
            if (click > 0) {
                int32_t c = (int32_t)(noise32(&v->s[2]) >> 17) - 16384 + ((int32_t)(ph0 >> 31) ? -12000 : 12000);
                s += mulq15(c, click);
            }
            s = pulsar_grit(s, clamp(dr - 8, 0, 127));
            ph0 += incs;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 3300);
        break;
    }
    case PL_SNARE: {
        /* body: two triangles (180 and 290 Hz) an octave up at the hit and falling in ~15 ms, fading with the sweep;
         * the snap: noise through a resonant band that sweeps down from ~6 kHz, plus its highs; the rattle: noise
         * ring-modulated by a 410 Hz square (the metal in it). SNAP moves it from a thudding tom to a noise crack;
         * the grit runs hotter than the kick's: the snare is where the Pulsar's distortion lives */
        int32_t snap = clamp(p[P_E2] + m1 / 2, 0, 127), sw = sweep >> 12, body = 32767 - snap * 180, nzl = 1500 + snap * snap * 3 / 2;
        int32_t bamp = 9000 + (sweep >> 9), rat = (snap * (600 + dr * 12)) >> 6, k = 4096 + 700, cut;
        uint32_t oct = (uint32_t)((sw * sw) >> 12), inc0 = pulsar_hz(1800 + m2 * 4), inc1 = pulsar_hz(2900 + m2 * 6), inc2 = pulsar_hz(4100 + m1 * 8);
        inc0 += (uint32_t)(((uint64_t)inc0 * oct) >> 12);
        inc1 += (uint32_t)(((uint64_t)inc1 * oct) >> 12);
        cut = clamp((82 << 8) + (sweep >> 11) * 4, 0, 125 << 8);   /* the crack sweeps down into the mids */
        tsvf_coef_k(&f, cut, k);
        body = mulq15(body, bamp > 32767 ? 32767 : bamp);
        for (i = 0; i < n; i++) {
            int32_t nz = (int32_t)(noise32(&v->s[2]) >> 16) - 32768, lp, bp, hp, s, tone, rattle;
            pulsar_svf(&f, k, nz, &ic1, &ic2, &lp, &bp, &hp);
            tone = (osc_tri(ph0) + osc_tri(ph1)) >> 1;
            rattle = (int32_t)(ph2 >> 31) ? nz >> 1 : -(nz >> 1);
            s = mulq15(tone, body) + mulq15(clamp(bp * 2 + hp, -60000, 60000), nzl) + mulq15(rattle, rat);
            s = pulsar_grit(s, clamp(dr + 14, 0, 127));
            ph0 += inc0;
            ph1 += inc1;
            ph2 += inc2;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 1400);
        break;
    }
    case PL_HATC: case PL_HATO: case PL_CYM: {
        /* the six squares need no stored phases: ph[0] counts the samples since the hit, and a square's phase is that
         * count times its increment (mod 2^32, exactly as an accumulator would hold it). High-passed, with noise */
        uint32_t inc[6], j, c = ph0;
        int32_t k = 4096 + 1024;
        for (j = 0; j < 6; j++)
            inc[j] = pulsar_hz((int32_t)PL_METAL[j] * (pv == PL_CYM ? 3 : 4) / 4 * (256 + m1) / 256);   /* SHAOS: +-50 % */
        tsvf_coef_k(&f, clamp((pv == PL_CYM ? 112 : 118) * 256 + m2 * 24, 90 << 8, 125 << 8), k);
        for (i = 0; i < n; i++, c++) {
            int32_t sum = 0, nz = (int32_t)(noise32(&v->s[2]) >> 17) - 16384, lp, bp, hp, s;
            for (j = 0; j < 6; j++)
                sum += (int32_t)((c * inc[j]) >> 31) ? -5000 : 5000;
            pulsar_svf(&f, k, sum + nz, &ic1, &ic2, &lp, &bp, &hp);
            s = pulsar_grit(hp, dr / 2 + 20);
            out[i] += voice_amp(s, m, i);
        }
        ph0 = c;
        break;
    }
    default: {
        /* BASS: the note's pitch, an octave drop in the first ~20 ms; triangle and square through a resonant low-pass
         * that opens with the sweep and the drive, then the grit */
        int32_t sw = sweep >> 12, cut, k = 4096 + 1100;
        uint32_t inc0 = m->inc + (uint32_t)(((uint64_t)m->inc * (uint32_t)((sw * sw) >> 12)) >> 12);
        cut = clamp(44 * 256 + (sweep >> 11) * 5 + dr * 64 + m1 * 40, 0, 127 * 256);
        tsvf_coef_k(&f, cut, k);
        for (i = 0; i < n; i++) {
            int32_t sq = (int32_t)(ph0 >> 31) ? -24000 : 24000, s, lp, bp, hp;
            s = (osc_tri(ph0) + sq) >> 1;
            pulsar_svf(&f, k, s, &ic1, &ic2, &lp, &bp, &hp);
            s = pulsar_grit(lp, dr);
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

/* the part's sum, after its voices: a DC blocker (the grit's bias), then the glue: DRIVE pushes the whole kit into
 * a soft clipper with make-up gain, so hits squash together the way a hot analog bus does (x1.4 .. x3.3 in) */
static int32_t pulsar_dc[NPART][2];
static void pulsar_post(track_t *t, int32_t *out, uint32_t n, uint32_t nr)
{
    uint32_t part = (uint32_t)(t - trk) % NPART, i;
    int32_t g = 5600 + t->p[P_E6] * 62, xin = pulsar_dc[part][0], yo = pulsar_dc[part][1];   /* Q12 */
    (void)nr;
    for (i = 0; i < n; i++) {
        int32_t x = out[i], y = x - xin + yo - (yo >> 9);                   /* ~14 Hz high-pass */
        xin = x;
        yo = y;
        out[i] = softclip((int32_t)(((int64_t)y * g) >> 13)) * 2;            /* half scale in, x2 out */
    }
    pulsar_dc[part][0] = xin;
    pulsar_dc[part][1] = yo;
}

static const preset_t PULSAR_PRESETS[] = {
    /* name, {KICK, K.DEC, SNAP, S.DEC, METAL, SHAOS, DRIVE, SUST}, {A D S R}, fenv, mono */
    {"PULSAR", {48, 94, 70, 74, 66, 0, 60, 0}, {0, 127, 127, 127}, 0, 0},
    {"PULVERISE", {40, 98, 90, 80, 72, 30, 118, 0}, {0, 127, 127, 127}, 0, 0},
    {"DEEP SUB", {20, 108, 50, 70, 60, 0, 46, 0}, {0, 127, 127, 127}, 0, 0},
    {"TIGHT", {64, 80, 60, 62, 46, 0, 40, 0}, {0, 127, 127, 127}, 0, 0},
    {"DRONE", {30, 100, 80, 82, 84, 50, 80, 90}, {0, 127, 127, 127}, 0, 0},
    {"NOISE BED", {50, 92, 120, 92, 104, 80, 96, 60}, {0, 127, 127, 127}, 0, 0},
    {"ORGANISM", {44, 96, 76, 78, 70, 64, 84, 0}, {0, 127, 127, 127}, 0, 0},
    {"SHAOS", {40, 100, 96, 84, 86, 116, 100, 0}, {0, 127, 127, 127}, 0, 0},
};

static const engine_t ENG_PULSAR = {
    .name = "PULSAR",
    .page_title = {"KICK SNARE", "METAL CHAOS"},
    .edit = {
        {"KICK", F_PCT, 0, 127, 48, 0, 0},
        {"K.DEC", F_TIME, 0, 127, 94, 0, 0},
        {"SNAP", F_PCT, 0, 127, 70, 0, 0},
        {"S.DEC", F_TIME, 0, 127, 74, 0, 0},
        {"METAL", F_TIME, 0, 127, 66, 0, 0},
        {"SHAOS", F_PCT, 0, 127, 0, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 60, 0, 0},
        {"SUST", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = PULSAR_PRESETS,
    .npresets = NELEM(PULSAR_PRESETS),
    .knob = {P_E6, P_E2, P_E5, P_E7},    /* DRIVE, SNAP, SHAOS, SUST */
    .poly = 8,
    .oneshot = 1,
    .note_on = pulsar_note_on,
    .render = pulsar_render,
    .amp = pulsar_amp,
    .post = pulsar_post,
};
