/* SPDX-License-Identifier: GPL-3.0-only */
/* PULSAR: big, driven drums after the sound of SOMA's Pulsar-23 (original DSP: no circuit model, no samples).
 * Four channels, chosen by the note as the General MIDI drum map plays them, so the K.O. II and the keys both work:
 *   BD     35 36           a sine with a tune envelope (DROP: how far and how long it drops), a click, the grit;
 *                          from sub booms to distorted industrial thumps; SHP (the host's) rings it metallic
 *   SD     38 (snare), 40 (a higher, noisier one), 37 (rimshot), 39 (clap): a tunable drum (SNARE) of three membrane
 *                          partials (1, 1.59, 2.14 of the pitch, the higher ones dying first) with its own pitch drop,
 *                          noise through a resonant band that sweeps down, a rattle (noise ring-modulated by a square);
 *                          SNAP from a hollow tom-like body to a noise burst
 *   HH     42 44 (closed), 46 (open), 49 51 52 55 57 59 (cymbals): TONE moves through three sources: six square
 *                          waves at inharmonic ratios (the classic metal), the same squares ring-modulated in pairs
 *                          (ringing metal), then a digital noise register run at a crushed rate (lo-fi digital
 *                          texture); high-passed, the grit
 *   BASS   every other note (toms 41 43 45 47 48 50, and any pitch): the note's pitch, a pitch drop (DROP), triangle,
 *                          square and a sub square through a resonant low-pass (TONE: from a round tom to a screaming
 *                          resonant lead)
 * The decays are on the ENV page (the host's ATK DEC SUS REL, unused by hits, named K.DEC S.DEC M.DEC B.DEC here).
 * SUST (the Pulsar's sustaining envelopes): above 0, a held key keeps its voice at that level: any channel becomes a
 * drone or a synth voice. DRIVE: each voice's grit and the bus (post): warm to pulverised.
 * SHAOS (SHift + chAOS): a 16-bit shift register per part, stepped once a hit, its top bit back in at the bottom,
 * flipped now and then: rarely at low SHAOS (the variations loop and slowly mutate), often at high SHAOS (noise).
 * Three taps vary each hit as deep as SHAOS: pitches, SNAP, tuning, tone, drive and every decay.
 * The host's modulation reaches the drums: PIT (LFO, ENV, the matrix: S&H, random) bends every channel's pitch, FLT
 * moves the snare's band, the metal's and the bass's filters, SHP the kick's ring and the metal's ring-modulation.
 * Voice state: s[0] the amplitude (Q24), s[1] the tune envelope (Q24, also the body's envelope), s[2] noise, s[3] the
 * channel (PL_*, bits 0..7) and its three SHAOS values (signed bytes), s[4] s[5] the voice's filter. ph[]: the
 * oscillators (the metal: ph[0] counts samples since the hit). */
enum { PL_KICK, PL_BASS, PL_SNARE, PL_HATC, PL_HATO, PL_CYM, PL_SNARE2, PL_RIM, PL_CLAP };

static uint32_t pulsar_voice_of(uint32_t note)
{
    switch (note) {
    case 35: case 36: return PL_KICK;
    case 37: return PL_RIM;
    case 38: return PL_SNARE;
    case 39: return PL_CLAP;
    case 40: return PL_SNARE2;
    case 42: case 44: return PL_HATC;
    case 46: return PL_HATO;
    case 49: case 51: case 52: case 55: case 57: case 59: return PL_CYM;
    default: return PL_BASS;
    }
}
static int pulsar_is_snare(uint32_t pv) { return pv == PL_SNARE || pv == PL_SNARE2 || pv == PL_RIM || pv == PL_CLAP; }
static int pulsar_is_metal(uint32_t pv) { return pv == PL_HATC || pv == PL_HATO || pv == PL_CYM; }

static inline uint32_t pulsar_hz(int32_t hz10)         /* phase increment of hz10 / 10 Hz at 44.1 kHz */
{
    return (uint32_t)hz10 * 9739u;                         /* 2^32 / 44100 / 10 = 9739.1 */
}
static inline uint32_t pulsar_scale(uint32_t inc, uint32_t q16)   /* inc x q16 / 65536 */
{
    return (uint32_t)(((uint64_t)inc * q16) >> 16);
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
    v->s[1] = 1 << 24;                                    /* and at the top of its tune envelope */
    if (!v->s[2])
        v->s[2] = (int32_t)(0x1234567u + lane * 0x9E3779B9u);
    v->s[4] = v->s[5] = 0;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;                   /* drums start in phase: the kick's first cycle is its punch */
}

/* the decay time (ENV_EXP index) of a voice, from the ENV page (K.DEC S.DEC M.DEC B.DEC); at most about 3 s, so every
 * hit dies away; SHAOS moves it */
static uint32_t pulsar_decay(const int16_t *p, const voice_t *v)
{
    int32_t d, sh = shaos_m(v, 3) / 6;
    switch ((uint32_t)v->s[3] & 0xFFu) {
    case PL_KICK: d = p[P_ATK]; break;
    case PL_SNARE: case PL_SNARE2: d = p[P_DEC] + 6; break;
    case PL_RIM: d = p[P_DEC] / 2 + 10; break;
    case PL_CLAP: d = p[P_DEC] + 2; break;
    case PL_HATC: d = p[P_SUS] / 2 + 22; break;
    case PL_HATO: d = p[P_SUS] + 12; break;
    case PL_CYM: d = p[P_SUS] + 34; break;
    default: d = p[P_REL]; break;
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
    if (e < (1 << 15)) {                                   /* -54 dB: done */
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

/* the metal: six square waves at the classic inharmonic ratios (Hz x 10) */
static const uint16_t PL_METAL[6] = {2053, 3044, 3696, 5227, 5400, 8000};

/* The grit: the Pulsar's channels run hot into analog distortion. a 0..127:
 *   up to 64   a tanh drive, x1 .. x6 (warm to saturated)
 *   64 .. 127  harder (to x21), a wavefolder blended in (a clipper folding over: the rasp), and a bias that makes it
 *              asymmetric (even harmonics: the industrial edge). The bus's DC blocker (post) takes the bias out.
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
    int32_t dr = clamp(p[P_E6] + m2 / 3, 0, 127), punch = p[P_E1], timbre = p[P_E4];
    int32_t bend = clamp(m->pitch16 - v->pitch16, -24 * 16, 24 * 16);   /* the host's PIT: LFO, ENV, the matrix */
    int32_t flt = m->cutoff, shp = m->shape - 64 * 256;                  /* FLT (0..127 << 8), SHP around its middle */
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], bq = pow2_q16(bend);
    tsvf_t f;
    if (pv == PL_KICK) {
        /* KICK 30..90 Hz; DROP: the drop from 1 to 6 octaves up, in 15 to 70 ms; a click and a square's edge on the
         * attack; SHP rings it (a partial at 2.76 times, ring-modulated); the grit turns the tail into a growl */
        int32_t base10 = clamp(300 + p[P_E0] * 5 + m1 * 2, 200, 1400);
        uint32_t inc0 = pulsar_scale(pulsar_hz(base10), bq), incs, depth = 1u + (uint32_t)punch * 62u / 127u;
        int32_t sw = sweep >> 12, click = sweep > (13 << 20) ? (sweep >> 9) - 26624 : 0;
        int32_t ring = shp > 0 ? shp >> 1 : 0;              /* Q15 */
        incs = inc0 + (uint32_t)(((uint64_t)inc0 * depth * (uint32_t)((sw * sw) >> 12)) >> 12);
        for (i = 0; i < n; i++) {
            int32_t s = osc_sine(ph0) + (osc_sine(ph0 * 2u) >> 3);
            if (ring)
                s += mulq15(mulq15(s, osc_sine(ph1)), ring);
            if (click > 0) {
                int32_t c = (int32_t)(noise32(&v->s[2]) >> 17) - 16384 + ((int32_t)(ph0 >> 31) ? -12000 : 12000);
                s += mulq15(c, click);
            }
            s = pulsar_grit(s, clamp(dr - 8, 0, 127));
            ph0 += incs;
            ph1 += pulsar_scale(incs, 180879u);              /* x 2.76 */
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, (uint32_t)(5200 - punch * 30));
    } else if (pulsar_is_snare(pv)) {
        /* SNARE 120..420 Hz (the rimshot 1.8 times, the second snare 1.25 times); three partials of a membrane, the
         * higher ones dying first (the tune envelope squared and cubed); a pitch drop (up to a fifth with DROP);
         * the noise band sweeps down from the crack into the mids; the rattle; SNAP balances body and noise.
         * The clap: three noise bursts 11 ms apart, then the band's tail. The grit runs hotter than the kick's */
        int32_t base10 = clamp(1200 + p[P_E2] * 24 + m1 * 6, 800, 4400);
        int32_t snap = clamp(p[P_E3] + m1 / 2, 0, 127), sw = sweep >> 12, k = 4096 + 700, cut;
        int32_t body = 32767 - snap * 190, nzl = 1500 + snap * snap * 3 / 2, rat = (snap * (600 + dr * 12)) >> 6;
        int32_t e1 = sweep >> 9, e2 = (e1 * e1) >> 15;     /* the upper partials' envelopes, Q15 (the fundamental rings
                                                            * with the voice's own amplitude) */
        uint32_t drop, inc0, inc1, inc3, c = ph2;
        if (pv == PL_RIM) { base10 = base10 * 9 / 5; nzl /= 3; body = 32767; }
        else if (pv == PL_SNARE2) { base10 = base10 * 5 / 4; nzl = nzl * 3 / 2; }
        else if (pv == PL_CLAP) { body = 2000; nzl = 30000; rat = 0; k = 4096 + 1800; }
        drop = 65536u + (uint32_t)(((int64_t)(sw * sw >> 12) * (punch * 196)) >> 12);   /* up to x1.5 */
        inc0 = pulsar_scale(pulsar_scale(pulsar_hz(base10), bq), drop);
        inc1 = pulsar_scale(inc0, 104399u);                  /* x 1.593 */
        inc3 = pulsar_scale(inc0, 150733u);                  /* x 2.30: the rattle's square */
        cut = clamp((82 << 8) + (sweep >> 11) * 4 + (flt >> 1), 40 << 8, 125 << 8);
        if (pv == PL_CLAP) cut = clamp((96 << 8) + (flt >> 1), 60 << 8, 122 << 8);
        tsvf_coef_k(&f, cut, k);
        for (i = 0; i < n; i++, c++) {
            int32_t nz = (int32_t)(noise32(&v->s[2]) >> 16) - 32768, lp, bp, hp, s, tone, rattle;
            if (pv == PL_CLAP && c < 1460u) {                /* the bursts: 0, 11, 22 ms, each 8 ms falling */
                uint32_t b = c % 485u;
                nz = b < 360u ? mulq15(nz, 32767 - (int32_t)b * 91) : 0;
            }
            pulsar_svf(&f, k, nz, &ic1, &ic2, &lp, &bp, &hp);
            /* the partials: the fundamental, x1.593 and (the sum of their phases) x2.593 ~ the 2.14 / 2.30 region */
            tone = mulq15(osc_sine(ph0), 11000 + (e1 >> 1)) + (mulq15(osc_sine(ph1), e1) >> 1) +
                   (mulq15(osc_tri(ph1 + ph0), e2) >> 2);
            rattle = (int32_t)((c * inc3) >> 31) ? nz >> 1 : -(nz >> 1);
            s = mulq15(tone, body) + mulq15(clamp(bp * 2 + hp, -60000, 60000), nzl) + mulq15(rattle, rat);
            s = pulsar_grit(s, clamp(dr + 14, 0, 127));
            ph0 += inc0;
            ph1 += inc1;
            out[i] += voice_amp(s, m, i) << 1;
        }
        ph2 = c;
        sweep -= mulq16(sweep, pv == PL_RIM ? 3000u : 1400u);
    } else if (pulsar_is_metal(pv)) {
        /* TONE 0..42: the six squares; 43..85: also ring-modulated in pairs (ringing metal); 86..127: a 15-bit
         * noise register run at a crushed rate (lo-fi digital). ph[0] counts the samples since the hit (a square's
         * phase is that count times its increment), ph[1] the register, ph[2] its hold. SHAOS and PIT retune */
        uint32_t inc[6], j, cnt = ph0, reg = ph1 ? ph1 : 0x4001u, hold = ph2;
        int32_t k = 4096 + 1024, ringmix = clamp((timbre - 42) * 780 + (shp > 0 ? shp : 0), 0, 32767);
        int32_t dig = clamp((timbre - 85) * 780, 0, 32767);
        uint32_t crush = 1u + (uint32_t)clamp(timbre - 85, 0, 42) / 3u;   /* hold 1 .. 15 samples */
        for (j = 0; j < 6; j++)
            inc[j] = pulsar_scale(pulsar_hz((int32_t)PL_METAL[j] * (pv == PL_CYM ? 3 : 4) / 4 * (256 + m1) / 256), bq);
        int32_t nzg = 32767 - timbre * 200;                 /* the white noise gives way to the other sources */
        uint32_t tap = timbre > 105 ? 6u : 14u;              /* past 105: a 7-bit register, periodic: a pitched buzz */
        tsvf_coef_k(&f, clamp((pv == PL_CYM ? 112 : 118) * 256 - timbre * 64 + m2 * 24 + (flt >> 2), 70 << 8, 125 << 8), k);
        for (i = 0; i < n; i++, cnt++) {
            int32_t sq[6], sum = 0, nz = mulq15((int32_t)(noise32(&v->s[2]) >> 17) - 16384, nzg), lp, bp, hp, s, rm, dn;
            for (j = 0; j < 6; j++) {
                sq[j] = (int32_t)((cnt * inc[j]) >> 31) ? -5000 : 5000;
                sum += sq[j];
            }
            rm = ((sq[0] * sq[3]) >> 12) + ((sq[1] * sq[4]) >> 12) + ((sq[2] * sq[5]) >> 12);   /* +-18310 */
            sum += mulq15(rm * 2 - sum, ringmix);
            if (dig) {                                        /* the register: a step every crush samples */
                if (++hold >= crush) {
                    uint32_t bit = (reg ^ (reg >> 1)) & 1u;
                    reg = ((reg >> 1) | (bit << tap)) & 0x7FFFu;
                    hold = 0;
                }
                dn = (int32_t)(reg & 1u) ? 20000 : -20000;
                sum += mulq15(dn - sum, dig);
            }
            pulsar_svf(&f, k, sum + nz, &ic1, &ic2, &lp, &bp, &hp);
            s = pulsar_grit(hp, dr / 2 + 20);
            out[i] += voice_amp(s, m, i);
        }
        ph0 = cnt;
        ph1 = reg;
        ph2 = hold;
    } else {
        /* BASS: the note's pitch (and PIT); DROP: a drop of up to two octaves in the first ~20 ms; triangle, square
         * and a sub square an octave down through a low-pass that opens with the drop, the drive and FLT; TONE:
         * its resonance, from a round tom to a screaming lead */
        int32_t sw = sweep >> 12, cut, k = 4096 + 2400 - timbre * 30;
        uint32_t inc0 = m->inc + (uint32_t)(((uint64_t)m->inc * ((uint32_t)punch * 3u / 127u) * (uint32_t)((sw * sw) >> 12)) >> 12);
        cut = clamp(44 * 256 + (sweep >> 11) * 5 + dr * 64 + m1 * 40 + flt / 2 + timbre * 40, 0, 127 * 256);
        tsvf_coef_k(&f, cut, k);
        for (i = 0; i < n; i++) {
            int32_t sq = (int32_t)(ph0 >> 31) ? -24000 : 24000, sub = (int32_t)(ph1 >> 31) ? -14000 : 14000, s, lp, bp, hp;
            s = (osc_tri(ph0) + sq + sub) >> 1;
            pulsar_svf(&f, k, s, &ic1, &ic2, &lp, &bp, &hp);
            s = pulsar_grit(lp, dr);
            ph0 += inc0;
            ph1 += inc0 >> 1;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 5000);
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

/* the ENV page's four, renamed for PULSAR (params.c track_desc): the channels' decays (hits do not use the ADSR) */
static const param_desc_t PULSAR_DEC[4] = {
    {"K.DEC", F_TIME, 0, 127, 94, 0, 0},
    {"S.DEC", F_TIME, 0, 127, 74, 0, 0},
    {"M.DEC", F_TIME, 0, 127, 66, 0, 0},
    {"B.DEC", F_TIME, 0, 127, 84, 0, 0},
};

static const preset_t PULSAR_PRESETS[] = {
    /* name, {KICK, DROP, SNARE, SNAP, TONE, SHAOS, DRIVE, SUST}, {K.DEC S.DEC M.DEC B.DEC}, fenv, mono */
    {"PULSAR", {48, 70, 50, 70, 20, 0, 60, 0}, {94, 74, 66, 84}, 0, 0},
    {"PULVERISE", {40, 90, 40, 90, 60, 30, 118, 0}, {98, 80, 72, 88}, 0, 0},
    {"DEEP SUB", {20, 40, 30, 50, 10, 0, 46, 0}, {108, 70, 60, 100}, 0, 0},
    {"TIGHT", {64, 60, 70, 60, 30, 0, 40, 0}, {80, 62, 46, 70}, 0, 0},
    {"DRONE", {30, 50, 44, 80, 70, 50, 80, 90}, {100, 82, 84, 96}, 0, 0},
    {"NOISE BED", {50, 80, 60, 120, 110, 80, 96, 60}, {92, 92, 104, 84}, 0, 0},
    {"ORGANISM", {44, 76, 56, 76, 50, 64, 84, 0}, {96, 78, 70, 90}, 0, 0},
    {"SHAOS", {40, 100, 64, 96, 90, 116, 100, 0}, {100, 84, 86, 96}, 0, 0},
    {"INDUSTRY", {36, 110, 80, 84, 100, 40, 122, 0}, {90, 76, 74, 80}, 0, 0},
    {"WOOD", {58, 30, 96, 20, 20, 10, 30, 0}, {70, 52, 50, 60}, 0, 0},
};

static const engine_t ENG_PULSAR = {
    .name = "PULSAR",
    .page_title = {"KICK SNARE", "TONE CHAOS"},
    .edit = {
        {"KICK", F_PCT, 0, 127, 48, 0, 0},
        {"DROP", F_PCT, 0, 127, 70, 0, 0},
        {"SNARE", F_PCT, 0, 127, 50, 0, 0},
        {"SNAP", F_PCT, 0, 127, 70, 0, 0},
        {"TONE", F_PCT, 0, 127, 20, 0, 0},
        {"SHAOS", F_PCT, 0, 127, 0, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 60, 0, 0},
        {"SUST", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = PULSAR_PRESETS,
    .npresets = NELEM(PULSAR_PRESETS),
    .knob = {P_E6, P_E2, P_E3, P_E5},     /* DRIVE, SNARE, SNAP, SHAOS */
    .poly = 8,
    .oneshot = 1,
    .note_on = pulsar_note_on,
    .render = pulsar_render,
    .amp = pulsar_amp,
    .post = pulsar_post,
};
