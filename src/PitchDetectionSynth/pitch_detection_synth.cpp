// PitchDetectionSynth for Hothouse DIY DSP Platform
// Copyright (C) 2024 Cleveland Music Co. <code@clevelandmusicco.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "daisysp-lgpl.h"
#include "daisysp.h"
#include "hothouse.h"

#include <q/pitch/pitch_detector.hpp>
#include <q/fx/envelope.hpp>
#include <q/support/literals.hpp>

#include <cmath>
#include <cstring>

using namespace clevelandmusicco;
using namespace daisysp;
using namespace daisy;

namespace q = cycfi::q;
using namespace q::literals;

// 2000ms at 48kHz
static constexpr size_t MAX_DELAY_SAMPLES = 96000;
// 2x buffer for reverse mode headroom (reverse reads up to 2*chunk behind
// write pos)
static constexpr size_t DELAY_BUF_SIZE = 192000;
// Crossfade at reverse chunk boundaries (~5ms)
static constexpr size_t XFADE_SAMPLES = 256;
// Minimum useful delay (1ms) — below this, output silence
static constexpr size_t MIN_DELAY_SAMPLES = 48;

// ----- Bidirectional delay line (forward + chunk-based reverse) -----
//
// Delay time knob behavior:
//   Fully CCW (0.0) = 2000ms reversed
//   Noon      (0.5) = 0ms (no delay)
//   Fully CW  (1.0) = 2000ms forward
//
// Reverse mode records chunks of audio equal to the delay time, then
// plays each chunk back reversed. Short crossfades at chunk boundaries
// prevent clicks.

// SDRAM buffers for the two delay lines
float DSY_SDRAM_BSS delay_buf_1[DELAY_BUF_SIZE];
float DSY_SDRAM_BSS delay_buf_2[DELAY_BUF_SIZE];

struct BiDirDelay {
  float *buffer;
  size_t write_pos;
  size_t rev_phase;
  size_t prev_chunk_size;

  void Init(float *buf) {
    buffer = buf;
    memset(buffer, 0, sizeof(float) * DELAY_BUF_SIZE);
    write_pos = 0;
    rev_phase = 0;
    prev_chunk_size = 0;
  }

  float Process(float in, float delay_knob, float feedback) {
    float centered = (delay_knob - 0.5f) * 2.0f; // -1.0 to 1.0
    bool reversed = centered < 0.0f;
    float delay_frac = std::abs(centered); // 0.0 to 1.0
    size_t delay_samples =
        static_cast<size_t>(delay_frac * MAX_DELAY_SAMPLES);

    // Below minimum: no delay output, just keep buffer fed
    if (delay_samples < MIN_DELAY_SAMPLES) {
      buffer[write_pos] = in;
      write_pos = (write_pos + 1) % DELAY_BUF_SIZE;
      rev_phase = 0;
      return 0.0f;
    }

    if (delay_samples > MAX_DELAY_SAMPLES)
      delay_samples = MAX_DELAY_SAMPLES;

    float out = 0.0f;

    if (!reversed) {
      // --- Forward delay: simple circular buffer read ---
      size_t read_pos =
          (write_pos + DELAY_BUF_SIZE - delay_samples) % DELAY_BUF_SIZE;
      out = buffer[read_pos];
      rev_phase = 0;
    } else {
      // --- Reverse delay: chunk-based playback ---
      // Accumulate chunk_size samples, then play that chunk backwards.
      // Read position: write_pos - 1 - 2*rev_phase
      // At phase 0 (chunk boundary) this reads the newest sample of the
      // previous chunk; at phase chunk_size-1 it reads the oldest.
      size_t chunk_size = delay_samples;

      if (chunk_size != prev_chunk_size) {
        rev_phase = rev_phase % chunk_size;
        prev_chunk_size = chunk_size;
      }

      if (rev_phase >= chunk_size) {
        rev_phase = 0;
      }

      size_t offset = 1 + 2 * rev_phase;
      size_t read_pos = (write_pos + DELAY_BUF_SIZE - offset) % DELAY_BUF_SIZE;
      out = buffer[read_pos];

      // Crossfade at chunk boundaries to reduce clicks
      if (rev_phase < XFADE_SAMPLES) {
        out *= static_cast<float>(rev_phase) / XFADE_SAMPLES;
      } else if (rev_phase >= chunk_size - XFADE_SAMPLES) {
        out *= static_cast<float>(chunk_size - 1 - rev_phase) / XFADE_SAMPLES;
      }

      rev_phase++;
    }

    // Clamp feedback to prevent runaway
    float fb = std::max(0.0f, std::min(feedback, 0.95f));
    buffer[write_pos] = in + out * fb;
    write_pos = (write_pos + 1) % DELAY_BUF_SIZE;

    return out;
  }
};

// ----- Global objects -----

Hothouse hw;
Oscillator osc;
MoogLadder flt;
Adsr env;

BiDirDelay delay1, delay2;

// Knob parameters — Bank A (synth)
Parameter p_cutoff, p_res, p_attack, p_decay, p_sustain, p_release;

// Knob parameters — Bank B (detection/mix)
Parameter p_sensitivity, p_drywet;

// Knob parameters — Bank C (delays)
Parameter p_d1_time, p_d1_vol, p_d1_fb;
Parameter p_d2_time, p_d2_vol, p_d2_fb;

// Pitch detection (allocated in main, guitar range 80Hz–1200Hz)
q::pitch_detector *pd = nullptr;

// Envelope follower for onset/offset detection
q::ar_envelope_follower *input_env = nullptr;

// Latched parameter values (persist across bank switches)
float latch_cutoff = 1000.0f;
float latch_res = 0.7f;
float latch_attack = 0.01f;
float latch_decay = 0.1f;
float latch_sustain = 0.8f;
float latch_release = 0.3f;

float latch_sensitivity = 0.02f;
float latch_drywet = 1.0f;

float latch_d1_time = 0.5f; // noon = no delay
float latch_d1_vol = 0.5f;
float latch_d1_fb = 0.3f;
float latch_d2_time = 0.5f;
float latch_d2_vol = 0.5f;
float latch_d2_fb = 0.3f;

// Synth state
float detected_freq = 0.0f;
bool gate_open = false;

const int waveforms[] = {
    Oscillator::WAVE_SIN,
    Oscillator::WAVE_POLYBLEP_SQUARE,
    Oscillator::WAVE_POLYBLEP_SAW,
};

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  hw.ProcessAllControls();

  // Toggle 2: knob bank select
  //   UP (0) = Bank A (synth)
  //   MIDDLE (1) = Bank B (detection/mix)
  //   DOWN (2) = Bank C (delays)
  int bank = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);

  switch (bank) {
  case 0: // Bank A — synth
    latch_cutoff = p_cutoff.Process();
    latch_res = p_res.Process();
    latch_attack = p_attack.Process();
    latch_decay = p_decay.Process();
    latch_sustain = p_sustain.Process();
    latch_release = p_release.Process();
    break;
  case 1: // Bank B — detection/mix
    latch_sensitivity = p_sensitivity.Process();
    latch_drywet = p_drywet.Process();
    break;
  case 2: // Bank C — delays
    latch_d1_time = p_d1_time.Process();
    latch_d1_vol = p_d1_vol.Process();
    latch_d1_fb = p_d1_fb.Process();
    latch_d2_time = p_d2_time.Process();
    latch_d2_vol = p_d2_vol.Process();
    latch_d2_fb = p_d2_fb.Process();
    break;
  }

  // Apply ADSR parameters
  env.SetAttackTime(latch_attack);
  env.SetDecayTime(latch_decay);
  env.SetSustainLevel(latch_sustain);
  env.SetReleaseTime(latch_release);

  // Toggle 1: waveform select
  osc.SetWaveform(
      waveforms[hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1)]);

  // Onset threshold with hysteresis
  float onset_thresh = latch_sensitivity;
  float release_thresh = onset_thresh * 0.3f;

  for (size_t i = 0; i < size; ++i) {
    float input = in[0][i];
    float abs_input = std::abs(input);

    // Track input envelope
    float env_level = (*input_env)(abs_input);

    // Gate detection with hysteresis
    if (!gate_open && env_level > onset_thresh) {
      gate_open = true;
    } else if (gate_open && env_level < release_thresh) {
      gate_open = false;
    }

    // Pitch detection — feed every sample
    (*pd)(input);
    float freq = pd->get_frequency();
    if (freq > 0.0f) {
      detected_freq = freq;
      osc.SetFreq(detected_freq);
    }

    // ADSR: gate=true while input is present, releases when signal drops
    float env_out = env.Process(gate_open);

    // Filter modulated by envelope
    float cutoff_mod = latch_cutoff * env_out;
    flt.SetFreq(cutoff_mod);
    flt.SetRes(latch_res);

    // Synth output
    float synth_out = flt.Process(osc.Process()) * env_out;

    // Dry/wet mix
    float mixed = input * (1.0f - latch_drywet) + synth_out * latch_drywet;

    // Two parallel delay lines fed from post-mix signal
    float d1_out = delay1.Process(mixed, latch_d1_time, latch_d1_fb);
    float d2_out = delay2.Process(mixed, latch_d2_time, latch_d2_fb);

    float final_out = mixed + d1_out * latch_d1_vol + d2_out * latch_d2_vol;

    out[0][i] = out[1][i] = final_out;
  }
}

int main() {
  hw.Init();
  hw.SetAudioBlockSize(48);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
  float sr = hw.AudioSampleRate();

  // Bank A: synth parameters
  p_cutoff.Init(hw.knobs[Hothouse::KNOB_1], 20.0f, 20000.0f,
                Parameter::LOGARITHMIC);
  p_res.Init(hw.knobs[Hothouse::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
  p_attack.Init(hw.knobs[Hothouse::KNOB_3], 0.001f, 0.5f,
                Parameter::LOGARITHMIC);
  p_decay.Init(hw.knobs[Hothouse::KNOB_4], 0.05f, 2.0f,
               Parameter::LOGARITHMIC);
  p_sustain.Init(hw.knobs[Hothouse::KNOB_5], 0.0f, 1.0f, Parameter::LINEAR);
  p_release.Init(hw.knobs[Hothouse::KNOB_6], 0.05f, 2.0f,
                 Parameter::LOGARITHMIC);

  // Bank B: detection/mix parameters
  p_sensitivity.Init(hw.knobs[Hothouse::KNOB_1], 0.001f, 0.1f,
                     Parameter::LOGARITHMIC);
  p_drywet.Init(hw.knobs[Hothouse::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);

  // Bank C: delay parameters (time knobs are raw 0–1, mapped in BiDirDelay)
  p_d1_time.Init(hw.knobs[Hothouse::KNOB_1], 0.0f, 1.0f, Parameter::LINEAR);
  p_d1_vol.Init(hw.knobs[Hothouse::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
  p_d1_fb.Init(hw.knobs[Hothouse::KNOB_3], 0.0f, 0.95f, Parameter::LINEAR);
  p_d2_time.Init(hw.knobs[Hothouse::KNOB_4], 0.0f, 1.0f, Parameter::LINEAR);
  p_d2_vol.Init(hw.knobs[Hothouse::KNOB_5], 0.0f, 1.0f, Parameter::LINEAR);
  p_d2_fb.Init(hw.knobs[Hothouse::KNOB_6], 0.0f, 0.95f, Parameter::LINEAR);

  // Pitch detector: guitar range ~80Hz (low E) to ~1200Hz (high frets)
  static q::pitch_detector pitch_det{80_Hz, 1200_Hz, sr, -30_dB};
  pd = &pitch_det;

  // Envelope follower: fast attack for onset detection, moderate release
  static q::ar_envelope_follower env_fol{1_ms, 100_ms, sr};
  input_env = &env_fol;

  // Oscillator
  osc.Init(sr);
  osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);

  // Moog ladder filter
  flt.Init(sr);
  flt.SetRes(0.7f);

  // ADSR envelope
  env.Init(sr);
  env.SetAttackTime(0.01f);
  env.SetDecayTime(0.1f);
  env.SetSustainLevel(0.8f);
  env.SetReleaseTime(0.3f);

  // Delay lines — buffers live in SDRAM
  delay1.Init(delay_buf_1);
  delay2.Init(delay_buf_2);

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  while (true) {
    hw.DelayMs(10);
    hw.CheckResetToBootloader();
  }
  return 0;
}
