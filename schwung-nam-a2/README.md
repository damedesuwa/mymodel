# Nam A2

Neural Amp Modeler audio effect module for [Schwung](https://github.com/charlesvestal/move-everything)
on Ableton Move, based on [schwung-nam](https://github.com/charlesvestal/schwung-nam)
by Charles Vestal. Adds a 3-band EQ and a Full/Lite quality switch.

Built on [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) by Mike
Oliphant and [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
by Steven Atkinson.

## Prerequisites

- Schwung installed on your Ableton Move

## Signal chain

```
Input Gain -> NAM Model -> 3-Band EQ -> Cab IR -> Output Gain
```

For delay, reverb or chorus, add them as separate FX in the slot's own
chain - the host already hosts those, and keeping them out of this module
keeps `create_instance` (which runs on the SPI audio callback, where
allocation is forbidden) light.

## Features

- **Neural amp/effect modeling**: run trained `.nam` / `.aidax` models for
  realistic amp and pedal emulation.
- **Full / Lite quality switch**: Lite halves the neural net's per-block
  work (processes at half rate, holds each output sample for two frames) to
  free up CPU headroom on Move's ARM core when a heavy model plus the cab
  IR and EQ would otherwise miss the real-time budget.
  Measure with the CPU page (`docs/DIAGNOSTICS.md` in the host repo) before
  relying on it - it's a genuine tradeoff, not free.
- **Cabinet IR convolution**: apply cabinet impulse responses with optional
  bypass.
- **3-band EQ**: independent gain + frequency for Low (shelf), Mid (bell)
  and High (shelf) bands. It is the amp's tone stack, so it sits between
  the model and the cab - the same place it does on real hardware.
- **Model / cabinet browsers**: hierarchical file browsers for selecting
  `.nam` model files and `.wav` cabinet IRs.
- **Input/Output level**: independent gain staging controls.

## Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `input_level` | 0.0-1.0 | 0.5 | Input gain before model processing |
| `output_level` | 0.0-1.0 | 0.5 | Output gain after the whole chain |
| `quality` | Full / Lite | Full | Neural net CPU/quality tradeoff |
| `cab_bypass` | 0-1 | 0 | Bypass cabinet IR convolution |
| `eq_low_gain` / `eq_low_freq` | -15..15 dB / 40..500 Hz | 0 dB / 100 Hz | Low shelf |
| `eq_mid_gain` / `eq_mid_freq` | -15..15 dB / 200..4000 Hz | 0 dB / 800 Hz | Mid bell |
| `eq_high_gain` / `eq_high_freq` | -15..15 dB / 1000..10000 Hz | 0 dB / 3000 Hz | High shelf |

## Adding Models and Cabinets

Place `.nam` model files and `.wav` cabinet IRs in the module directory on
your Move (or via the Schwung Manager web UI at `move.local:7700`):

```
/data/UserData/schwung/modules/audio_fx/nam-a2/models/
/data/UserData/schwung/modules/audio_fx/nam-a2/cabs/
```

NAM models can be trained with the
[Neural Amp Modeler Trainer](https://github.com/sdatkinson/neural-amp-modeler).
Find pretrained models at [tone3000.com](https://tone3000.com).

## Building

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

See `BUILDING.md` in the host repo for cross-compilation details; this
module follows the same "External Module Development" layout described in
the host's `CLAUDE.md`.

## Publishing

This repo ships with placeholder GitHub URLs in `release.json` and
`.github/workflows/release.yml` (`YOUR_GITHUB_USERNAME/schwung-nam-a2`).
Before your first tagged release, push this to your own GitHub repo and
update those placeholders (or just tag `v0.1.0` - `release.yml` rewrites
`release.json` for you on every tag push once the repo exists).

## Credits

- **schwung-nam**: [Charles Vestal](https://github.com/charlesvestal/schwung-nam) (MIT License) - the NAM model loading, cab IR convolution and background loader thread this module builds on.
- **NeuralAmpModelerCore**: [Steven Atkinson](https://github.com/sdatkinson/NeuralAmpModelerCore) (MIT License)
- **NeuralAudio**: [Mike Oliphant](https://github.com/mikeoliphant/NeuralAudio) (MIT License)
- **RTNeural**: [Jatin Chowdhury](https://github.com/jatinchowdhury18/RTNeural) (BSD 3-Clause License)
- **math_approx**: [Jatin Chowdhury](https://github.com/jatinchowdhury18/math_approx) (BSD 3-Clause License)

## License

MIT License - See `LICENSE` file for details. See `THIRD_PARTY_LICENSES.md`
for the licenses of bundled/linked dependencies.

## AI Assistance Disclaimer

This module was developed with AI assistance. All architecture,
implementation and release decisions should be reviewed by a human
maintainer before publishing - in particular, the DSP has not been
validated on real Move hardware; validate CPU headroom (see
`docs/DIAGNOSTICS.md` in the host repo) and audio correctness before
relying on it live.
