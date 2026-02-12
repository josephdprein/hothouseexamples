# PitchDetectionSynth

Based on BasicSynth by Cleveland Music Co. \<<code@clevelandmusicco.com>\>

## Description

A pitch-tracking monophonic synthesizer. Plug in a guitar (or any monophonic source) and the synth follows the input pitch in real time. No MIDI or USB host required — just audio in, synth out.

**How it works:**
- **Pitch detection** (Cycfi Q library autocorrelation) extracts the fundamental frequency from the audio input
- **Envelope following** with threshold hysteresis detects note onsets and releases, gating the ADSR envelope
- The detected pitch drives the oscillator; the last confident pitch is held when detection confidence drops
- **ADSR envelope** sustains as long as the input signal is present, then releases when the signal drops

The synth features multiple waveforms, a Moog-style ladder filter, and a dry/wet mix to blend the original signal with the synthesized output.

## Controls

Toggle Switch 2 selects between two knob banks. **Bank A** (UP position) controls the synth engine. **Bank B** (DOWN position) controls detection sensitivity and mix. When you switch banks, the other bank's parameters hold their last values.

### Bank A — Synth (Toggle 2 UP)

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | FILTER | Moog ladder filter cutoff, 20Hz to 20kHz (modulated by envelope) |
| KNOB 2 | RESONANCE | Filter resonance 0.0–1.0. High resonance + low frequencies can clip! |
| KNOB 3 | ATTACK | ADSR attack time, 0.001 to 0.5 sec |
| KNOB 4 | DECAY | ADSR decay time, 0.05 to 2 sec |
| KNOB 5 | SUSTAIN | ADSR sustain level, 0.0 to 1.0 |
| KNOB 6 | RELEASE | ADSR release time, 0.05 to 2 sec |

### Bank B — Detection/Mix (Toggle 2 DOWN)

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| KNOB 1 | SENSITIVITY | Onset detection threshold. Lower = more sensitive, higher = rejects noise. Range 0.001 to 0.1 |
| KNOB 2 | DRY/WET | 0.0 = input only, 1.0 = synth only. Defaults to full wet |
| KNOB 3–6 | Unused | |

### Switches

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| SWITCH 1 | WAVEFORM | **UP** — Sine, **MIDDLE** — PolyBLEP Square, **DOWN** — PolyBLEP Saw |
| SWITCH 2 | KNOB BANK | **UP** — Bank A (synth params), **DOWN** — Bank B (detection/mix) |
| SWITCH 3 | Unused | |
| FOOTSWITCH 1 | RESET | Hold 2 seconds for bootloader mode |
| FOOTSWITCH 2 | Unused | |

## Tips

- Start with **sensitivity** (Bank B, Knob 1) around 10 o'clock for clean guitar signals. Turn it up if you get false triggers from noise, or down for quieter/more dynamic playing
- The pitch detector works best with **monophonic** input — single notes, not chords
- For a classic synth-bass-from-guitar sound: saw wave, filter around noon, moderate resonance, fast attack, medium decay, high sustain
- The dry/wet mix lets you blend your clean guitar signal underneath the synth

## Dependencies

Uses the [Cycfi Q DSP library](https://github.com/cycfi/q) (MIT license) for pitch detection and envelope following, included via the buzzbox_octa_squawker submodule.
