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
 *   TOM    41 43 45 47 48 50 (and the congas, bongos, timbales 60..66): the note's pitch, a driven membrane with a
 *                          pitch drop (DROP) and a stick's noise; SNARE tunes them too
 *   BELL   53 56 58 67 68 80 81 (ride bell, cowbell, agogo, triangle): two squares band-passed, TONE rings them
 *   KICK2  35: a deeper kick with a longer drop
 *   BASS   every note outside the kit: the note's pitch, a pitch drop (DROP), triangle, square and a sub square through
 *                          a resonant low-pass (TONE: from a round thump to a screaming resonant lead)
 * The keys play the kit (pulsar_keys: as DRUM's, the first C is the kick, then rim, snare, clap ... up the GM map).
 * The decays are on the ENV page (the host's ATK DEC SUS REL, unused by hits, named K.DEC S.DEC M.DEC T.DEC here).
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
enum { PL_KICK, PL_BASS, PL_SNARE, PL_HATC, PL_HATO, PL_CYM, PL_SNARE2, PL_RIM, PL_CLAP, PL_TOM, PL_KICK2, PL_BELL };

/* the kit: the General MIDI drum map (the keys play it as DRUM's do: the first C, key 7, is the kick: pulsar_keys) */
static uint32_t pulsar_voice_of(uint32_t note)
{
    switch (note) {
    case 35: return PL_KICK2;                             /* the deep kick */
    case 36: return PL_KICK;
    case 37: case 75: case 76: case 77: return PL_RIM;    /* rimshot, claves, wood blocks */
    case 38: return PL_SNARE;
    case 39: return PL_CLAP;
    case 40: return PL_SNARE2;
    case 41: case 43: case 45: case 47: case 48: case 50: return PL_TOM;   /* toms, low to high */
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: return PL_TOM;   /* bongos, congas, timbales */
    case 42: case 44: case 54: case 69: case 70: return PL_HATC;   /* closed, pedal; tambourine, cabasa, maracas */
    case 46: case 71: case 72: case 73: case 74: return PL_HATO;
    case 49: case 51: case 52: case 55: case 57: case 59: return PL_CYM;
    case 53: case 56: case 58: case 67: case 68: case 80: case 81: return PL_BELL;   /* ride bell, cowbell, agogo, triangle */
    default: return PL_BASS;                              /* below the kit (and above it): the pitched bass */
    }
}
/* the keys: the GM kit from the first C (as DRUM: ui_input.c, seq.c) */
static int32_t pulsar_keys(const track_t *t, uint32_t k)
{
    return clamp(29 + 12 * input_octave(t) + (int32_t)k, 0, 127);
}
static int pulsar_is_snare(uint32_t pv) { return pv == PL_SNARE || pv == PL_SNARE2 || pv == PL_RIM || pv == PL_CLAP; }
static int pulsar_is_metal(uint32_t pv) { return pv == PL_HATC || pv == PL_HATO || pv == PL_CYM; }
static int pulsar_is_kick(uint32_t pv) { return pv == PL_KICK || pv == PL_KICK2; }

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
    case PL_KICK2: d = p[P_ATK] + 8; break;
    case PL_TOM: d = p[P_REL] + 6; break;
    case PL_BELL: d = p[P_SUS] + 18; break;
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

/* the boom tail's state (post, below): declared here, as a voice keeps the part rendering while the tail rings */
#define PB_N 4u
static struct { uint16_t w[PB_N]; int32_t damp[PB_N], acc, out0, out1, nz, lvl; uint8_t owner, k, on; } pb;

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
        /* the part's boom tail still ringing: one voice stays, silent (a ghost: render skips it), so the part keeps
         * rendering and its bus (post) is heard until the tail dies away */
        uint32_t part = (uint32_t)(t - trk), k;
        if (pb.owner == part + 1u && pb.lvl > 40) {
            for (k = 0; k < NVOICE && !(t->v[k].active && &t->v[k] != v && !t->v[k].s[0]); k++)
                ;
            if (k == NVOICE || !v->s[0]) {               /* no other ghost (or this is it) */
                v->s[0] = 0;
                v->env = 1 << 24;
                return 0;
            }
        }
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
    int32_t flt = m->cutoff, size = p[P_ED_SHP];                         /* FLT (0..127 << 8); SIZE (post) -64..63 */
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], bq = pow2_q16(bend);
    tsvf_t f;
    if (!v->s[0])
        return;                                          /* a ghost: the boom tail's keeper (pulsar_amp) */
    if (pulsar_is_kick(pv)) {
        /* KICK 30..90 Hz; DROP: the drop from 1 to 6 octaves up, in 15 to 70 ms; a click and a square's edge on the
         * attack (softer as SIZE goes muffled); TONE past the middle rings it (a partial at 2.76 times,
         * ring-modulated: a metallic thud); the grit turns the tail into a growl */
        int32_t base10 = clamp(300 + p[P_E0] * 5 + m1 * 2, 200, 1400);
        uint32_t inc0, incs, depth = 1u + (uint32_t)punch * 62u / 127u;
        if (pv == PL_KICK2) {                              /* the deep one: lower, a longer, deeper drop */
            base10 = base10 * 3 / 4;
            depth += depth / 2;
        }
        inc0 = pulsar_scale(pulsar_hz(base10), bq);
        int32_t sw = sweep >> 12, click = sweep > (13 << 20) ? (sweep >> 9) - 26624 : 0;
        int32_t ring = timbre > 64 ? (timbre - 64) * 400 : 0;   /* Q15 */
        if (size < 0)
            click = click * (64 + size) / 64;
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
        sweep -= mulq16(sweep, (uint32_t)(pv == PL_KICK2 ? 3600 - punch * 20 : 5200 - punch * 30));
    } else if (pv == PL_TOM) {
        /* TOM (and the congas, bongos, timbales up the map): the note's pitch, SNARE's tuning (x0.7 .. x1.4); a
         * membrane (the fundamental and a partial at 1.5, dying first), a pitch drop with DROP (up to an octave in the
         * first ~40 ms), a short stick noise through a band, the grit: the Pulsar's toms are driven hard */
        int32_t sw = sweep >> 12, e1 = sweep >> 9, tune = 45875 + p[P_E2] * 361;   /* Q16: 0.7 .. 1.4 */
        uint32_t inc0 = pulsar_scale(pulsar_scale(m->inc, (uint32_t)tune), 65536u), inc1;
        int32_t nzl = 2000 + p[P_E3] * 40, cut = clamp((92 << 8) + (flt >> 1), 60 << 8, 122 << 8);
        inc0 = inc0 + (uint32_t)(((uint64_t)inc0 * (uint32_t)punch * (uint32_t)((sw * sw) >> 12)) >> 19);   /* up to x2 */
        inc1 = pulsar_scale(inc0, 98304u);                    /* x 1.5 */
        tsvf_coef_k(&f, cut, 4096 + 900);
        for (i = 0; i < n; i++) {
            int32_t nz = (int32_t)(noise32(&v->s[2]) >> 16) - 32768, lp, bp, hp, s;
            pulsar_svf(&f, 4096 + 900, nz, &ic1, &ic2, &lp, &bp, &hp);
            s = mulq15(osc_sine(ph0), 26000) + (mulq15(osc_sine(ph1), e1) >> 1) + mulq15(bp, mulq15(nzl, e1));
            s = pulsar_grit(s, clamp(dr + 4, 0, 127));
            ph0 += inc0;
            ph1 += inc1;
            out[i] += voice_amp(s, m, i) << 1;
        }
        sweep -= mulq16(sweep, 2600u);
    } else if (pv == PL_BELL) {
        /* BELL (cowbell, agogo, the ride's bell, triangle): two squares a fifth-and-a-bit apart (the 808's 540 / 800
         * Hz; higher notes a set up), band-passed, driven; TONE rings them against each other */
        uint32_t up = v->note == 53u || v->note == 80u || v->note == 81u ? 2u : v->note == 67u ? 3u : 0u;
        uint32_t ia = pulsar_scale(pulsar_hz(5400 * (2 + (int32_t)up) / 2 * (256 + m1) / 256), bq);
        uint32_t ib = pulsar_scale(ia, 97090u);              /* x 1.48 */
        int32_t ring = timbre * 200;
        tsvf_coef_k(&f, clamp((100 << 8) + (int32_t)up * 1024 + (flt >> 2), 70 << 8, 124 << 8), 4096 + 1600);
        for (i = 0; i < n; i++) {
            int32_t a = (int32_t)(ph0 >> 31) ? -12000 : 12000, b = (int32_t)(ph1 >> 31) ? -9000 : 9000, lp, bp, hp, s;
            s = a + b + mulq15((a * b) >> 12, ring);
            pulsar_svf(&f, 4096 + 1600, s, &ic1, &ic2, &lp, &bp, &hp);
            s = pulsar_grit(bp * 3, dr / 2 + 16);
            ph0 += ia;
            ph1 += ib;
            out[i] += voice_amp(s, m, i) << 1;
        }
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
        int32_t k = 4096 + 1024, ringmix = clamp((timbre - 42) * 780, 0, 32767);
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

/* The bus, after the part's voices: a DC blocker (the grit's bias), SIZE, then the glue (DRIVE pushes the whole kit into
 * a soft clipper with make-up gain, so hits squash together as a hot analog bus does).
 * SIZE (the ENV page's SHP, renamed: params.c), -64 .. 63:
 *   below 0  muffled: two one-pole low-passes close over the kit (from open to ~500 Hz) and the glue eases off
 *            (softer, rounder: soft electronic); the kick's click softens with it (render)
 *   above 0  huge: a sub boost (+6 dB under ~100 Hz at the top), then a dark boom tail: a four-line feedback delay
 *            network run at 1/8 rate (5.5 kHz: only the low end, which is what booms), its feedback and size growing
 *            with SIZE (to a several-second rumble); past 24 the low band's envelope also feeds rolling low noise into
 *            it: big hits leave thunder behind. One part at a time owns the tail (the first that asks).
 * Memory: the tail's lines are 3248 int16 (6.5 KB) */
static int32_t pulsar_bus[NPART][6];             /* DC (in, out), low-passes (2), the sub band, its envelope */
static const uint16_t PB_LEN[PB_N] = {557u, 701u, 887u, 1103u};
static int16_t pb_line[557u + 701u + 887u + 1103u] __attribute__((section(".pool")));

static void pulsar_post(track_t *t, int32_t *out, uint32_t n, uint32_t nr)
{
    uint32_t part = (uint32_t)(t - trk) % NPART, i, j;
    int32_t *b = pulsar_bus[part], size = t->p[P_ED_SHP];
    int32_t g = 5600 + t->p[P_E6] * 62, xin = b[0], yo = b[1], l1 = b[2], l2 = b[3], sub = b[4], env = b[5];
    int32_t lpa = size < 0 ? 32767 - (-size) * 500 : 32767;    /* Q15 one-pole coefficient: 1 .. ~0.02 */
    int32_t fb = size > 0 ? 24000 + size * 70 : 0, wet = size > 0 ? size * 460 : 0;   /* Q15: 0.73 .. 0.87 */
    int32_t thunder = size > 24 ? (size - 24) * 700 : 0;
    int tail = size > 0 && (pb.owner == part + 1u || !pb.owner);
    (void)nr;
    if (size < 0)
        g = g * (128 + size) / 128;                      /* the glue eases as it muffles */
    if (tail && !pb.owner) {                             /* a new owner: the tail from silence */
        memset(pb_line, 0, sizeof pb_line);
        memset(&pb, 0, sizeof pb);
        pb.owner = (uint8_t)(part + 1u);
        pb.nz = 0x1F2E3D4C;
    }
    if (!tail && pb.owner == part + 1u)
        pb.owner = 0;                                    /* SIZE back under 0: give it up */
    for (i = 0; i < n; i++) {
        int32_t x = out[i], y = x - xin + yo - (yo >> 9);        /* ~14 Hz high-pass */
        xin = x;
        yo = y;
        if (size < 0) {                                  /* muffled */
            l1 += (int32_t)(((int64_t)(y - l1) * lpa) >> 15);
            l2 += (int32_t)(((int64_t)(l1 - l2) * lpa) >> 15);
            y = l2;
        } else if (size > 0) {                           /* huge: the sub, its envelope */
            int32_t a = y < 0 ? -y : y;
            sub += (y - sub) >> 6;                       /* ~110 Hz one-pole */
            y += (int32_t)(((int64_t)sub * size) >> 6);  /* up to x2 under it */
            env += a > env ? (a - env) >> 6 : -(env >> 13) - (env > 0);   /* (the -1: it reaches 0) */
        }
        y = softclip((int32_t)(((int64_t)y * g) >> 13)) * 2;   /* the glue: half scale in, x2 out */
        if (tail) {                                      /* the boom: in at 1/8 rate, out interpolated */
            pb.acc += y >> 3;
            if (++pb.k >= 8u) {
                int32_t in = pb.acc, o[PB_N], sum = 0, h0, h1, h2, h3;
                uint32_t base = 0;
                pb.k = 0;
                pb.acc = 0;
                if (thunder) {                           /* rolling noise, as loud as the low band has just been */
                    int32_t nzs = (int32_t)(noise32(&pb.nz) >> 17) - 16384;
                    in += mulq15(mulq15(nzs, clamp(env * 2, 0, 32767)), thunder);
                }
                for (j = 0; j < PB_N; j++) {
                    o[j] = pb_line[base + pb.w[j]];
                    pb.damp[j] += (o[j] - pb.damp[j]) >> 1;   /* dark: a one-pole in each line */
                    o[j] = pb.damp[j];
                    sum += o[j];
                    base += PB_LEN[j];
                }
                /* a Hadamard mix of the four, scaled by the feedback */
                h0 = (o[0] + o[1] + o[2] + o[3]) >> 1;
                h1 = (o[0] - o[1] + o[2] - o[3]) >> 1;
                h2 = (o[0] + o[1] - o[2] - o[3]) >> 1;
                h3 = (o[0] - o[1] - o[2] + o[3]) >> 1;
                base = 0;
                for (j = 0; j < PB_N; j++) {
                    int32_t h = j == 0 ? h0 : j == 1 ? h1 : j == 2 ? h2 : h3;
                    pb_line[base + pb.w[j]] = (int16_t)clamp(mulq15(h, fb) + (in >> 1), -32768, 32767);
                    if (++pb.w[j] >= PB_LEN[j])
                        pb.w[j] = 0;
                    base += PB_LEN[j];
                }
                pb.out0 = pb.out1;
                pb.out1 = mulq15(sum, wet);
                {
                    int32_t a = pb.out1 < 0 ? -pb.out1 : pb.out1;
                    pb.lvl = a > pb.lvl ? a : pb.lvl - (pb.lvl >> 7) - 1;   /* a peak hold, falling (the ghost's cue) */
                    if (pb.lvl < 0) pb.lvl = 0;
                }
            }
            y += pb.out0 + (((pb.out1 - pb.out0) * (int32_t)pb.k) >> 3);
        }
        out[i] = y;
    }
    b[0] = xin; b[1] = yo; b[2] = l1; b[3] = l2; b[4] = sub; b[5] = env;
}

/* the ENV page's four, renamed for PULSAR (params.c track_desc): the channels' decays (hits do not use the ADSR) */
static const param_desc_t PULSAR_DEC[4] = {
    {"K.DEC", F_TIME, 0, 127, 94, 0, 0},
    {"S.DEC", F_TIME, 0, 127, 74, 0, 0},
    {"M.DEC", F_TIME, 0, 127, 66, 0, 0},
    {"T.DEC", F_TIME, 0, 127, 84, 0, 0},       /* the toms (and the bass under the kit) */
};
static const param_desc_t PULSAR_SIZE_D = {"SIZE", F_BIPCT, -64, 63, 0, 0, 0};   /* the ENV page's SHP, renamed */

static const preset_t PULSAR_PRESETS[] = {
    /* name, {KICK, DROP, SNARE, SNAP, TONE, SHAOS, DRIVE, SUST}, {K.DEC S.DEC M.DEC B.DEC}, fenv, mono; SIZE: PULSAR_SIZE */
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
    {"HUGE", {34, 96, 44, 74, 30, 10, 84, 0}, {104, 84, 80, 96}, 0, 0},
    {"THUNDER", {18, 120, 30, 100, 40, 40, 100, 0}, {112, 96, 96, 108}, 0, 0},
    {"SOFT", {44, 40, 56, 40, 10, 0, 12, 0}, {84, 66, 56, 80}, 0, 0},
    {"MUFFLED", {38, 50, 40, 60, 20, 16, 30, 0}, {96, 76, 70, 90}, 0, 0},
};
#define ENG_PULSAR_NPRESETS NELEM(PULSAR_PRESETS)
static const int8_t PULSAR_SIZE[ENG_PULSAR_NPRESETS] = {10, 20, 30, 0, 40, 30, 16, 24, 20, -10, 44, 63, -36, -58};

/* a factory kit loaded (ui.c apply_preset_to): its SIZE, which lives outside the preset's EDIT values */
static void pulsar_preset_loaded(track_t *t, uint32_t pi)
{
    if (t->eng_req == ENGI_PULSAR && pi < ENG_PULSAR_NPRESETS)
        t->p[P_ED_SHP] = PULSAR_SIZE[pi];
}



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
    .knob = {P_E6, P_E3, P_E5, P_ED_SHP},     /* DRIVE, SNAP, SHAOS, SIZE */
    .poly = 8,
    .oneshot = 1,
    .keys = pulsar_keys,                      /* the keys play the kit (GM), as DRUM's */
    .note_on = pulsar_note_on,
    .render = pulsar_render,
    .amp = pulsar_amp,
    .post = pulsar_post,
};
