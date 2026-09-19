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
Input Gain -> Noise Gate -> NAM Model -> DC Block -> 3-Band EQ -> Cab IR -> Output Gain
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

## Gain staging on a high-gain model

A high-gain amp model is heavily saturated, so its output level barely
changes with input level - but the noise it amplifies does. Measured here
with a 90s Dual Rectifier (red channel, 808 in front), a -60 dBFS input
noise floor and a -20 dBFS guitar:

| `input_level` | noise out | guitar out | SNR |
|---------------|-----------|------------|------|
| 0.0 (-24 dB)  | 0.0065    | 0.104      | 24.0 dB |
| 0.3 (-13 dB)  | 0.023     | 0.106      | 13.3 dB |
| 0.5 (-6 dB)   | 0.048     | 0.106      | 6.9 dB |
| 1.0 (+12 dB)  | 0.076     | 0.106      | 2.9 dB |

The guitar is the same level in every row. Only the hiss moves. So on a
noisy input - Move's line in with a passive pickup, say - **turn
`input_level` down**, and use `output_level` to make up the volume. At the
0.5 default the hiss is 7 dB below the guitar, which reads as "white noise
with the guitar faintly behind it" and is not a fault in the model.

### Noise gate

There is one, **off by default**, and it goes where a real high-gain rig puts
it: **before** the amp. The amp is saturated, so gating after it would have
to chase a signal already compressed to a near-constant level; gating
before it simply hands the amp silence between notes. Same signals as the
table above:

| | silence | playing | SNR |
|---|---------|---------|------|
| gate off | 0.0233 | 0.0763 | 10.3 dB |
| gate on (-50 dB) | 0.0047 | 0.0763 | **24.2 dB** |

The guitar level is unchanged; only the hiss moves. A note played 20 dB
softer (-40 dBFS in) still passes at full level.

`gate_threshold` defaults to -50 dB rather than something lower because the
detector tracks close to peak, so a -60 dBFS noise floor reads about -61 dB
on it. At -55 dB the gate's own 6 dB of hysteresis shuts right on top of
the noise and it hovers half-open: measured 17.1 dB SNR against -50 dB's
24.2. Lower it if your input is quieter than that, raise it if hiss still
gets through between notes, and `gate_bypass` turns it off.

### The model's own calibration

A `.nam` records the input level it was captured at and the output level
that restores unity, and this module applies both. A high-gain model is
only voiced correctly when they are honoured, since its distortion
character is a function of how hard its input is driven.

### If you hear the clean signal alongside the processed one

Nothing in this module passes dry audio, and nothing between here and the
speaker mixes any in on purpose. Traced: the chain host calls
`process_block` in place and this module overwrites the whole buffer; the
`linein` module replaces its slot buffer rather than adding to it; and the
shim sums Schwung's ME bus **onto Move's own mailbox**
(`schwung_shim.c`, `mailbox_audio[i] + scaled_me`). Whatever Move is
already putting out goes to the DAC with Schwung's output on top of it, and
if Move is monitoring its line input, that is a dry guitar.

The giveaway is that it is level-dependent in one direction only. A
saturated amp model's output level barely moves with input level - measured
above, the same 0.076 at every input setting - while a dry monitor path
tracks the guitar's volume knob exactly. So turning the guitar up raises
the dry and not the amp, and the clean "appears" at high volume. It was
there the whole time.

**Global Settings -> Audio -> `Move->Schwung`** is the switch. It sets
`rebuild_from_la`, which zeroes the mailbox and rebuilds it from the four
per-track Link Audio channels, so anything Move mixes at master rather than
into a track is gone by construction - the same reason Move's metronome
disappears in that mode. It needs Link enabled in Move's own System
Settings, and Schwung warns if it is not.

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
