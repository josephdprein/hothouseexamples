# PitchDetectionSynth

Based on BasicSynth by Cleveland Music Co. \<<code@clevelandmusicco.com>\>

## Description

A pitch-tracking monophonic synthesizer with dual bidirectional delay lines and LFO modulation. Plug in a guitar (or any monophonic source) and the synth follows the input pitch in real time. No MIDI or USB host required — just audio in, synth out.

**How it works:**
- **Pitch detection** (Cycfi Q library autocorrelation) extracts the fundamental frequency from the audio input
- **Envelope following** with threshold hysteresis detects note onsets and releases, gating the ADSR envelope
- The detected pitch drives the oscillator; the last confident pitch is held when detection confidence drops
- **ADSR envelope** sustains as long as the input signal is present, then releases when the signal drops
- **Two parallel delay lines** with forward and reverse modes feed from the post-mix signal
- **Three sine LFOs** modulate the filter cutoff and both delay times, toggled on/off via footswitch

## Controls

Four knob banks are selected by the combination of Toggle Switches 2 and 3. When you switch banks, the other banks' parameters hold their last values. **Knob pickup** prevents parameter jumps: after switching banks, each knob must be physically moved before it updates its value.

| SW 2 | SW 3 | Bank |
|------|------|------|
| UP | UP | **A** — Synth |
| DOWN | UP | **B** — Detection/Mix/Reverb |
| UP | DOWN | **C** — Delays |
| DOWN | DOWN | **D** — LFOs |

MIDDLE position on either switch is treated the same as UP for bank selection, but enables additional features:

- **SW2 MIDDLE** — LFO 1 modulates **pitch** instead of filter cutoff (when mod is active)
- **SW3 MIDDLE** — LFO 1 **resets phase** on each note onset (trigger sync)

### Bank A — Synth (SW2 UP, SW3 UP)

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | FILTER | Moog ladder filter cutoff, 20Hz to 20kHz (modulated by envelope + LFO 1) |
| KNOB 2 | RESONANCE | Filter resonance 0.0–1.0. High resonance + low frequencies can clip! |
| KNOB 3 | ATTACK | ADSR attack time, 0.001 to 0.5 sec |
| KNOB 4 | DECAY | ADSR decay time, 0.05 to 2 sec |
| KNOB 5 | SUSTAIN | ADSR sustain level, 0.0 to 1.0 |
| KNOB 6 | RELEASE | ADSR release time, 0.05 to 2 sec |

### Bank B — Detection/Mix/Reverb (SW2 DOWN, SW3 UP)

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | SENSITIVITY | Onset detection threshold. Lower = more sensitive, higher = rejects noise. Range 0.001 to 0.1 |
| KNOB 2 | GAIN | Pre-delay gain stage. Noon = unity, CCW = attenuate to silence, CW = tanh soft saturation |
| KNOB 3 | DRY/WET | 0.0 = input only, 1.0 = synth only. Defaults to full wet |
| KNOB 4 | REVERB SEND | How much signal is sent to the reverb, 0.0–1.0. Fully CCW = no reverb |
| KNOB 5 | REVERB DECAY | Reverb tail length (feedback), 0.3–0.999. Higher = longer decay |
| KNOB 6 | REVERB TONE | Reverb low-pass filter, 500 Hz–16 kHz. Lower = darker reverb |

### Bank C — Delays (SW2 UP, SW3 DOWN)

Each delay line's time knob is bidirectional around noon:

| | CCW | Noon | CW |
|-|-|-|-|
| **Direction** | Reversed | No delay | Forward |
| **Time** | 2000ms | 0ms | 2000ms |

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | DELAY 1 TIME | Bidirectional: CCW = reversed, noon = off, CW = forward. 0–2000ms |
| KNOB 2 | DELAY 1 VOLUME | Return level of delay 1 output, 0.0–1.0 |
| KNOB 3 | DELAY 1 FEEDBACK | 0.0–0.95. Higher = more repeats |
| KNOB 4 | DELAY 2 TIME | Same as Delay 1 time |
| KNOB 5 | DELAY 2 VOLUME | Return level of delay 2 output, 0.0–1.0 |
| KNOB 6 | DELAY 2 FEEDBACK | 0.0–0.95 |

### Bank D — LFOs (SW2 DOWN, SW3 DOWN)

Three sine-wave LFOs in vertical pairs (rate on top row, depth on bottom row):

```
  KNOB 1         KNOB 2         KNOB 3
  LFO 1 Rate     LFO 2 Rate     LFO 3 Rate
  (filter)       (delay 1)      (delay 2)

  KNOB 4         KNOB 5         KNOB 6
  LFO 1 Depth    LFO 2 Depth    LFO 3 Depth
  (filter)       (delay 1)      (delay 2)
```

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | LFO 1 RATE | Filter cutoff modulation rate, 0.05–20 Hz |
| KNOB 2 | LFO 2 RATE | Delay 1 time modulation rate, 0.05–20 Hz |
| KNOB 3 | LFO 3 RATE | Delay 2 time modulation rate, 0.05–20 Hz |
| KNOB 4 | LFO 1 DEPTH | Filter cutoff mod depth. Fully CCW = no modulation |
| KNOB 5 | LFO 2 DEPTH | Delay 1 time mod depth. Fully CCW = no modulation |
| KNOB 6 | LFO 3 DEPTH | Delay 2 time mod depth. Fully CCW = no modulation |

### Switches and Footswitches

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| SWITCH 1 | WAVEFORM | **UP** — Sine, **MIDDLE** — PolyBLEP Square, **DOWN** — PolyBLEP Saw |
| SWITCH 2 | BANK SELECT (row) | Combined with Switch 3 — see bank table above |
| SWITCH 3 | BANK SELECT (col) | Combined with Switch 2 — see bank table above |
| FOOTSWITCH 1 | MOD / FREEZE | Short press toggles LFO modulation (LED 1 = active). Hold to freeze delay lines — buffers loop without updating. Long press (2s) = bootloader mode |
| FOOTSWITCH 2 | BYPASS / SUSTAIN | Short press toggles effect on/off. **Hold** to force infinite sustain — the ADSR gate stays open regardless of input level. LED 2 lights when active (effect engaged). |

## Tips

- Start with **sensitivity** (Bank B, Knob 1) around 10 o'clock for clean guitar signals. Turn it up if you get false triggers from noise, or down for quieter/more dynamic playing
- The pitch detector works best with **monophonic** input — single notes, not chords
- For a classic synth-bass-from-guitar sound: saw wave, filter around noon, moderate resonance, fast attack, medium decay, high sustain
- The dry/wet mix lets you blend your clean guitar signal underneath the synth
- **Reverse delay** works best with longer delay times — short reverse chunks can sound glitchy (which may be what you want)
- Try one delay forward and one reversed for ambient/textural sounds
- The delays run in parallel and sum with the dry signal, so watch your output level with both volumes up
- LFO on the filter creates classic auto-wah and filter sweep effects
- LFO on delay times creates chorus-like pitch modulation effects — try slow rates with subtle depth
- All depth knobs at fully CCW = zero modulation, so you can set rates first then bring in depth to taste
- The reverb adds space to the overall signal (post-delays). Keep the send moderate to avoid wash-out, or crank it for ambient pads
- After switching banks, wiggle each knob slightly before expecting it to respond — this is the **knob pickup** preventing accidental jumps

## Signal Flow

```
Guitar In → Pitch Detect + Envelope Follow
                ↓
           Oscillator → Moog Ladder Filter ←── LFO 1 (when mod active)
                            ↓
                       ADSR Envelope
                            ↓
                       Dry/Wet Mix (input + synth)
                            ↓
           ┌─── Delay 1 (fwd/rev) ←── LFO 2 (when mod active) ───┐
           ├─── Delay 2 (fwd/rev) ←── LFO 3 (when mod active) ───┤
           └─── Dry signal ───────────────────────────────────────┘
                            ↓ (summed)
                       ┌─── Reverb (send/return) ───┐
                       └─── Dry ────────────────────┘
                            ↓ (summed)
                       Stereo Output
```

## Dependencies

Uses the [Cycfi Q DSP library](https://github.com/cycfi/q) (MIT license) for pitch detection and envelope following, included via the buzzbox_octa_squawker submodule. Make sure the submodule and its nested dependencies are initialized before building:

```sh
git submodule update --init --recursive
```
