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

using namespace clevelandmusicco;
using namespace daisysp;
using namespace daisy;

namespace q = cycfi::q;
using namespace q::literals;

Hothouse hw;
Oscillator osc;
MoogLadder flt;
Adsr env;

// Knob parameters - Bank A (synth)
Parameter p_cutoff, p_res, p_attack, p_decay, p_sustain, p_release;

// Knob parameters - Bank B (detection/mix)
Parameter p_sensitivity, p_drywet;

// Pitch detection (allocated in main, guitar range 80Hz-1200Hz)
q::pitch_detector* pd = nullptr;

// Envelope follower for onset/offset detection
q::ar_envelope_follower* input_env = nullptr;

// Latched parameter values (persist across bank switches)
float latch_cutoff = 1000.0f;
float latch_res = 0.7f;
float latch_attack = 0.01f;
float latch_decay = 0.1f;
float latch_sustain = 0.8f;
float latch_release = 0.3f;
float latch_sensitivity = 0.02f;
float latch_drywet = 1.0f;

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

  // Toggle 2: knob bank select (UP = Bank A synth, DOWN = Bank B detection)
  bool bank_a =
      (hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2) == 0);

  if (bank_a) {
    latch_cutoff = p_cutoff.Process();
    latch_res = p_res.Process();
    latch_attack = p_attack.Process();
    latch_decay = p_decay.Process();
    latch_sustain = p_sustain.Process();
    latch_release = p_release.Process();
  } else {
    latch_sensitivity = p_sensitivity.Process();
    latch_drywet = p_drywet.Process();
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

    // Pitch detection - feed every sample
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
    out[0][i] = out[1][i] = mixed;
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

  // Bank B: detection/mix parameters (same physical knobs, different mapping)
  p_sensitivity.Init(hw.knobs[Hothouse::KNOB_1], 0.001f, 0.1f,
                     Parameter::LOGARITHMIC);
  p_drywet.Init(hw.knobs[Hothouse::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);

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

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  while (true) {
    hw.DelayMs(10);
    hw.CheckResetToBootloader();
  }
  return 0;
}
