# 303 — TB-303 Emulator for Schwung

A monophonic TB-303 emulator sound generator for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. Open303 DSP core, Devilfish-inspired modifications, and neural-network overdrive with 197 amp/pedal models.

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
| 12 | Overdrive Level |
| 13 | Overdrive Dry/Wet |
| 123 | All Notes Off |

## Overdrive

197 neural-network amp/pedal models (37 from jc303's JC303 pack + 160 from the GuitarML Proteus Tone Packs) are bundled and selectable from the Shadow UI. Default model is `jc303/TS9_DriveKnob`. Inference is done with [RTNeural](https://github.com/jatinchowdhury18/RTNeural) (Eigen backend) — no framework dependencies.

Turn overdrive off to bypass the RNN entirely (zero CPU cost).

## Devilfish Mods

Six Devilfish-inspired parameters, implemented inside the Open303 engine by jc303:

- **Normal Decay** and **Accent Decay** split the single stock decay into two (30 ms – 3 s)
- **Feedback HPF** cuts low end from the filter's self-feedback
- **Soft Attack** slows the envelope attack for non-accented notes
- **Slide Time** extends the slide range
- **Shaper Drive** controls a pre-filter tanh waveshaper

Toggle off to restore stock 303 values.

## Credits

This module is a port, not original work. All DSP lineage acknowledged:

- **Open303** engine — Robin Schmidt ([RobinSchmidt/Open303](https://github.com/RobinSchmidt/Open303)), MIT license
- **JC-303** — midilab ([midilab/jc303](https://github.com/midilab/jc303)), GPL-3.0, source of the Devilfish extensions and the integrated GuitarML overdrive path
- **GuitarML BYOD** — [GuitarML](https://github.com/GuitarML) / Jatin Chowdhury, MIT license, neural amp modelling framework
- **RTNeural** — [jatinchowdhury18/RTNeural](https://github.com/jatinchowdhury18/RTNeural), BSD-3-Clause, real-time neural network inference
- **Proteus Tone Packs** — [GuitarML/Proteus_Tone_Packs](https://github.com/GuitarML/Proteus_Tone_Packs), bundled pre-trained amp/pedal models
- **Eigen** — linear algebra dependency of RTNeural, MPL2
- **nlohmann/json** — JSON parsing, MIT license

Schwung port and glue code: Charles Vestal.

## License

**GPL-3.0** — inherited from jc303, which is the direct upstream for the DSP graph (Open303 core + Devilfish extensions + GuitarML integration). The Open303 engine core itself remains MIT (Robin Schmidt's original licensing is preserved in `src/dsp/open303/LICENSE`).

See [LICENSE](LICENSE) for the full GPL-3.0 text.
