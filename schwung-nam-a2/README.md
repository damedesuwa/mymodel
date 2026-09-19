# Nam A2

Neural Amp Modeler audio effect module for [Schwung](https://github.com/charlesvestal/move-everything)
on Ableton Move, based on [schwung-nam](https://github.com/charlesvestal/schwung-nam)
by Charles Vestal. Adds a Full/Lite quality switch and the model's own level calibration.

Built on [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) by Mike
Oliphant and [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
by Steven Atkinson.

## Prerequisites

- Schwung installed on your Ableton Move

## Signal chain

```
Input Channel -> Input Gain -> NAM Model -> DC Block -> Cab IR -> Output Gain
```

For delay, reverb or chorus, add them as separate FX in the slot's own
chain - the host already hosts those, and keeping them out of this module
keeps `create_instance` (which runs on the SPI audio callback, where
allocation is forbidden) light.

## Features

- **Neural amp/effect modeling**: run trained `.nam` / `.aidax` models for
  realistic amp and pedal emulation.
- **Quality: Full / Slim / Lite.** Full and Slim are the *model's* own
  tiers, not ours. An A2 `.nam` is a SlimmableContainer carrying several
  trained WaveNets, picked by quality scale - a Dual Rectifier capture here
  carries two, at 8 and 3 channels. Slim asks for the low tier, which is a
  properly trained smaller model rather than a rate hack. Lite additionally
  halves the rate and is the fallback that works on a plain WaveNet `.nam`,
  where `SetQualityScaleFactor` is a no-op.

  Measured on that capture, per 128-frame block:

  | | x86 | vs Full | Move (ARM) |
  |---|-----|---------|------------|
  | Full (q 1.0) | 173 us | 1.00x | **~1190 us** |
  | Slim (q 0.5) | 32 us | 0.18x | **~219 us** |
  | Lite (q 0.5 + half rate) | 18 us | 0.10x | ~120 us |

  The Move column is the measured `Slot fx max(us)` for Full, scaled by the
  x86 ratios. Against Move's ~2370 us frame slack, Full is about half the
  frame on its own.

- **CPU meter.** `cpu` is a read-only, `live` percentage of that frame slack,
  sitting beside Input and Output on the first page. Peak-held with a slow
  decay rather than averaged, because an average hides the one block that
  overruns. It stops moving when the slot goes silent - the shim skips a
  silent slot - which is the meter being honest, not stuck.

- **Cabinet IR convolution**: apply cabinet impulse responses with optional
  bypass.
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
| `input_mode` | Left / Right / Sum L+R | Left | Which channel feeds the mono model |
| `output_level` | 0.0-1.0 | 0.85 | Output gain after the whole chain |
| `quality` | Full / Slim / Lite | Full | Model's own tier, then a rate fallback |
| `cpu` | 0-100 % | — | Read-only: worst recent block, as % of frame slack |
| `cab_bypass` | 0-1 | 0 | Bypass cabinet IR convolution |

## Move monitors its own line input, and you cannot turn that off from here

A guitar into Move's line in is audible **dry** alongside whatever this
module produces. That is Move, not this module, and it was established by
elimination rather than assumed: removing the Line In module from the slot
entirely leaves the dry guitar audible, with no Schwung module reading the
input at all. Zeroing Move's mailbox (Global Settings -> Audio ->
`Move->Schwung`) does not remove it either, nor does Move's own monitoring
toggle - with monitoring off you hear only the dry signal, with it on you
hear dry plus the amp. So the dry path sits below the software mixer
entirely, in the XMOS audio hardware, and nothing in this module can see or
subtract it.

What it leaves you is a balance problem, and only one side of it moves.
**A saturated amp model's output level does not follow its input** -
measured, the same 0.076 rms across a 36 dB input range - while the dry
monitor tracks the guitar's volume knob one for one. Two consequences:

- **Turning the guitar down lowers the dry and not the amp.** It costs
  nothing.
- **`output_level` is the lever that buries it.** Measured on a -34 dBFS
  input: 0.5 gives rms 0.075 / peak 0.11, 0.85 gives 0.320 / 0.48, and 1.0
  gives 0.595 / 0.89 - none of them clipping a sample. That is +18 dB of
  amp against an unchanged dry signal.

Hence the 0.85 default. The knob maps 0..1 to -24..+12 dB, so the old 0.5
was -6 dB on top of an amp that already sits near -22 dBFS: simply quiet.

## The input channel, and why Left is the default

A mono guitar reaches Move through a TRS jack with its ring tied to sleeve,
so the **right channel is silent** - Move's own setting says as much
(`lineInRecordingMode: monoFromLeftChannel`). Averaging L+R would drive the
model at (L + 0) / 2, 6 dB under the signal that is actually there.

Left is the default because it is right for both mono cases: a source on
the left alone, and a mono source duplicated to both channels, where
(L+L)/2 and L are the same number - measured identical to 0.00 dB. Only a
genuinely stereo source needs `Sum L+R`.

Be honest about what this buys: at the **output** it is +0.2 to +0.5 dB,
not +6, because the amp is saturated and 6 dB more drive does not make 6 dB
more output. What it fixes is the amp's operating point - a high-gain
model's character is a function of how hard its input is driven - not the
level.

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

There is no noise gate here. On a noisy input, keep `input_level` low as
above, or put a gate ahead of this module in the slot's own FX chain -
`linein` has one.

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
