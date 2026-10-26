/* SPDX-License-Identifier: GPL-3.0-only */
/* fork: MASSIVE, a huge reverb for the shared FX rack (original DSP). After the big modulated feedback-delay-network
 * reverbs (Jot's FDN, Dattorro's input diffusion, the long lush "space" reverbs of today's plug-ins and of the NTS-1),
 * with a cathedral after GHOULBOX's in mind.
 *   the input   the send's mono sum through the slot's TONE (its low and high cut), half rate (22.05 kHz: twice the
 *               tail for the memory, and the highs a big space loses anyway), a predelay, four allpasses in series
 *               (the diffusion: a smeared, dense onset; few for ECHO, many for CLOUD)
 *   the tank    eight delay lines (39 .. 97 ms) mixed by a Householder matrix (lossless, every line into every line),
 *               each damped by a one-pole low-pass; four of them read through slowly moving, interpolated taps (the
 *               chorus that keeps a long tail from ringing metallic); a gain per line from the decay time, so every
 *               line dies at the same rate
 *   the output  two decorrelated sums of the lines (left, right), back to full rate
 * Its knobs: MIX, SIZE (the decay: about a second to about a minute, nearly endless at the top; the size of the space
 * with it in ECHOES and SPACE) and MODE (CHAR's place):
 *   HALL        a concert hall, 1 .. 9 s
 *   NAVE        a cathedral's: stone, a long predelay, dense and dark, 3 .. 25 s
 *   SPACE       huge and bright-ish, wide moving taps, 4 .. 40 s
 *   ECHO        little diffusion and a long predelay: the lines' repeats heard as echoes that smear into a cloud
 *   CLOUD       a nebula: a slow bloom (a long, dense diffusion), deep movement, dark: an endless pad under anything
 *   DEEP        very dark and wobbling: underwater
 *   RISE        a shimmer, after the NTS-1's RISER: the tail fed back an octave up, so it climbs as it rings
 *   DIVE        the same an octave down: the tail sinks (the NTS-1's SUBMARINE)
 * The shimmer: the tank's sum into a two-tap pitch shifter (a 43 ms window, the taps half a window apart, each faded in
 * and out by a triangle, so the sum stays level), an octave up or down, back into the tank's input.
 * TONE (the slot's third knob) cuts its lows or highs at the input. One MASSIVE at a time (as REVERB): 32 KiB from the
 * audio arena while it is on. */
#define MV_N 8u
#define MV_MOD 24u                                       /* the moving taps' reach (samples at half rate) */
#define MV_PRE 2048u                                     /* predelay: up to 92 ms */
#define MV_SH 1024u                                      /* the shimmer's buffer, its window MV_SHW (Q16 phase) */
#define MV_SHW 960
static const uint16_t MV_LEN[MV_N] __attribute__((section(".data.fx_sloop"))) = {863, 1031, 1201, 1361, 1559, 1723, 1931, 2129};
static const uint16_t MV_AP[4] __attribute__((section(".data.fx_sloop"))) = {142, 107, 379, 277};
#define MV_LINES (863u + 1031u + 1201u + 1361u + 1559u + 1723u + 1931u + 2129u + MV_N * (MV_MOD + 2u))
#define MV_APS (142u + 107u + 379u + 277u)
typedef struct {
    int16_t line[MV_LINES], ap[MV_APS], pre[MV_PRE], sh[MV_SH];
    uint16_t pos[MV_N], ap_pos[4], pre_pos, sh_pos;
    uint32_t sh_ph;                                      /* the shimmer's window phase (Q16 of one window) */
    int32_t low[MV_N], gain[MV_N], in_prev, out_l, out_r;   /* out: the last tank step's (full-rate scale) */
    uint32_t phase[2];
    uint8_t half, mode, size, ok;
} fx_massive_t;
_Static_assert(sizeof(fx_massive_t) < 32u * 1024u, "MASSIVE fits 32 KiB");

/* a mode: predelay (samples), diffusion (allpass gain Q15), damping (the low-pass coefficient Q15: high = bright),
 * the moving taps' depth (samples Q8) and rate (LFO_INC index), the decay at SIZE 0 and 127 (tenths of a second), the
 * shimmer (+1 an octave up, -1 down, 0 none) and how much of it goes back in (Q15) */
typedef struct { uint16_t pre, diff, damp, depth; uint8_t rate; uint16_t t0, t1; int8_t shift; uint16_t shim; } mv_mode_t;
static const mv_mode_t MV_MODE[] __attribute__((section(".data.fx_sloop"))) = {
    {330, 20000, 20000, 6 << 8, 24, 10, 90},             /* HALL */
    {1100, 23000, 13000, 4 << 8, 18, 30, 250},           /* NAVE */
    {550, 24000, 24000, 14 << 8, 30, 40, 400},           /* SPACE */
    {2040, 5000, 18000, 3 << 8, 20, 20, 200},            /* ECHO */
    {0, 27500, 15000, 22 << 8, 14, 60, 900},             /* CLOUD */
    {220, 21000, 8000, 18 << 8, 34, 30, 200},            /* DEEP */
    {330, 24000, 21000, 8 << 8, 24, 30, 300, 1, 11000},  /* RISE */
    {220, 22000, 12000, 6 << 8, 20, 30, 250, -1, 12000}, /* DIVE */
};
_Static_assert(NELEM(MV_MODE) == MV_NMODE, "params.c N_MASSIVE: a name a mode");
static const uint16_t MV_BASE[MV_N] __attribute__((section(".data.fx_sloop"))) = {0, 889, 1946, 3173, 4560, 6145, 7894, 9851};
_Static_assert(MV_LINES == 9851u + 2129u + MV_MOD + 2u, "MV_BASE: each line MV_MOD + 2 longer");

/* the lines' gains for the mode and SIZE: each line loses 60 dB over the decay time, in proportion to its length */
static void mv_design(fx_massive_t *s, uint32_t mode, uint32_t size)
{
    const mv_mode_t *m = &MV_MODE[mode];
    float t = (float)m->t0 + (float)(m->t1 - m->t0) * (float)(size * size) / (127.0f * 127.0f);   /* tenths of s */
    uint32_t k;
    for (k = 0; k < MV_N; k++) {
        float g = fm_exp2f(-9.966f * (float)MV_LEN[k] * 10.0f / (t * (FS / 2.0f)));   /* 10^(-3 len / (T fs)) */
        s->gain[k] = (int32_t)(g * 32767.0f);
    }
    s->mode = (uint8_t)mode;
    s->size = (uint8_t)size;
}

static __attribute__((noinline,section(".audio_text"))) void fx_massive_run(fx_state_t *restrict f, fx_massive_t *restrict s,
                                                                            uint32_t n, int32_t target, int32_t *restrict l,
                                                                            int32_t *restrict r)
{
    uint32_t mode = (uint32_t)clamp(f->character, 0, MV_NMODE - 1), size = (uint32_t)clamp(f->rate, 0, 127), i, k;
    const mv_mode_t *m;
    int32_t mix = f->mix, diff, damp, depth, ma[2], mb[2], dm[2], md[2];
    if (!s->ok || s->mode != mode) {                    /* a new space: an empty tank */
        memset(s, 0, sizeof *s);
        s->ok = 1;
        mv_design(s, mode, size);
    } else if (s->size != size) {
        mv_design(s, mode, size);
    }
    if (!n)
        return;
    m = &MV_MODE[mode];
    diff = m->diff;
    damp = m->damp;
    depth = m->depth;
    for (k = 0; k < 2u; k++) {                          /* the moving taps: designed twice a block, a ramp between */
        ma[k] = (int32_t)(MV_MOD << 7) + (int32_t)(((int64_t)sine_i(s->phase[k]) * depth) >> 16);
        s->phase[k] += LFO_INC[m->rate + k * 3u] * n / CTL;
        mb[k] = (int32_t)(MV_MOD << 7) + (int32_t)(((int64_t)sine_i(s->phase[k]) * depth) >> 16);
        dm[k] = (mb[k] - ma[k]) * 2 / (int32_t)n;       /* per half-rate sample, Q8 */
        md[k] = ma[k];
    }
    for (i = 0; i < n; i++) {
        int32_t dry_l = l[i], dry_r = r[i], wl, wr;
        int32_t mono = fx_div_pow2(clamp(dry_l, -1048575, 1048575) + clamp(dry_r, -1048575, 1048575), 1);
        int32_t x = fx_tone_input(f, mono, 1.0f / (0.010f * FS));
        if (s->half ^= 1u) {                            /* the first of a pair: keep it; out: the newest tank step */
            s->in_prev = x;
            wl = s->out_l;
            wr = s->out_r;
        } else {                                        /* the second: one tank step; out: halfway to it */
            int32_t a = fx_div_pow2(clamp((s->in_prev + x) >> 1, -262143, 262143), 3), o[MV_N], sum = 0, ol, orr;
            uint32_t off = 0, pr;
            s->pre[s->pre_pos] = (int16_t)clamp(a, -32768, 32767);
            pr = s->pre_pos + MV_PRE - m->pre;
            if (pr >= MV_PRE) pr -= MV_PRE;
            a = m->pre ? s->pre[pr] : a;
            if (++s->pre_pos == MV_PRE) s->pre_pos = 0;
            if (m->shift) {                             /* the shimmer: the last step's tank, shifted, back in */
                uint32_t ph = s->sh_ph, j;
                int32_t y = 0;
                for (j = 0; j < 2u; j++, ph += 32768u) {   /* two taps half a window apart */
                    uint32_t fr = ph & 65535u, w = fr < 32768u ? fr * 2u : (65535u - fr) * 2u;   /* triangle Q16 */
                    uint32_t dq = m->shift > 0 ? (uint32_t)MV_SHW * (65535u - fr) : (uint32_t)MV_SHW * fr;   /* Q16 */
                    uint32_t back = (dq >> 16) + 2u, fq = (dq >> 8) & 255u;
                    uint32_t i0 = (s->sh_pos + MV_SH - back) % MV_SH, i1 = (i0 + MV_SH - 1u) % MV_SH;
                    int32_t v = s->sh[i0] + (((s->sh[i1] - s->sh[i0]) * (int32_t)fq) >> 8);
                    y += (int32_t)(((int64_t)v * w) >> 16);
                }
                s->sh_ph += m->shift > 0 ? 65536u / MV_SHW : 32768u / MV_SHW;   /* up: delay falls 1 a sample; down: rises 1/2 */
                a += (y * m->shim) >> 15;
            }
            for (k = 0; k < 4u; k++) {                  /* the diffusion: four allpasses */
                int32_t b = s->ap[off + s->ap_pos[k]], v = a + ((b * diff) >> 15);
                v = clamp(v, -32768, 32767);
                s->ap[off + s->ap_pos[k]] = (int16_t)v;
                a = b - ((v * diff) >> 15);
                if (++s->ap_pos[k] == MV_AP[k]) s->ap_pos[k] = 0;
                off += MV_AP[k];
            }
            for (k = 0; k < MV_N; k++) {                /* the lines' outputs: lines 0 2 5 7 through the moving taps */
                uint32_t len = MV_LEN[k] + MV_MOD + 2u, base = MV_BASE[k], rd;
                int32_t o_k;
                if (k == 0u || k == 2u || k == 5u || k == 7u) {
                    int32_t d = md[k & 1u];             /* Q8 samples back from the full length */
                    uint32_t back = (uint32_t)(d >> 8), fr = (uint32_t)d & 255u, rd2;
                    rd = s->pos[k] + 1u + back;         /* the oldest is at pos + 1 */
                    while (rd >= len) rd -= len;
                    rd2 = rd + 1u == len ? 0u : rd + 1u;
                    o_k = s->line[base + rd] + (((s->line[base + rd2] - s->line[base + rd]) * (int32_t)fr) >> 8);
                } else {
                    rd = s->pos[k] + 1u + MV_MOD;
                    while (rd >= len) rd -= len;
                    o_k = s->line[base + rd];
                }
                s->low[k] += ((o_k - s->low[k]) * damp) >> 15;   /* the damping */
                o[k] = s->low[k];
                sum += o[k];
            }
            sum >>= 2;                                  /* Householder: x - 2/N sum */
            for (k = 0; k < MV_N; k++) {
                int32_t fb = ((o[k] - sum) * s->gain[k]) >> 15;
                int32_t in = (k & 1u) ? -a : a;
                uint32_t base = MV_BASE[k], len = MV_LEN[k] + MV_MOD + 2u;
                s->line[base + s->pos[k]] = (int16_t)clamp(fb + (in >> 1), -32768, 32767);
                if (++s->pos[k] == len) s->pos[k] = 0;
            }
            if (m->shift) {                             /* the tank's sum into the shimmer's buffer */
                s->sh[s->sh_pos] = (int16_t)clamp((o[0] + o[1] + o[2] + o[3]) >> 1, -32768, 32767);
                if (++s->sh_pos == MV_SH) s->sh_pos = 0;
            }
            md[0] += dm[0];
            md[1] += dm[1];
            ol = (o[0] - o[2] + o[4] - o[6] + (o[1] >> 1)) * 6;
            orr = (o[1] - o[3] + o[5] - o[7] + (o[6] >> 1)) * 6;
            wl = (s->out_l + ol) >> 1;                  /* back to full rate: halfway, then there (linear) */
            wr = (s->out_r + orr) >> 1;
            s->out_l = ol;
            s->out_r = orr;
        }
        if (mix != target) mix += clamp(target - mix, -128, 128);
        l[i] = dry_l + fx_scale(clamp(wl, -262143, 262143) - dry_l, (uint32_t)mix * 2u);
        r[i] = dry_r + fx_scale(clamp(wr, -262143, 262143) - dry_r, (uint32_t)mix * 2u);
    }
    f->mix = mix;
}
