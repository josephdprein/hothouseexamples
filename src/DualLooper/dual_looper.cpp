// DualLooper for Hothouse DIY DSP Platform
// Two independent loop samplers with variable-speed playback,
// semitone quantization, reverse, and warble modulation.
//
// LOOPER 1                          LOOPER 2
// --------                          --------
// Knob 1: Speed                     Knob 4: Speed
// Knob 2: Level                     Knob 5: Level
// Knob 3: Warble                    Knob 6: Warble
// Switch 1 UP:   Semitone snap      Switch 2 UP:   Semitone snap
// Switch 1 DOWN: Reverse            Switch 2 DOWN: Reverse
// Footswitch 1:  Record/Stop        Footswitch 2:  Record/Stop
//
// Speed knob mapping:
//   Fully CCW  = half speed (0.5x)
//   Noon ±10%  = frozen (sample-and-hold)
//   Fully CW   = double speed (2x)
//
// LED 1 blinks while recording looper 1, solid when loop exists.
// LED 2 blinks while recording looper 2, solid when loop exists.
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

// Frozen-zone boundaries: noon (0.5) ± 10% of total travel
static constexpr float FREEZE_LO = 0.40f;
static constexpr float FREEZE_HI = 0.60f;

static constexpr float TWO_PI = 6.283185307f;
static constexpr float SAMPLE_RATE_F = 48000.f;

// Convert a 0–1 knob value to a playback rate.
// CCW = 0.5x, noon dead-zone = frozen, CW = 2x.
// When semitone_snap is true, quantize to nearest semitone.
static float KnobToSpeed(float knob, bool semitone_snap) {
  float speed;

  if (knob <= FREEZE_LO) {
    // 0.0 → 0.5x,  FREEZE_LO → 0x
    float t = knob / FREEZE_LO;            // 0 … 1
    speed = 0.5f * (1.0f - t);
  } else if (knob >= FREEZE_HI) {
    // FREEZE_HI → 0x,  1.0 → 2x
    float t = (knob - FREEZE_HI) / (1.0f - FREEZE_HI);  // 0 … 1
    speed = 2.0f * t;
  } else {
    return 0.0f;  // frozen zone
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

struct Looper {
  float *buffer;
  size_t loop_length;  // recorded length in samples
  float read_pos;      // fractional playback head
  size_t rec_pos;      // write head during recording

  bool recording;
  bool has_loop;

  // Parameters (updated each block from knobs/switches)
  float speed;
  float level;
  float warble_depth;
  bool reverse;
  bool semitone_snap;

  // Warble LFO (two components for a richer wow-flutter)
  float lfo_phase_slow;
  float lfo_phase_fast;

  void Init(float *buf) {
    buffer = buf;
    loop_length = 0;
    read_pos = 0.0f;
    rec_pos = 0;
    recording = false;
    has_loop = false;
    speed = 0.0f;
    level = 1.0f;
    warble_depth = 0.0f;
    reverse = false;
    semitone_snap = false;
    lfo_phase_slow = 0.0f;
    lfo_phase_fast = 0.0f;
  }

  void ToggleRecord() {
    if (!recording) {
      recording = true;
      rec_pos = 0;
      loop_length = 0;
      has_loop = false;
    } else {
      recording = false;
      loop_length = rec_pos;
      has_loop = (loop_length > 0);
      read_pos = 0.0f;
    }
  }

  // Returns the loop playback sample (wet only, no dry).
  // Writes incoming audio into the buffer while recording.
  float Process(float in) {
    // --- Recording --------------------------------------------------
    if (recording) {
      if (rec_pos < MAX_LOOP_SAMPLES) {
        buffer[rec_pos++] = in;
      }
      return 0.0f;  // no playback output while recording
    }

    if (!has_loop || loop_length < 2) {
      return 0.0f;
    }

    // --- Warble LFO -------------------------------------------------
    float effective_speed = speed;
    if (warble_depth > 0.001f && speed > 0.001f) {
      // Slow wow (~1.5 Hz) + faster flutter (~6 Hz)
      lfo_phase_slow += 1.5f / SAMPLE_RATE_F;
      if (lfo_phase_slow >= 1.0f) lfo_phase_slow -= 1.0f;
      lfo_phase_fast += 6.0f / SAMPLE_RATE_F;
      if (lfo_phase_fast >= 1.0f) lfo_phase_fast -= 1.0f;

      float lfo = 0.7f * sinf(lfo_phase_slow * TWO_PI) +
                  0.3f * sinf(lfo_phase_fast * TWO_PI);
      effective_speed = speed * (1.0f + warble_depth * 0.3f * lfo);
      if (effective_speed < 0.0f) effective_speed = 0.0f;
    }

    // --- Read with linear interpolation -----------------------------
    float pos = read_pos;
    float flen = static_cast<float>(loop_length);

    // Wrap into range
    while (pos < 0.0f) pos += flen;
    while (pos >= flen) pos -= flen;

    size_t idx0 = static_cast<size_t>(pos);
    size_t idx1 = (idx0 + 1 < loop_length) ? idx0 + 1 : 0;
    float frac = pos - static_cast<float>(idx0);
    float sample = buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;

    // --- Advance playback head --------------------------------------
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

Looper looper1, looper2;

Led led1, led2;

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  hw.ProcessAllControls();

  // ---- Looper 1 controls (knobs 1-3, switch 1, footswitch 1) ------
  float k1 = hw.GetKnobValue(Hothouse::KNOB_1);
  float k2 = hw.GetKnobValue(Hothouse::KNOB_2);
  float k3 = hw.GetKnobValue(Hothouse::KNOB_3);

  auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  looper1.semitone_snap = (sw1 == Hothouse::TOGGLESWITCH_UP);
  looper1.reverse = (sw1 == Hothouse::TOGGLESWITCH_DOWN);

  looper1.speed = KnobToSpeed(k1, looper1.semitone_snap);
  looper1.level = k2;
  looper1.warble_depth = k3;

  if (hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge()) {
    looper1.ToggleRecord();
  }

  // ---- Looper 2 controls (knobs 4-6, switch 2, footswitch 2) ------
  float k4 = hw.GetKnobValue(Hothouse::KNOB_4);
  float k5 = hw.GetKnobValue(Hothouse::KNOB_5);
  float k6 = hw.GetKnobValue(Hothouse::KNOB_6);

  auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  looper2.semitone_snap = (sw2 == Hothouse::TOGGLESWITCH_UP);
  looper2.reverse = (sw2 == Hothouse::TOGGLESWITCH_DOWN);

  looper2.speed = KnobToSpeed(k4, looper2.semitone_snap);
  looper2.level = k5;
  looper2.warble_depth = k6;

  if (hw.switches[Hothouse::FOOTSWITCH_2].RisingEdge()) {
    looper2.ToggleRecord();
  }

  // ---- Audio processing --------------------------------------------
  for (size_t i = 0; i < size; ++i) {
    float dry = in[0][i];
    float wet1 = looper1.Process(dry);
    float wet2 = looper2.Process(dry);

    // Mix dry + both loopers, soft-clip to prevent digital overs
    float mix = dry + wet1 + wet2;
    out[0][i] = out[1][i] = daisysp::SoftClip(mix);
  }
}

int main() {
  hw.Init();
  hw.SetAudioBlockSize(4);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

  looper1.Init(loop_buf_1);
  looper2.Init(loop_buf_2);

  led1.Init(hw.seed.GetPin(Hothouse::LED_1), false);
  led2.Init(hw.seed.GetPin(Hothouse::LED_2), false);

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  uint32_t blink_counter = 0;

  while (true) {
    hw.DelayMs(10);
    blink_counter++;

    // LED 1: blink while recording, solid when loop captured, off otherwise
    if (looper1.recording) {
      led1.Set((blink_counter % 10 < 5) ? 1.0f : 0.0f);
    } else {
      led1.Set(looper1.has_loop ? 1.0f : 0.0f);
    }
    led1.Update();

    // LED 2: same for looper 2
    if (looper2.recording) {
      led2.Set((blink_counter % 10 < 5) ? 1.0f : 0.0f);
    } else {
      led2.Set(looper2.has_loop ? 1.0f : 0.0f);
    }
    led2.Update();
  }

  return 0;
}
