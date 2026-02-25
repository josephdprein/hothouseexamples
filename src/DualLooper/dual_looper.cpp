// DualLooper for Hothouse DIY DSP Platform
// Two independent loop samplers with variable-speed playback,
// semitone quantization, reverse, overdub, and random warble.
//
// LOOPER 1                          LOOPER 2
// --------                          --------
// Knob 1: Speed                     Knob 4: Speed
// Knob 2: Level                     Knob 5: Level
// Knob 3: Warble                    Knob 6: Warble
// Switch 1 UP:   Semitone snap      Switch 2 UP:   Semitone snap
// Switch 1 DOWN: Reverse            Switch 2 DOWN: Reverse
// Footswitch 1:  See below          Footswitch 2:  See below
//
// Switch 3: Signal routing
//   UP   = Looper 1 output feeds Looper 2 input (series: L1 → L2)
//   MID  = Both loopers record dry input independently (parallel)
//   DOWN = Looper 2 output feeds Looper 1 input (series: L2 → L1)
//
// Footswitch (short press):
//   a) No loop, not recording  → start recording
//   b) No loop, recording      → set loop end, begin playing (clean)
//   c) Has loop, playing, not overdubbing → start overdub
//   d) Has loop, playing, overdubbing     → stop overdub
//   e) Has loop, not playing   → start playing from beginning
//
// Footswitch (hold ≥1 s):
//   a) Recording, no loop → cancel recording (LED blinks 1×)
//   b) Playing            → stop playback (LED blinks 1×)
//   c) Stopped, has loop  → erase loop (LED blinks 3×)
//
// Speed knob mapping:
//   Fully CCW   = frozen (sample-and-hold, 0×)
//   Noon ±5%    = unity speed (1×) grace zone
//   Fully CW    = double speed (2×)
//
// LED: fast blink while recording, slow pulse while overdubbing,
//      solid while playing, off otherwise.
// Feedback blinks temporarily override normal LED state.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "daisysp.h"
#include "hothouse.h"

#include <cmath>

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::SaiHandle;
using daisy::System;

// 60 seconds of recording per looper at 48 kHz
#define MAX_LOOP_SAMPLES (static_cast<size_t>(48000 * 60))

Hothouse hw;

// Loop buffers in external SDRAM
float DSY_SDRAM_BSS loop_buf_1[MAX_LOOP_SAMPLES];
float DSY_SDRAM_BSS loop_buf_2[MAX_LOOP_SAMPLES];

// Frozen zone: CCW extreme → FREEZE_HI (5% of travel)
static constexpr float FREEZE_HI = 0.05f;

// Unity-speed (1×) grace zone: ±5% around noon (center of travel)
static constexpr float UNITY_LO = 0.45f;
static constexpr float UNITY_HI = 0.55f;

static constexpr float TWO_PI = 6.283185307f;
static constexpr float SAMPLE_RATE_F = 48000.f;

// Hold threshold for long-press detection (ms)
static constexpr uint32_t HOLD_MS = 1000;

// ---------------------------------------------------------------------------
// Speed knob → playback rate
//
// Zone layout (knob 0.0 = fully CCW, 1.0 = fully CW):
//   0.00 – 0.05 : frozen (0×)          — CCW extreme
//   0.05 – 0.45 : 0× → 1×             — slow ramp (0.5× lands ~25%)
//   0.45 – 0.55 : 1× grace zone        — noon ±5%
//   0.55 – 1.00 : 1× → 2×             — fast ramp
// ---------------------------------------------------------------------------
static float KnobToSpeed(float knob, bool semitone_snap) {
  float speed;

  if (knob < FREEZE_HI) {
    return 0.0f;  // frozen zone — CCW extreme
  } else if (knob < UNITY_LO) {
    // Smooth ramp: 0× at FREEZE_HI, 1× at UNITY_LO
    float t = (knob - FREEZE_HI) / (UNITY_LO - FREEZE_HI);
    speed = t;
  } else if (knob <= UNITY_HI) {
    speed = 1.0f;  // unity grace zone — noon ±5%
  } else {
    // UNITY_HI → 1×,  1.0 → 2×
    float t = (knob - UNITY_HI) / (1.0f - UNITY_HI);
    speed = 1.0f + t;
  }

  if (semitone_snap && speed > 0.01f) {
    float semitones = 12.0f * log2f(speed);
    semitones = roundf(semitones);
    if (semitones < -12.0f) semitones = -12.0f;
    if (semitones > 12.0f) semitones = 12.0f;
    speed = powf(2.0f, semitones / 12.0f);
  }

  return speed;
}

// ---------------------------------------------------------------------------
// Filtered random noise helpers
// ---------------------------------------------------------------------------
static float RandFloat(uint32_t &state) {
  state = state * 1664525u + 1013904223u;
  return static_cast<float>(static_cast<int32_t>(state)) / 2147483648.0f;
}

static inline float OnePoleCoeff(float freq_hz) {
  return 1.0f - expf(-TWO_PI * freq_hz / SAMPLE_RATE_F);
}

// ---------------------------------------------------------------------------
// Looper
// ---------------------------------------------------------------------------
struct Looper {
  float *buffer;
  size_t loop_length;
  float read_pos;
  size_t rec_pos;

  bool recording;
  bool has_loop;
  bool playing;

  // Knob / switch parameters
  float speed;
  float level;
  float warble_depth;
  bool reverse;
  bool semitone_snap;

  // Random warble — three filtered-noise bands per looper
  uint32_t rng_state;
  float noise_slow;
  float noise_mid;
  float noise_fast;

  // LED feedback blink request (set from audio ISR, consumed by main loop)
  volatile int led_blink_count;

  void Init(float *buf, uint32_t seed) {
    buffer = buf;
    loop_length = 0;
    read_pos = 0.0f;
    rec_pos = 0;
    recording = false;
    has_loop = false;
    playing = false;
    speed = 0.0f;
    level = 1.0f;
    warble_depth = 0.0f;
    reverse = false;
    semitone_snap = false;
    rng_state = seed;
    noise_slow = 0.0f;
    noise_mid = 0.0f;
    noise_fast = 0.0f;
    led_blink_count = 0;
  }

  // --- Footswitch actions ------------------------------------------------

  void NormalPress() {
    if (!has_loop && !recording) {
      // (a) Idle → start recording
      recording = true;
      rec_pos = 0;
      loop_length = 0;
    } else if (!has_loop && recording) {
      // (b) Set loop end, start playing clean (no auto-overdub)
      loop_length = rec_pos;
      has_loop = (loop_length > 1);
      if (has_loop) {
        playing = true;
        recording = false;
        read_pos = 0.0f;
      } else {
        recording = false;  // too short, cancel
      }
    } else if (has_loop && playing && !recording) {
      // (c) Playing → start overdub
      recording = true;
    } else if (has_loop && playing && recording) {
      // (d) Overdubbing → stop overdub, keep playing
      recording = false;
    } else if (has_loop && !playing) {
      // (e) Stopped → start playing from beginning
      read_pos = 0.0f;
      playing = true;
    }
  }

  void LongPress() {
    if (playing) {
      // Stop playback and any overdub
      playing = false;
      recording = false;
      led_blink_count = 1;
    } else if (has_loop || recording) {
      // Erase loop or cancel recording
      bool was_loop = has_loop;
      has_loop = false;
      recording = false;
      loop_length = 0;
      rec_pos = 0;
      read_pos = 0.0f;
      led_blink_count = was_loop ? 3 : 1;  // 3× erase, 1× cancel
    }
    // else: idle, nothing to do
  }

  // --- Per-sample audio --------------------------------------------------

  float Process(float in) {
    // Initial recording — no loop exists yet
    if (recording && !has_loop) {
      if (rec_pos < MAX_LOOP_SAMPLES) {
        buffer[rec_pos++] = in;
      }
      return 0.0f;
    }

    if (!has_loop || loop_length < 2) {
      return 0.0f;
    }

    if (!playing) {
      return 0.0f;
    }

    // --- Random warble ----------------------------------------------------
    float effective_speed = speed;
    if (warble_depth > 0.001f && speed > 0.001f) {
      float raw = RandFloat(rng_state);
      noise_slow += OnePoleCoeff(1.0f) * (raw - noise_slow);
      raw = RandFloat(rng_state);
      noise_mid += OnePoleCoeff(4.0f) * (raw - noise_mid);
      raw = RandFloat(rng_state);
      noise_fast += OnePoleCoeff(12.0f) * (raw - noise_fast);

      float wobble = 0.55f * noise_slow +
                     0.30f * noise_mid +
                     0.15f * noise_fast;
      effective_speed = speed * (1.0f + warble_depth * wobble);
      if (effective_speed < 0.0f) effective_speed = 0.0f;
    }

    // Overdub: mix input into buffer at current playback position.
    // Only write when the head is actually moving so a frozen loop
    // doesn't saturate a single sample.
    if (recording && effective_speed > 0.001f) {
      size_t wr = static_cast<size_t>(read_pos) % loop_length;
      buffer[wr] = daisysp::SoftClip(buffer[wr] + in);
    }

    // --- Read with linear interpolation -----------------------------------
    float pos = read_pos;
    float flen = static_cast<float>(loop_length);
    while (pos < 0.0f) pos += flen;
    while (pos >= flen) pos -= flen;

    size_t idx0 = static_cast<size_t>(pos);
    size_t idx1 = (idx0 + 1 < loop_length) ? idx0 + 1 : 0;
    float frac = pos - static_cast<float>(idx0);
    float sample = buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;

    // --- Advance playback head --------------------------------------------
    if (effective_speed > 0.001f) {
      if (reverse) {
        read_pos -= effective_speed;
        if (read_pos < 0.0f) read_pos += flen;
      } else {
        read_pos += effective_speed;
        if (read_pos >= flen) read_pos -= flen;
      }
    }

    return sample * level;
  }
};

// ---------------------------------------------------------------------------
// Footswitch hold-detection (fires NormalPress on release, LongPress on hold)
// ---------------------------------------------------------------------------
struct FootswitchTracker {
  bool prev_pressed;
  bool long_triggered;
  uint32_t press_start;

  void Process(bool pressed, Looper &looper) {
    if (pressed && !prev_pressed) {
      press_start = System::GetNow();
      long_triggered = false;
    }

    if (pressed && !long_triggered &&
        (System::GetNow() - press_start) >= HOLD_MS) {
      looper.LongPress();
      long_triggered = true;
    }

    if (!pressed && prev_pressed && !long_triggered) {
      looper.NormalPress();
    }

    prev_pressed = pressed;
  }
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Looper looper1, looper2;
FootswitchTracker fs1 = {false, false, 0};
FootswitchTracker fs2 = {false, false, 0};
Led led1, led2;

// ---------------------------------------------------------------------------
// Audio callback
// ---------------------------------------------------------------------------
void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  hw.ProcessAllControls();

  // ---- Footswitch handling (hold-aware) --------------------------------
  fs1.Process(hw.switches[Hothouse::FOOTSWITCH_1].Pressed(), looper1);
  fs2.Process(hw.switches[Hothouse::FOOTSWITCH_2].Pressed(), looper2);

  // ---- Switch 3: signal routing ----------------------------------------
  auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);
  bool route_l1_to_l2 = (sw3 == Hothouse::TOGGLESWITCH_UP);
  bool route_l2_to_l1 = (sw3 == Hothouse::TOGGLESWITCH_DOWN);

  // ---- Looper 1 knobs / switches --------------------------------------
  float k1 = hw.GetKnobValue(Hothouse::KNOB_1);
  float k2 = hw.GetKnobValue(Hothouse::KNOB_2);
  float k3 = hw.GetKnobValue(Hothouse::KNOB_3);

  auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  looper1.semitone_snap = (sw1 == Hothouse::TOGGLESWITCH_UP);
  looper1.reverse = (sw1 == Hothouse::TOGGLESWITCH_DOWN);
  looper1.speed = KnobToSpeed(k1, looper1.semitone_snap);
  looper1.level = k2;
  looper1.warble_depth = k3;

  // ---- Looper 2 knobs / switches --------------------------------------
  float k4 = hw.GetKnobValue(Hothouse::KNOB_4);
  float k5 = hw.GetKnobValue(Hothouse::KNOB_5);
  float k6 = hw.GetKnobValue(Hothouse::KNOB_6);

  auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  looper2.semitone_snap = (sw2 == Hothouse::TOGGLESWITCH_UP);
  looper2.reverse = (sw2 == Hothouse::TOGGLESWITCH_DOWN);
  looper2.speed = KnobToSpeed(k4, looper2.semitone_snap);
  looper2.level = k5;
  looper2.warble_depth = k6;

  // ---- Per-sample processing -------------------------------------------
  for (size_t i = 0; i < size; ++i) {
    float dry = in[0][i];
    float wet1, wet2;

    if (route_l1_to_l2) {
      // Series: L1 records dry; L2 records L1's loop output
      wet1 = looper1.Process(dry);
      wet2 = looper2.Process(wet1);
    } else if (route_l2_to_l1) {
      // Series: L2 records dry; L1 records L2's loop output
      wet2 = looper2.Process(dry);
      wet1 = looper1.Process(wet2);
    } else {
      // Parallel: both loopers record dry independently
      wet1 = looper1.Process(dry);
      wet2 = looper2.Process(dry);
    }

    out[0][i] = out[1][i] = daisysp::SoftClip(dry + wet1 + wet2);
  }
}

// ---------------------------------------------------------------------------
// LED helper — handles feedback blinks then falls back to normal state
// ---------------------------------------------------------------------------
static void UpdateLed(Led &led, Looper &looper,
                      int &blink_remaining, int &blink_timer,
                      uint32_t tick) {
  // Check for a new blink request from the audio ISR
  int req = looper.led_blink_count;
  if (req > 0) {
    looper.led_blink_count = 0;
    blink_remaining = req;
    blink_timer = 0;
  }

  if (blink_remaining > 0) {
    // Each blink: 80 ms on + 80 ms off = 160 ms per blink (8 ticks × 2)
    bool on = (blink_timer % 16) < 8;
    led.Set(on ? 1.0f : 0.0f);
    blink_timer++;
    if (blink_timer >= blink_remaining * 16) {
      blink_remaining = 0;
    }
  } else if (looper.recording && !looper.has_loop) {
    // Initial recording: fast blink (~12 Hz) — "commit carefully, loop point next"
    led.Set((tick % 4 < 2) ? 1.0f : 0.0f);
  } else if (looper.recording && looper.has_loop) {
    // Overdubbing: slow pulse (~2.5 Hz) — "adding layers, relaxed"
    led.Set((tick % 20 < 10) ? 1.0f : 0.0f);
  } else {
    led.Set(looper.playing ? 1.0f : 0.0f);
  }

  led.Update();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
  hw.Init();
  hw.SetAudioBlockSize(4);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

  looper1.Init(loop_buf_1, 0xDEADBEEF);
  looper2.Init(loop_buf_2, 0x8BADF00D);

  led1.Init(hw.seed.GetPin(Hothouse::LED_1), false);
  led2.Init(hw.seed.GetPin(Hothouse::LED_2), false);

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  int blink1_remaining = 0, blink1_timer = 0;
  int blink2_remaining = 0, blink2_timer = 0;
  uint32_t tick = 0;

  while (true) {
    hw.DelayMs(10);
    tick++;
    UpdateLed(led1, looper1, blink1_remaining, blink1_timer, tick);
    UpdateLed(led2, looper2, blink2_remaining, blink2_timer, tick);
  }

  return 0;
}
