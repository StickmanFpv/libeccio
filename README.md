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
| **MOOG** | Minimoog-style: three oscillators (15 curated setups on one OSC knob), the transistor-ladder filter with its own contour, mixer drive | DSP from [FMini-1](https://github.com/ruppert/fmini-1) |
| **SEM** | Oberheim SEM-style: two oscillators into a state-variable filter whose STATE morphs LP → notch → HP | [Caravel](https://github.com/bonszaimusic/caravel) |
| **HADES** | Dark basses and subs: saw / pulse with two divider sub-octaves, a three-pole filter, BODY and DRIVE | Caravel |
| **PULSAR** | Big driven drums after SOMA's Pulsar-23, on the GM drum map: a kick with a tune envelope (DROP), a tunable snare of three membrane partials (plus rimshot, clap and a second snare), metal from 808 squares through ring-modulation to lo-fi digital noise (TONE), a pitched bass for toms, bass lines and resonant leads; per-voice grit (drive, fold, asymmetry), a glued bus, SUST for drones, SHAOS for per-hit chaos; the four decays on the ENV page; the LFO and the mod matrix bend it | new (original DSP) |

They come first in the sound browser; Melodee's PROPHET, FM6, CZ-1, SID and DRUM follow.

**Home screen.** The sound you are editing on top (its voice mode, the keys' octave, EDIT or its MIDI channel), the
four knobs always shown as rings of dots, and under them the MIDI channels in use, each lit as the KO II plays it.
Melodee's track rows give way to it (the sequencer pages are all still there).

**Knobs.** On the main page MOOG, SEM and HADES put cutoff, resonance, envelope amount and release on the four knobs
(PULSAR: DRIVE, SNAP, SHAOS and SIZE: muffled to thunder). The filter knobs turn like a Minimoog's dial: a slow turn is one step a detent, a
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
Caravel (bonszaimusic: SEM, HADES), FMini-1 (Guilherme Ruppert: the ladder filter and contour), and everyone credited
in [docs/MELODEE_README.md](docs/MELODEE_README.md).
