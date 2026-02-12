# PitchDetectionSynth

Based on BasicSynth by Cleveland Music Co. \<<code@clevelandmusicco.com>\>

## Description

A pitch-tracking monophonic synthesizer with dual bidirectional delay lines. Plug in a guitar (or any monophonic source) and the synth follows the input pitch in real time. No MIDI or USB host required — just audio in, synth out.

**How it works:**
- **Pitch detection** (Cycfi Q library autocorrelation) extracts the fundamental frequency from the audio input
- **Envelope following** with threshold hysteresis detects note onsets and releases, gating the ADSR envelope
- The detected pitch drives the oscillator; the last confident pitch is held when detection confidence drops
- **ADSR envelope** sustains as long as the input signal is present, then releases when the signal drops
- **Two parallel delay lines** with forward and reverse modes feed from the post-mix signal

The synth features multiple waveforms, a Moog-style ladder filter, a dry/wet mix, and two independently configurable delay lines that can each run forward or reversed.

## Controls

Toggle Switch 2 selects between three knob banks. When you switch banks, the other banks' parameters hold their last values.

- **Bank A** (UP) — Synth engine
- **Bank B** (MIDDLE) — Detection sensitivity and mix
- **Bank C** (DOWN) — Dual delay lines

### Bank A — Synth (Toggle 2 UP)

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | FILTER | Moog ladder filter cutoff, 20Hz to 20kHz (modulated by envelope) |
| KNOB 2 | RESONANCE | Filter resonance 0.0–1.0. High resonance + low frequencies can clip! |
| KNOB 3 | ATTACK | ADSR attack time, 0.001 to 0.5 sec |
| KNOB 4 | DECAY | ADSR decay time, 0.05 to 2 sec |
| KNOB 5 | SUSTAIN | ADSR sustain level, 0.0 to 1.0 |
| KNOB 6 | RELEASE | ADSR release time, 0.05 to 2 sec |

### Bank B — Detection/Mix (Toggle 2 MIDDLE)

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | SENSITIVITY | Onset detection threshold. Lower = more sensitive, higher = rejects noise. Range 0.001 to 0.1 |
| KNOB 2 | DRY/WET | 0.0 = input only, 1.0 = synth only. Defaults to full wet |
| KNOB 3–6 | Unused | |

### Bank C — Delays (Toggle 2 DOWN)

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

### Switches

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| SWITCH 1 | WAVEFORM | **UP** — Sine, **MIDDLE** — PolyBLEP Square, **DOWN** — PolyBLEP Saw |
| SWITCH 2 | KNOB BANK | **UP** — Bank A (synth), **MIDDLE** — Bank B (detection/mix), **DOWN** — Bank C (delays) |
| SWITCH 3 | Unused | |
| FOOTSWITCH 1 | RESET | Hold 2 seconds for bootloader mode |
| FOOTSWITCH 2 | Unused | |

## Tips

- Start with **sensitivity** (Bank B, Knob 1) around 10 o'clock for clean guitar signals. Turn it up if you get false triggers from noise, or down for quieter/more dynamic playing
- The pitch detector works best with **monophonic** input — single notes, not chords
- For a classic synth-bass-from-guitar sound: saw wave, filter around noon, moderate resonance, fast attack, medium decay, high sustain
- The dry/wet mix lets you blend your clean guitar signal underneath the synth
- **Reverse delay** works best with longer delay times — short reverse chunks can sound glitchy (which may be what you want)
- Try one delay forward and one reversed for ambient/textural sounds
- The delays run in parallel and sum with the dry signal, so watch your output level with both volumes up

## Signal Flow

```
Guitar In → Pitch Detect + Envelope Follow
                ↓
           Oscillator → Moog Ladder Filter → ADSR Envelope
                ↓
           Dry/Wet Mix (input + synth)
                ↓
           ┌─── Delay 1 (forward or reverse) ───┐
           ├─── Delay 2 (forward or reverse) ───┤
           └─── Dry signal ─────────────────────┘
                ↓ (summed)
           Stereo Output
```

## Dependencies

Uses the [Cycfi Q DSP library](https://github.com/cycfi/q) (MIT license) for pitch detection and envelope following, included via the buzzbox_octa_squawker submodule.
