/* SPDX-License-Identifier: GPL-3.0-only */
/* fork: SLOTS, one MIDI channel per sound, for playing along with a K.O. II (or any sequencer that sends each part
 * on its own channel).
 *
 * The selected track is the scratch: the sound on the knobs, played from the keys (and from any MIDI channel no slot
 * has). The other synth tracks are slots, each listening on one channel (seq.c midi_track; slot_ch in core.h).
 * Hold SCL (the layer, ui_layer.c) and:
 *   tap a white key 1..16      the scratch's sound goes to that channel: its slot's sound is replaced at once, or a
 *                              free track becomes its slot (a new track when needed). EDIT held undoes, as any load
 *   hold a white key 0.5 s     that channel's sound comes back into the scratch, to work on it (then tap to put it back)
 *   hold a white key 2 s       the channel is cleared (its track stays, silent to MIDI)
 * While SCL is held the keys of the channels in use are lit and blink as MIDI plays them.
 * The scratch's voice mode, octave (the keys' OCT, folded into the slot's TRN) and everything that makes the sound
 * travel with it; the mix, the arpeggiator, the scale and the patterns stay the slot's own.
 * Up to NPART - 1 slots: the scratch is the fourth track. Projects keep the channels (project.c, P_SLOTCH). */
#define SLOT_HOLD_MS 500u
#define SLOT_CLEAR_MS 2000u
static struct { uint32_t t0; uint8_t key, ch, stage; } slk;   /* the white key held in the layer: key + 1, its
                                                                * channel, stage 0 down, 1 pulled, 2 cleared */

static uint32_t slot_of_ch(uint32_t ch)
{
    uint32_t k;
    for (k = 0; k < NPART; k++)
        if (slot_ch[k] == ch && k < trk_n())
            return k;
    return NTRK;
}

static void slot_say(uint32_t ch, const char *what)
{
    char b[20] = "CH ";
    fmt_int(b + 3, (int32_t)ch);
    str_cpy(b + str_len(b), ": ", sizeof b - str_len(b));
    ui_say(b, what);
}

/* dst's sound = src's (an undo level on dst; the engine's own patches too); trn: dst's TRN */
static void slot_copy_sound(track_t *dst, const track_t *src, int32_t trn)
{
    uint32_t i, d = trk_index(dst), s = trk_index(src);
    load_begin(dst, UNDO_SOUND);
    engine_load_of(dst, src->eng_req, src->preset);     /* the engine, its state, its own patch hooks */
    fm1_irq_off();
    for (i = 0; i < P_COUNT; i++)
        if (!param_kept(i) && !param_reserved(i))
            dst->p[i] = src->p[i];
    dst->p[P_TRANS] = (int16_t)clamp(trn, TP[P_TRANS].min, TP[P_TRANS].max);
    dst->preset = src->preset;
    dst->user = src->user;
    dst->user_native = src->user_native;
    if (d < NPART && s < NPART) {
        p5_patch[d] = p5_patch[s];
        *cz_patch_of(d) = *cz_patch_of(s);
        *drum_patch_of(d) = *drum_patch_of(s);
    }
    fm6_put_patch(d, fm6_patch_of(s), 0);
    memcpy(fm6_fn[d % NTRK], fm6_fn[s % NTRK], FM6_NFN);
    fm6_on[d % NTRK] = fm6_on[s % NTRK];
    panic_req |= (uint8_t)(1u << d);                   /* the voice mode may change: release what sounds */
    fm1_irq_on();
    load_end(dst);
}

static void slot_assign(uint32_t ch)
{
    uint32_t s = song.sel, k = slot_of_ch(ch);
    if (s >= NPART || midi_track_only(TSEL)) {
        ui_message("SLOTS: PICK A SYNTH TRACK");
        return;
    }
    if (k == s) {                                        /* the scratch had this channel: it moves to a slot */
        slot_ch[s] = 0;
        k = NTRK;
    }
    if (k >= NPART)
        for (k = 0; k < NPART; k++)
            if (k != s && (k >= trk_n() || !slot_ch[k]))
                break;
    if (k >= NPART) {
        ui_message("SLOTS FULL: TAP A LIT KEY");
        return;
    }
    browse_commit();
    momentary_restore();
    while (trk_n() <= k) {                               /* tracks are contiguous: create the places through k */
        uint32_t next = trk_n();
        track_blank(next);
        song.ntrk = (uint8_t)(next + 1u);
    }
    slot_copy_sound(&trk[k], TSEL, TSEL->p[P_TRANS] + 12 * song.octave);
    slot_ch[k] = (uint8_t)ch;
    slot_say(ch, ENGINES[trk[k].eng_req % NENGINES]->name);
}

static void slot_pull(uint32_t ch)
{
    uint32_t k = slot_of_ch(ch);
    if (k >= NPART) {
        slot_say(ch, "EMPTY");
        return;
    }
    if (k == song.sel)
        return;
    browse_commit();
    momentary_restore();
    slot_copy_sound(TSEL, &trk[k], trk[k].p[P_TRANS] - 12 * song.octave);
    slot_say(ch, "TO SCRATCH");
}

static void slot_clear(uint32_t ch)
{
    uint32_t k = slot_of_ch(ch);
    if (k < NPART) {
        slot_ch[k] = 0;
        panic_req |= (uint8_t)(1u << k);
    }
    slot_say(ch, "CLEARED");
}

/* SCL's layer: white key place p (0..15: channels 1..16) went down as key k */
static void slot_key_down(uint32_t k, uint32_t p)
{
    if (p >= 16u)
        return;
    slk.key = (uint8_t)(k + 1u);
    slk.ch = (uint8_t)(p + 1u);
    slk.stage = 0;
    slk.t0 = fm1_ms;
}

/* every pass: the held key's time (pull, clear), its release (assign). held: the keys still the layer's */
static void slot_tick(uint32_t held)
{
    uint32_t dt;
    if (!slk.key)
        return;
    dt = fm1_ms - slk.t0;
    if ((held >> (slk.key - 1u)) & 1u) {
        if (slk.stage == 0u && dt >= SLOT_HOLD_MS) {
            slot_pull(slk.ch);
            slk.stage = 1;
        } else if (slk.stage == 1u && dt >= SLOT_CLEAR_MS) {
            slot_clear(slk.ch);
            slk.stage = 2;
        }
        return;
    }
    if (slk.stage == 0u)
        slot_assign(slk.ch);
    slk.key = 0;
}

/* the LEDs while SCL is held: the channels in use lit (as white keys 1..16), dark while MIDI plays them (a blink) */
static uint32_t slot_leds(void)
{
    uint32_t k, m = 0, p;
    for (k = 0; k < 27u; k++) {
        if (key_black(k))
            continue;
        p = key_place(k);
        if (p < 16u) {
            uint32_t s = slot_of_ch(p + 1u);
            if (s < NPART && !(slot_act[s] && ((fm1_ms / 60u) & 1u)))
                m |= 1u << k;
        }
    }
    for (k = 0; k < NPART; k++)
        if (slot_act[k])
            slot_act[k]--;
    return m;
}
