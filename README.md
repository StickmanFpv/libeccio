# Libeccio

Custom firmware for the M-VAVE FM-1, built to play along with a Teenage Engineering K.O. II and a KeyStep Pro: a few
curated synth engines with Minimoog-style knobs, and one MIDI channel per sound.

Libeccio is a fork of [Melodee](https://github.com/keremimo/melodee) by Kerem Kilic (Ellic Studio), itself based on
[Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita / Hügelton Instruments. It is not affiliated with or
endorsed by them, by M-VAVE, Teenage Engineering or SOMA Laboratory. Everything Melodee does is still here; its
README is [docs/MELODEE_README.md](docs/MELODEE_README.md).

**Status: early and untested on hardware.** The sound engines and the UI are tested on a computer with Melodee's own
test suite; CPU load on the FM-1 is not measured yet. Keep a backup and the official V15 firmware before installing.

## What Libeccio adds

| Engine | Sound | From |
|---|---|---|
| **MOOG** | Minimoog-style: the transistor-ladder filter and its contour (FILTER: CUT EMPH CNTR DRIVE; CONTOUR: F.ATK F.DEC F.SUS KBD), and the oscillators on four knobs (OSC: WAVE, OSCS 1..3, DTUNE, OCTS: unison, sub, octaves, fifths, bass 16'/8', one-finger chords) | DSP from [FMini-1](https://github.com/ruppert/fmini-1) |
| **SEM** | Oberheim SEM-style: two oscillators into a state-variable filter whose STATE morphs LP → notch → HP | [Caravel](https://github.com/bonszaimusic/caravel) |
| **HADES** | Dark basses and subs: saw / pulse with two divider sub-octaves, a three-pole filter, BODY and DRIVE | Caravel |
| **VOICE** | Sung vowels, GR!MM's VOICE: VOWEL, SHIFT, BUZZ, BREATH on the main knobs, the vowel's glide (VOWL2, TALK), Q and RAND; GR!MM's choirs, monks and whispers with their slow movement | [GR!MM](https://github.com/GRiMMBIENT/grimm) (Felucca's formant oscillator) |
| **PULSAR** | Big driven drums after SOMA's Pulsar-23, a full kit on the keys (GM map, the first C is the kick): kick and deep kick, snare, second snare, rimshot, clap, closed / pedal / open hats, cymbals, six toms (and congas), cowbell and bells; DROP, a tunable snare, TONE (808 squares to ring-mod to lo-fi digital noise), per-voice grit, a glued bus, SUST for drones, SHAOS for per-hit chaos, SIZE from muffled to thunder; the four decays on the ENV page (kick, snare, metal, toms) | new (original DSP) |

They come first in the sound browser; Melodee's PROPHET, FM6, CZ-1, SID and DRUM follow. The four tracks start as MOOG,
SEM, HADES and PULSAR.

**Home screen.** The sound you are editing on top (its voice mode, the keys' octave, EDIT or its MIDI channel), the
four knobs always shown as rings of dots, and under them the MIDI channels in use, each lit as the KO II plays it.
Melodee's track rows give way to it (the sequencer pages are all still there).

**MASSIVE reverb.** A huge modulated reverb in the shared FX rack (FX > SHARED: pick Massive in a slot, then raise
each track's SEND). MIX, SIZE (about a second to about a minute, nearly endless at the top), MODE and TONE:
HALL, NAVE (a cathedral: stone, a long predelay, dark), SPACE (huge, wide), ECHO (sparse repeats smearing into a
cloud), CLOUD (a slow, endless bloom) and DEEP (dark and wobbling, underwater). Eight modulated delay lines at half
rate, 30 KB while it is on; one at a time.

**Knobs.** On the main page MOOG, SEM and HADES put cutoff, resonance, envelope amount and release on the four knobs
(VOICE: VOWEL, SHIFT, BUZZ, BREATH; PULSAR: DRIVE, SNAP, SHAOS and SIZE: muffled to thunder). The filter knobs turn like a Minimoog's dial: a slow turn is one step a detent, a
quick turn speeds up from its second detent (up to x12), so one quick turn sweeps the whole range, and the engines
glide between the steps.

**MIDI slots.** The selected track is the sound you are working on. Hold **SCL** and:

- tap a white key (1..16): this sound goes to that MIDI channel, at once (EDIT held undoes)
- hold a white key 0.5 s: that channel's sound comes back to work on it
- hold a white key 2 s: the channel is cleared

While SCL is held, the keys of the channels in use are lit and blink as MIDI plays them. Up to three channels (the
sound you are editing is the fourth track); channels without a slot play the sound you are editing. Projects keep
the channels. ROOT, SCALE and the chord keys stay on SCL's knobs.

## Install

In Chrome or Edge: **https://stickmanfpv.github.io/libeccio/**, the Melodee web installer carrying the latest build of
`main`. Melodee's editor and manual work with Libeccio too.

## Build

Every push to `main` builds the package and publishes it to the web installer. A push of a version tag (`v0.1.0`) builds, attests and releases the package with the shared FM-1 builder
([zvenson/fm1-build](https://github.com/zvenson/fm1-build)): download `libeccio-<version>.fwsc` from the release.
Locally: [BUILDING.md](BUILDING.md) (the JieLi toolchain and SDK). The host tests run without them:
`tests/run_tests.sh` after generating the headers.

## Licence

GPL-3.0-only, as Melodee and Felucca. Bundled fonts and ported DSP keep their own licences ([LICENSES/](LICENSES),
[LICENSING.md](LICENSING.md)). Credits: Melodee (Kerem Kilic), Felucca (Leo Kuroshita, Hügelton Instruments),
Caravel (bonszaimusic: SEM, HADES), FMini-1 (Guilherme Ruppert: the ladder filter and contour), GR!MM (VOICE), and everyone credited
in [docs/MELODEE_README.md](docs/MELODEE_README.md).
