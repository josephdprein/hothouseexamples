# DualLooper

## Description

Two fully independent loop samplers with variable-speed playback, re-overdub, semitone quantization, reverse, and random warble modulation. Each looper can capture up to 60 seconds of audio and play it back at speeds from near-zero to double, with a freeze mode that holds a single sample. The warble engine uses filtered random noise (not a periodic LFO) for organic, tape-like pitch drift. Switch 3 sets the signal routing between the two loopers, enabling series chaining for cascaded processing.

## Controls

The six knobs and Switches 1/2 are split symmetrically between the two loopers. Switch 3 and the footswitches are shared/global.

### Knobs

| CONTROL | LOOPER | DESCRIPTION | NOTES |
|-|-|-|-|
| KNOB 1 | 1 | SPEED | Playback rate — see speed mapping below |
| KNOB 2 | 1 | LEVEL | Loop playback volume, 0.0–1.0 |
| KNOB 3 | 1 | WARBLE | Warble depth: CCW = none, CW = maximum |
| KNOB 4 | 2 | SPEED | Same as Knob 1 for looper 2 |
| KNOB 5 | 2 | LEVEL | Same as Knob 2 for looper 2 |
| KNOB 6 | 2 | WARBLE | Same as Knob 3 for looper 2 |

### Speed Knob Mapping

| KNOB RANGE | ZONE | SPEED |
|-|-|-|
| Fully CCW → ~9 o'clock | **Frozen** | 0× (sample-and-hold, buffer doesn't advance) |
| ~9 o'clock → noon | Slow | 0× → 1× (half-speed lands around 9–10 o'clock) |
| Noon ± 5% | **Unity grace** | 1× (dead zone snaps to normal speed) |
| Noon → fully CW | Fast | 1× → 2× |

Turning the knob all the way counter-clockwise freezes the loop. Noon is normal speed. Clockwise goes up to double speed.

With **semitone snap** enabled (Switch 1/2 UP), speeds are quantized to the nearest semitone interval (2^(n/12)), covering ±12 semitones (one octave down to one octave up). Half-speed (-12 semitones, 0.5×) lands naturally in the slow zone at around 9–10 o'clock.

### Switches

| CONTROL | DESCRIPTION | NOTES |
|-|-|-|
| SWITCH 1 | LOOPER 1 MODE | **UP** — Semitone snap: speed quantizes to chromatic intervals. **MIDDLE** — Normal. **DOWN** — Reverse: loop plays backwards |
| SWITCH 2 | LOOPER 2 MODE | Same as Switch 1 for looper 2 |
| SWITCH 3 | ROUTING | **UP** — Series: Looper 1 output feeds Looper 2 input. **MIDDLE** — Parallel: both loopers record dry independently. **DOWN** — Series: Looper 2 output feeds Looper 1 input |

### Footswitches

Each footswitch controls its respective looper. Short press and hold (≥1 second) perform different actions depending on the current state:

#### Short Press

| STATE | ACTION |
|-|-|
| No loop, not recording | Start recording |
| No loop, recording | Set loop endpoint, begin playing (clean — no auto-overdub) |
| Has loop, playing, not overdubbing | Start overdub |
| Has loop, playing, overdubbing | Stop overdub (playback continues) |
| Has loop, not playing | Start playing from beginning |

#### Hold (≥1 second)

| STATE | ACTION | LED FEEDBACK |
|-|-|-|
| Playing (with or without overdub) | Stop playback | 1 blink |
| Stopped (has loop) | Erase loop | 3 blinks |
| Recording (no loop) | Cancel recording | 1 blink |

### LEDs

| LED STATE | MEANING |
|-|-|
| Off | No loop captured, idle |
| Fast blink (~12 Hz) | Initial recording — next press sets the loop point |
| Slow pulse (~2.5 Hz) | Overdubbing — adding layers to an existing loop |
| Solid | Loop is playing |
| Feedback blinks (1× or 3×) | Confirmation after a hold action |

## Warble

The warble engine modulates playback speed with three bands of lowpass-filtered random noise, creating non-repeating tape-like pitch drift:

| BAND | CUTOFF | CHARACTER |
|-|-|-|
| Slow | ~1 Hz | Broad pitch drift (wow) |
| Mid | ~4 Hz | Irregular wander |
| Fast | ~12 Hz | Subtle flutter / grit |

Each looper has its own independent random seed, so the two loops drift apart rather than wobbling in lockstep. Turn the warble knob (3 or 6) from fully CCW (none) to fully CW (maximum depth).

## Signal Routing (Switch 3)

Switch 3 controls how the two loopers connect to each other:

- **Parallel (MIDDLE):** Both loopers independently record from the dry guitar signal. This is the default mode.
- **Series L1→L2 (UP):** Looper 1 records the dry signal. Looper 2 records Looper 1's loop output — capturing it after any pitch shift, speed change, or warble applied to L1. Record into L1 first, then arm L2 to freeze a transformed version of it.
- **Series L2→L1 (DOWN):** Same as above with the roles reversed.

Series routing enables cascaded tape-loop style processing: L1 can warp the source material (pitch, speed, warble) before L2 captures it as its own independent loop.

## Signal Flow

**Parallel (Switch 3 middle):**
```
Guitar In ──┬──────────────────────────────────────┐
            │                                      │
            ├──→ Looper 1 (record / overdub / play) │
            │         ↓                            │
            ├──→ Looper 2 (record / overdub / play) │
            │         ↓                            │
            └──→ Dry ─┴───── Sum ──→ SoftClip ──→ Out
```

**Series L1→L2 (Switch 3 up):**
```
Guitar In ──→ Looper 1 (record / overdub / play)
                   ↓ wet1
              Looper 2 (records wet1 / plays back)
                   ↓ wet2
Guitar In ─────────┴──── Sum ──→ SoftClip ──→ Out
```

Overdub mixes new input into the existing buffer at the current playback position. All loop outputs and the dry signal are summed through a soft clipper to prevent digital overs.
