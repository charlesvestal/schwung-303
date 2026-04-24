# 303 — TB-303 Emulator for Schwung

A monophonic TB-303 emulator sound generator for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. Open303 DSP core, Devilfish-inspired modifications, and a lightweight two-model drive stage (Soft tanh + ProCo RAT port).

No built-in sequencer — drive it with any MIDI source. Pairs naturally with [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po) for generative acid basslines.

## Install

From the Schwung Module Store, or manually:

```bash
git clone https://github.com/charlesvestal/schwung-303
cd schwung-303
./scripts/build.sh && ./scripts/install.sh
```

## MIDI

**Notes** — standard note-on / note-off drive pitch and gate.

**Accent** — velocity ≥ 100 triggers Open303's accent envelope. Matches TB-3PO's emission (velocity 118 = accent, 72 = normal).

**Slide** — a new note-on arriving before the previous note-off (legato overlap) is detected as slide. Open303 handles this natively; no CC 65 / portamento toggle needed. Also matches TB-3PO.

**CC map** — honors GM2/MPE conventions where they exist:

| CC | Parameter |
|---:|-----------|
| 74 | Cutoff |
| 71 | Resonance |
| 75 | Decay |
| 70 | Env Mod |
| 16 | Accent |
| 7  | Volume |
| 12 | Drive |
| 13 | Mix |
| 123 | All Notes Off |

## Drive

Two selectable saturation models under the Drive submenu:

- **Soft** — tilt-EQ + 2x oversampled asymmetric tanh. Generic warm softclip, neutral flavor. Good for gentle thickening.
- **RAT** — ported from [davemollen/dm-Rat](https://github.com/davemollen/dm-Rat) (GPL-3.0). Distortion-modulated 3rd-order op-amp IIR → algebraic waveshaper `x/(1+x⁴)^(1/4)` around 2x oversampling → fixed-mid tone stack. Aggressive, gritty, distinctively ProCo RAT.

`Drive = 0` fully bypasses the stage (zero CPU cost). `Mix` blends with the clean synth signal.

## Devilfish Mods

Six Devilfish-inspired parameters, implemented inside the Open303 engine by jc303:

- **Normal Decay** and **Accent Decay** split the single stock decay into two (30 ms – 3 s)
- **Feedback HPF** cuts low end from the filter's self-feedback
- **Soft Attack** slows the envelope attack for non-accented notes
- **Slide Time** extends the slide range
- **Shaper Drive** controls a pre-filter tanh waveshaper inside the square-wave wavetable (distinct from the post-synth Drive stage above)

Toggle off to restore stock 303 values.

## Credits

This module is a port, not original work. All DSP lineage acknowledged:

- **Open303** engine — Robin Schmidt ([RobinSchmidt/Open303](https://github.com/RobinSchmidt/Open303)), MIT license. Original license preserved at `src/dsp/open303/LICENSE`.
- **JC-303** — midilab ([midilab/jc303](https://github.com/midilab/jc303)), GPL-3.0, source of the Devilfish extensions to the Open303 engine.
- **dm-Rat** — Dave Mollen ([davemollen/dm-Rat](https://github.com/davemollen/dm-Rat)), GPL-3.0. The RAT drive model (`src/dsp/drive.h`, `namespace drive::rat`) is a direct port of the Rust DSP in `rat/src/{clipper,op_amp,tone}.rs` — op-amp s-domain coefficients, bilinear transform, 3rd-order IIR, algebraic clipper, and tone stack. Oversampling reduced from 8x FIR to 2x biquad to fit the embedded CPU budget.

Schwung port and glue code: Charles Vestal.

## License

**GPL-3.0** — inherited from jc303 (Devilfish extensions) and dm-Rat (RAT drive model). The Open303 engine core itself remains MIT (Robin Schmidt's original licensing is preserved in `src/dsp/open303/LICENSE`).

See [LICENSE](LICENSE) for the full GPL-3.0 text.
