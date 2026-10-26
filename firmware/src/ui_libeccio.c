/* SPDX-License-Identifier: GPL-3.0-only */
/* fork: Libeccio's HOME (Stage's place, rows 32 .. 205; the header above, the band of notes under it stay Stage's).
 *
 *   the sound card   the sound you are editing (the selected track: the scratch), as large as it fits; under it its
 *                    voice mode and the keys' octave; at the right where it plays from: its MIDI channel, or EDIT
 *                    (the keys and every channel no slot has)
 *   the four knobs   always out, a ring of dots each (lit to the value, in the knob's colour), the value inside, the
 *                    label under it; the knob turning: its label lit
 *   the slots        the channels in use: the channel large, the engine under it; a slot flashes as MIDI plays it.
 *                    A free place says how to add one (SCL + a key)
 *
 * Each part keeps a signature of what it drew and redraws only when it changes (the ISR's slot_act blink included). */
#define LH_CARD_Y 32
#define LH_CARD_H 54
#define LH_KNOB_Y 90
#define LH_KNOB_H 72
#define LH_SLOT_Y 166
#define LH_SLOT_H 40
#define LH_RING_R 24                                    /* the knobs' rings: radius, dots */
#define LH_RING_N 15
#define LH_BLINK_MS 120u                                /* a slot lit this long after MIDI */

static struct {
    uint32_t card, knob[4], slot[3];
    uint32_t blink[NTRK];                               /* the last MIDI on a slot (fm1_ms | 1), 0 none */
} lh;

static void lh_reset(void) { memset(&lh, 0, sizeof lh); }
static uint8_t lh_stage;                                /* 1: Melodee's Stage instead (its tests) */

/* the slots shown: the tracks (not the selected) with a channel, by channel; n of them */
static uint32_t lh_slots(uint32_t *k)
{
    uint32_t n = 0, i, j;
    for (i = 0; i < NPART && i < trk_n(); i++)
        if (slot_ch[i] && i != song.sel)
            k[n++] = i;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && slot_ch[k[j]] < slot_ch[k[j - 1]]; j--) {
            uint32_t t = k[j];
            k[j] = k[j - 1];
            k[j - 1] = t;
        }
    return n;
}

/* ------------------------------------------------------ the sound card --- */
static void lh_card(void)
{
    uint32_t s = song.sel % NTRK, ch = s < NPART ? slot_ch[s] : 0u, pending = browse_pending(), src, kk;
    uint16_t tc = T_TRK(s), bg = ux_mix(T_BG, tc, 24);
    char nm[24], sub[32], pill[8], tag[6];
    int32_t oct = song.octave, pw, room;
    const aafont_t *nf;
    if (pending) {                                      /* browsing: the sound the list shows, not yet loaded */
        src = browse_shown(&kk);
        entry_label(src, kk, tag, nm);
        if (!nm[0]) str_cpy(nm, tag, sizeof nm);
    } else {
        sound_name(TSEL, nm);
        if (!nm[0]) str_cpy(nm, ENGINES[eng_idx(TSEL->eng_req)]->name, sizeof nm);
    }
    str_cpy(sub, N_VOICE[clamp(TSEL->p[P_VOICE], 0, 3)], sizeof sub);
    str_cpy(sub + str_len(sub), "  OCT ", 8);
    if (oct > 0) str_cpy(sub + str_len(sub), "+", 2);
    fmt_int(sub + str_len(sub), oct);
    if (ch) {
        str_cpy(pill, "CH ", sizeof pill);
        fmt_int(pill + 3, (int32_t)ch);
    } else {
        str_cpy(pill, "EDIT", sizeof pill);
    }
    {
        uint32_t sig = str_hash(str_hash(str_hash(s * 7u + pending * 3u + ux.gen * 977u + ux.pal * 31u + 1u, nm), sub), pill);
        if (!ui.force && sig == lh.card)
            return;
        lh.card = sig;
    }
    pw = text_w(TY_LB, pill) + 10;
    room = BOX_W - 16 - pw - 8;
    nf = ty_fit(TY_VL, nm, room);
    for (uint32_t band = 0; band < (uint32_t)LH_CARD_H; band += CV_MAX / BOX_W) {
        cv_begin_band(BOX_W, LH_CARD_H, T_BG, band);
        cv_rrect(0, 0, BOX_W, LH_CARD_H, 8, tc, T_BG);
        cv_rrect(2, 2, BOX_W - 4, LH_CARD_H - 4, 6, bg, tc);
        cv_free_text(10, ty_y(19, nf), nf, nm, pending ? T_ACCENT : T_TEXT, bg, room);
        cv_free_text(10, ty_y(41, TY_LB), TY_LB, sub, tc, bg, BOX_W - 20);
        if (ch) {                                       /* its channel: a pill in its colour */
            cv_rrect(BOX_W - 8 - pw, 11, pw, 18, 5, tc, bg);
            cv_text_c(BOX_W - 8 - pw / 2, ty_y(20, TY_LB), TY_LB, pill, T_INK, tc);
        } else {                                        /* the scratch: outlined */
            cv_rrect(BOX_W - 8 - pw, 11, pw, 18, 5, tc, bg);
            cv_rrect(BOX_W - 7 - pw, 12, pw - 2, 16, 4, bg, tc);
            cv_text_c(BOX_W - 8 - pw / 2, ty_y(20, TY_LB), TY_LB, pill, tc, bg);
        }
        cv_blit_band(BOX_X, LH_CARD_Y);
    }
}

/* ------------------------------------------------------ the four knobs --- */
static void lh_knob(uint32_t c)
{
    const page_t *sp = stage_page();
    int16_t *vp;
    const char *unit, *label = "";
    char val[12];
    const param_desc_t *d = page_desc(sp, c, &vp);
    int32_t w = 240 / 4, cx = w / 2, cy = 4 + LH_RING_R + 2, r1 = -1, i;
    int hot = stage_knobs_out() && (ui.hot_col & 3u) == c;
    uint16_t kc = T_KNOB(c), lc;
    val[0] = 0;
    unit = "";
    if (d && vp && d->label && d->label[0] != '-') {
        label = stage_label(c, d);
        stage_value(sp, d, *vp, val, &unit);
        r1 = d->fmt == F_ENUM && d->max < 2 ? -1 : RATIO(d, *vp);
    }
    {
        uint32_t sig = str_hash(str_hash(str_hash(c * 5u + (uint32_t)hot * 3u + ux.gen * 977u + ux.pal * 31u +
                                                  (uint32_t)(r1 + 2) * 7919u + ux.trk * 13u + 1u, label), val), unit);
        if (!ui.force && sig == lh.knob[c])
            return;
        lh.knob[c] = sig;
    }
    lc = hot ? T_ACCENT : T_MID;
    for (uint32_t band = 0; band < (uint32_t)LH_KNOB_H; band += CV_MAX / (uint32_t)w) {
        cv_begin_band((uint32_t)w, LH_KNOB_H, T_BG, band);
        if (label[0]) {
            /* the ring: LH_RING_N dots over 270 degrees from the bottom left, lit up to the value */
            /* the dots' places on the ring (Q10 unit circle, y down): 225 degrees clockwise to -45, 270 / 14 apart */
            static const int16_t RING[LH_RING_N][2] = {
                {-724, 724}, {-923, 444}, {-1018, 115}, {-998, -228}, {-867, -545}, {-638, -801}, {-338, -967},
                {0, -1024}, {338, -967}, {638, -801}, {867, -545}, {998, -228}, {1018, 115}, {923, 444}, {724, 724}};
            int32_t lit = r1 < 0 ? 0 : (r1 * (LH_RING_N - 1) + 500) / 1000 + 1;
            for (i = 0; i < LH_RING_N; i++)
                cv_disc(cx + RING[i][0] * LH_RING_R / 1024, cy + RING[i][1] * LH_RING_R / 1024, i < lit ? 6 : 4,
                        i < lit ? (hot ? T_ACCENT : kc) : T_LINE, 0);
            {   /* the value inside: the label face, its unit beside it when it fits */
                char vu[16];
                str_cpy(vu, val, sizeof vu);
                if (unit && unit[0] && text_w(TY_LB, vu) + text_w(TY_LB, unit) <= 2 * LH_RING_R - 8)
                    str_cpy(vu + str_len(vu), unit, sizeof vu - str_len(vu));
                cv_free_text(st_mid_x(cx, TY_LB, vu, 2 * LH_RING_R - 6), ty_y(cy, TY_LB), TY_LB, vu,
                             hot ? T_ACCENT : T_TEXT, T_BG, 2 * LH_RING_R - 6);
            }
            cv_free_text(st_mid_x(cx, TY_LB, label, w), ty_y(LH_KNOB_H - 8, TY_LB), TY_LB, label, lc, T_BG, w);
        }
        cv_blit_band((uint32_t)(c * (uint32_t)w), LH_KNOB_Y);
    }
}

/* ------------------------------------------------------ the slots --- */
static void lh_slot_cells(void)
{
    uint32_t k[NPART], n = lh_slots(k), c, i;
    int32_t gap = 4, w = (BOX_W - 2 * gap) / 3;
    uint8_t act[NTRK];
    fm1_irq_off();
    for (i = 0; i < NTRK; i++) {
        act[i] = slot_act[i];
        slot_act[i] = 0;
    }
    fm1_irq_on();
    for (i = 0; i < NTRK; i++)
        if (act[i])
            lh.blink[i] = fm1_ms | 1u;
        else if (lh.blink[i] && fm1_ms - (lh.blink[i] & ~1u) >= LH_BLINK_MS)
            lh.blink[i] = 0;
    for (c = 0; c < 3u; c++) {
        uint32_t t = c < n ? k[c] : NTRK, on = t < NTRK && lh.blink[t];
        uint16_t tc = t < NTRK ? T_TRK(t) : T_DIM, bg = t < NTRK ? ux_mix(T_BG, tc, on ? 70 : 16) : T_BG;
        char num[4], name[16];
        uint32_t sig;
        num[0] = name[0] = 0;
        if (t < NTRK) {
            fmt_int(num, (int32_t)slot_ch[t]);
            str_cpy(name, ENGINES[eng_idx(trk[t].eng_req)]->name, sizeof name);
        }
        sig = str_hash(str_hash(t * 7u + on * 3u + c * 11u + ux.gen * 977u + ux.pal * 31u + (c == n) * 5u + 1u, num), name);
        if (!ui.force && sig == lh.slot[c])
            continue;
        lh.slot[c] = sig;
        for (uint32_t band = 0; band < (uint32_t)LH_SLOT_H; band += CV_MAX / (uint32_t)w) {
            cv_begin_band((uint32_t)w, LH_SLOT_H, T_BG, band);
            if (t < NTRK) {                             /* the channel, the engine under it */
                cv_rrect(0, 0, w, LH_SLOT_H, 7, bg, T_BG);
                cv_text(8, ty_y(12, TY_TX), TY_TX, num, on ? T_INK : tc);
                cv_free_text(8, ty_y(30, TY_LB), TY_LB, name, on ? T_INK : T_TEXT, bg, w - 12);
            } else {
                cv_dashed(0, 0, w, LH_SLOT_H, 7, T_LINE);
                if (c == n) {                           /* the first free place: how to fill it */
                    cv_text_c(w / 2, ty_y(12, TY_LB), TY_LB, "SCL", T_DIM, T_BG);
                    cv_text_c(w / 2, ty_y(29, TY_LB), TY_LB, "+ KEY", T_DIM, T_BG);
                }
            }
            cv_blit_band((uint32_t)(BOX_X + (int32_t)c * (w + gap)), LH_SLOT_Y);
        }
    }
}

/* every frame the knobs and the slots' blink; every other frame (the scope's) the card, then the band */
static void lh_draw(void)
{
    uint32_t c;
    stage_panel_hits();
    if (ui.force)
        lh_reset();
    for (c = 0; c < 4u; c++)
        lh_knob(c);
    lh_slot_cells();
    if (!ui.force && (ui.frame & 1u))
        return;
    lh_card();
    stage_band();
}
