# 303 — TB-303 Emulator for Schwung

A TB-303 emulator sound generator for [Schwung](https://github.com/charlesvestal/schwung). DSP ported from [midilab/jc303](https://github.com/midilab/jc303) (Robin Schmidt's Open303 + Devilfish-inspired mods + GuitarML RNN overdrive). No built-in sequencer — drive it with any MIDI source.

Pairs with [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po) for generative acid basslines.

## Install

From the Schwung Module Store, or manually:

```bash
git clone https://github.com/charlesvestal/schwung-303
cd schwung-303
./scripts/build.sh && ./scripts/install.sh
```

## MIDI convention

- **Accent**: high velocity (≥ ~100) on note-on.
- **Slide**: new note-on arrives before the previous note-off (legato). Open303 detects this natively.

This matches tb3po's emission exactly — no translation needed.

## License

GPL-3.0, inherited from jc303. The Open303 engine core is MIT (Robin Schmidt).

## Credits

- Open303: Robin Schmidt — [RobinSchmidt/Open303](https://github.com/RobinSchmidt/Open303)
- JC-303 port + Devilfish mods + GuitarML overdrive: [midilab/jc303](https://github.com/midilab/jc303)
- RTNeural: [jatinchowdhury18/RTNeural](https://github.com/jatinchowdhury18/RTNeural)
- GuitarML tone models: [GuitarML Proteus Tone Packs](https://github.com/GuitarML/Proteus_Tone_Packs)
- Schwung port: Charles Vestal
