# DualLooper

## Description

Two fully independent loop samplers with variable-speed playback, overdub, semitone quantization, reverse, and random warble modulation. Each looper can capture up to 60 seconds of audio and play it back at speeds from half to double, with a freeze mode that holds a single sample. The warble engine uses filtered random noise (not a periodic LFO) for organic, tape-like pitch drift.

## Controls

The six knobs and two toggle switches are split symmetrically between the two loopers. Switch 3 and the footswitches are shared/global.

### Knobs

| CONTROL | LOOPER | DESCRIPTION | NOTES |
|-|-|-|-|
| KNOB 1 | 1 | SPEED | Playback rate — see speed mapping below |
| KNOB 2 | 1 | LEVEL | Loop playback volume, 0.0–1.0 |
| KNOB 3 | 1 | WARBLE DEPTH | Per-looper warble amount, scaled by Switch 3 |
| KNOB 4 | 2 | SPEED | Same as Knob 1 for looper 2 |
| KNOB 5 | 2 | LEVEL | Same as Knob 2 for looper 2 |
| KNOB 6 | 2 | WARBLE DEPTH | Same as Knob 3 for looper 2 |

### Speed Knob Mapping

The speed knob has five zones with grace areas for the two most useful positions (frozen and unity):

| KNOB RANGE | ZONE | SPEED |
|-|-|-|
| Fully CCW → ~9 o'clock | Slow | 0.5× → 0× (decelerates toward frozen) |
| ~9 o'clock → just past noon | **Frozen** | 0× (sample-and-hold, buffer doesn't advance) |
| Just past noon → ~3 o'clock | Normal | 0× → 1× (accelerates toward unity) |
| ~3 o'clock | **Unity grace** | 1× (dead zone snaps to normal speed) |
| ~3 o'clock → fully CW | Fast | 1× → 2× (accelerates toward double speed) |

With **semitone snap** enabled (Switch 1/2 UP), speeds are quantized to the nearest semitone interval (2^(n/12)), covering ±12 semitones (one octave down to one octave up).

### Switches

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| SWITCH 1 | LOOPER 1 MODE | **UP** — Semitone snap: speed quantizes to chromatic intervals. **MIDDLE** — Normal. **DOWN** — Reverse: loop plays backwards |
| SWITCH 2 | LOOPER 2 MODE | Same as Switch 1 for looper 2 |
| SWITCH 3 | WARBLE INTENSITY | **UP** — Intense warble (both loopers). **MIDDLE** — No warble. **DOWN** — Subtle warble |

### Footswitches

Each footswitch controls its respective looper. Short press and hold (≥1 second) perform different actions depending on the current state:

#### Short Press

| STATE | ACTION |
|-|-|
| No loop, not recording | Start recording |
| No loop, recording | Set loop endpoint, begin playing, continue recording (overdub) |
| Has loop, recording | Stop overdubbing (playback continues) |
| Has loop, not recording, **loop mode** | Toggle play / stop |
| Has loop, not recording, **one-shot mode** | Trigger playback from beginning |

#### Hold (≥1 second)

| STATE | ACTION | LED FEEDBACK |
|-|-|-|
| Not playing (has loop) | Erase loop | 3 blinks |
| Not playing (recording) | Cancel recording | 1 blink |
| Playing | Stop playback, toggle between loop ↔ one-shot mode | 2 blinks = loop mode, 1 blink = one-shot mode |

### LEDs

| LED STATE | MEANING |
|-|-|
| Off | No loop captured, idle |
| Blinking | Recording or overdubbing |
| Solid | Loop is playing |
| Feedback blinks (1–3×) | Temporary confirmation after a hold action |

## Playback Modes

- **Loop mode** (default): Playback repeats continuously. Short press toggles play/stop.
- **One-shot mode**: Playback runs once from beginning to end, then stops automatically. Short press retriggers from the start.

Toggle between modes by holding the footswitch while the loop is playing.

## Warble

The warble engine modulates playback speed with three bands of lowpass-filtered random noise, creating non-repeating tape-like pitch drift:

| BAND | CUTOFF | CHARACTER |
|-|-|-|
| Slow | ~1 Hz | Broad pitch drift (wow) |
| Mid | ~4 Hz | Irregular wander |
| Fast | ~12 Hz | Subtle flutter / grit |

Each looper has its own independent random seed, so the two loops drift apart rather than wobbling in lockstep. The warble depth knob (per looper) scales within the range set by Switch 3.

## Signal Flow

```
Guitar In ──┬──────────────────────────────────────┐
            │                                      │
            ├──→ Looper 1 (record / overdub / play) │
            │         ↓                            │
            ├──→ Looper 2 (record / overdub / play) │
            │         ↓                            │
            └──→ Dry ─┴───── Sum ──→ SoftClip ──→ Out
```

Each looper records from the dry input independently. Overdub mixes new input into the existing buffer. Both loop outputs and the dry signal are summed through a soft clipper to prevent digital overs.
