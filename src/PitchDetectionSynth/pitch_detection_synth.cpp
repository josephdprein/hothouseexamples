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

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace clevelandmusicco;
using namespace daisysp;
using namespace daisy;

namespace q = cycfi::q;
using namespace q::literals;

// 2000ms at 48kHz
static constexpr size_t MAX_DELAY_SAMPLES = 96000;
// Reverse mode reads at offset 1 + 2*(chunk_size-1) from write_pos,
// requiring a buffer of at least 2 * MAX_DELAY_SAMPLES.
static constexpr size_t DELAY_BUF_SIZE = 2 * MAX_DELAY_SAMPLES;
// Target crossfade at reverse chunk boundaries (~5ms); shortened
// automatically when the chunk is smaller than 2 * XFADE_SAMPLES.
static constexpr size_t XFADE_SAMPLES = 256;
// Minimum useful delay (1ms)
static constexpr size_t MIN_DELAY_SAMPLES = 48;
// Knob must move this far (normalized 0–1) from its entry position before
// the latched value updates. Prevents jumps on bank switch.
static constexpr float PICKUP_THRESHOLD = 0.03f;

static constexpr int NUM_KNOBS = 6;
static constexpr int NUM_DELAYS = 2;
static constexpr int NUM_LFOS = 3;

// Bank indices
enum Bank { BANK_A = 0, BANK_B, BANK_C, BANK_D, NUM_BANKS };

Hothouse hw;

// ----- Knob pickup (soft takeover) -----
// When switching banks, knobs keep their physical position but may not
// match the latched values for the new bank. Pickup tracks each knob's
// position at the moment the bank was entered and only allows updates
// once the knob has been moved from that position.

struct KnobPickup {
  float entry_pos[NUM_KNOBS]; // normalized knob position when bank entered
  bool picked_up[NUM_KNOBS];  // true once knob has moved past threshold

  void OnBankEnter() {
    for (int i = 0; i < NUM_KNOBS; i++) {
      entry_pos[i] = hw.knobs[i].Value();
      picked_up[i] = false;
    }
  }

  // Returns true if the knob has moved enough to start updating
  bool Moved(int idx) {
    if (picked_up[idx])
      return true;
    if (std::abs(hw.knobs[idx].Value() - entry_pos[idx]) > PICKUP_THRESHOLD) {
      picked_up[idx] = true;
      return true;
    }
    return false;
  }
};

KnobPickup pickup[NUM_BANKS];

// ----- Bidirectional delay line (forward + chunk-based reverse) -----

float DSY_SDRAM_BSS delay_buf_1[DELAY_BUF_SIZE];
float DSY_SDRAM_BSS delay_buf_2[DELAY_BUF_SIZE];

struct BiDirDelay {
  float *buffer;
  size_t write_pos;
  size_t rev_phase;
  size_t prev_chunk_size;
  size_t freeze_phase;

  void Init(float *buf) {
    buffer = buf;
    memset(buffer, 0, sizeof(float) * DELAY_BUF_SIZE);
    write_pos = 0;
    rev_phase = 0;
    prev_chunk_size = 0;
    freeze_phase = 0;
  }

  float Process(float in, float delay_knob, float feedback, bool frozen) {
    float centered = (delay_knob - 0.5f) * 2.0f;
    bool reversed = centered < 0.0f;
    float delay_frac = std::abs(centered);
    size_t delay_samples =
        static_cast<size_t>(delay_frac * MAX_DELAY_SAMPLES);

    if (delay_samples < MIN_DELAY_SAMPLES) {
      if (!frozen) {
        buffer[write_pos] = in;
        write_pos = (write_pos + 1) % DELAY_BUF_SIZE;
      }
      rev_phase = 0;
      freeze_phase = 0;
      return 0.0f;
    }

    if (delay_samples > MAX_DELAY_SAMPLES)
      delay_samples = MAX_DELAY_SAMPLES;

    float out = 0.0f;

    if (frozen) {
      // Loop over the last delay_samples of buffer content
      size_t base =
          (write_pos + DELAY_BUF_SIZE - delay_samples) % DELAY_BUF_SIZE;
      size_t read_pos;
      if (!reversed) {
        read_pos = (base + freeze_phase) % DELAY_BUF_SIZE;
      } else {
        read_pos =
            (base + delay_samples - 1 - freeze_phase) % DELAY_BUF_SIZE;
      }
      out = buffer[read_pos];

      // Crossfade end of loop into beginning for a seamless wrap
      size_t fade_len = std::min(XFADE_SAMPLES, delay_samples / 2);
      if (fade_len > 0 && freeze_phase >= delay_samples - fade_len) {
        float alpha = static_cast<float>(delay_samples - 1 - freeze_phase) /
                      static_cast<float>(fade_len);
        size_t wrap_phase = freeze_phase - (delay_samples - fade_len);
        size_t wrap_pos;
        if (!reversed)
          wrap_pos = (base + wrap_phase) % DELAY_BUF_SIZE;
        else
          wrap_pos =
              (base + delay_samples - 1 - wrap_phase) % DELAY_BUF_SIZE;
        out = out * alpha + buffer[wrap_pos] * (1.0f - alpha);
      }

      freeze_phase = (freeze_phase + 1) % delay_samples;
      return out;
    }

    // Not frozen — normal processing
    freeze_phase = 0;

    if (!reversed) {
      size_t read_pos =
          (write_pos + DELAY_BUF_SIZE - delay_samples) % DELAY_BUF_SIZE;
      out = buffer[read_pos];
      rev_phase = 0;
    } else {
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

      // Shorten crossfade when the chunk is too small for the full window,
      // preventing unsigned underflow in the fade-out comparison.
      size_t fade_len = std::min(XFADE_SAMPLES, chunk_size / 2);
      if (fade_len > 0) {
        if (rev_phase < fade_len) {
          out *= static_cast<float>(rev_phase) / static_cast<float>(fade_len);
        } else if (rev_phase >= chunk_size - fade_len) {
          out *= static_cast<float>(chunk_size - 1 - rev_phase) /
                 static_cast<float>(fade_len);
        }
      }

      rev_phase++;
    }

    float fb = std::clamp(feedback, 0.0f, 0.95f);
    buffer[write_pos] = in + out * fb;
    write_pos = (write_pos + 1) % DELAY_BUF_SIZE;

    return out;
  }
};

// ----- Global objects -----

Oscillator osc;
MoogLadder flt;
Adsr env;

BiDirDelay delays[NUM_DELAYS];

// Reverb (SDRAM for internal buffers)
ReverbSc DSY_SDRAM_BSS reverb;

// 3 LFOs: filter cutoff, delay 1 time, delay 2 time
Oscillator lfos[NUM_LFOS];

// Modulation on/off (footswitch 1)
bool mod_active = false;
Led led_mod;

// Effect bypass (footswitch 2)
bool effect_active = true;
Led led_bypass;

// ----- Latched parameter values (persist across bank switches) -----

struct {
  float cutoff = 1000.0f;
  float res = 0.7f;
  float attack = 0.01f;
  float decay = 0.1f;
  float sustain = 0.8f;
  float release = 0.3f;
} synth;

struct {
  float sensitivity = 0.02f;
  float drywet = 1.0f;
  float rev_send = 0.0f;
  float rev_decay = 0.85f;
  float rev_tone = 10000.0f;
  float gain = 0.5f;
} detect;

struct {
  float time[NUM_DELAYS] = {0.5f, 0.5f};
  float vol[NUM_DELAYS] = {0.5f, 0.5f};
  float fb[NUM_DELAYS] = {0.3f, 0.3f};
} dly;

struct {
  float rate[NUM_LFOS] = {1.0f, 1.0f, 1.0f};
  float depth[NUM_LFOS] = {0.0f, 0.0f, 0.0f};
} lfo;

// Knob parameters — Bank A (synth)
Parameter p_cutoff, p_res, p_attack, p_decay, p_sustain, p_release;

// Knob parameters — Bank B (detection/mix/reverb/gain)
Parameter p_sensitivity, p_drywet, p_rev_send, p_rev_decay, p_rev_tone, p_gain;

// Knob parameters — Bank C (delays: 3 knobs per delay line)
Parameter p_d_time[NUM_DELAYS], p_d_vol[NUM_DELAYS], p_d_fb[NUM_DELAYS];

// Knob parameters — Bank D (LFOs: rate on top row, depth on bottom)
Parameter p_lfo_rate[NUM_LFOS], p_lfo_depth[NUM_LFOS];

// Pitch detection
q::pitch_detector *pd = nullptr;

// Envelope follower for onset/offset detection
q::ar_envelope_follower *input_env = nullptr;

// Synth state
float detected_freq = 0.0f;
bool gate_open = false;

constexpr int waveforms[] = {
    Oscillator::WAVE_SIN,
    Oscillator::WAVE_POLYBLEP_SQUARE,
    Oscillator::WAVE_POLYBLEP_SAW,
};

// Determine which bank is active from two toggle switches
//   SW2 UP   + SW3 UP   = A
//   SW2 DOWN + SW3 UP   = B
//   SW2 UP   + SW3 DOWN = C
//   SW2 DOWN + SW3 DOWN = D
// MIDDLE on either switch is treated as UP.
static Bank GetBank() {
  bool sw2_down = (hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2) ==
                   Hothouse::TOGGLESWITCH_DOWN);
  bool sw3_down = (hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3) ==
                   Hothouse::TOGGLESWITCH_DOWN);
  if (!sw2_down && !sw3_down)
    return BANK_A;
  if (sw2_down && !sw3_down)
    return BANK_B;
  if (!sw2_down && sw3_down)
    return BANK_C;
  return BANK_D;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  hw.ProcessAllControls();

  // Footswitch 1: short press = toggle mod, hold = freeze delays
  static constexpr float HOLD_THRESHOLD_MS = 150.0f;
  static float fs1_hold_ms = 0.0f;
  float block_ms =
      1000.0f * static_cast<float>(size) / hw.AudioSampleRate();

  if (hw.switches[Hothouse::FOOTSWITCH_1].Pressed())
    fs1_hold_ms += block_ms;

  bool freeze = hw.switches[Hothouse::FOOTSWITCH_1].Pressed() &&
                fs1_hold_ms > HOLD_THRESHOLD_MS;

  if (hw.switches[Hothouse::FOOTSWITCH_1].FallingEdge()) {
    if (fs1_hold_ms <= HOLD_THRESHOLD_MS)
      mod_active = !mod_active;
    fs1_hold_ms = 0.0f;
  }

  // Footswitch 2: toggle effect bypass
  effect_active ^= hw.switches[Hothouse::FOOTSWITCH_2].RisingEdge();

  // When bypassed, pass dry input + reverb tail (delays cut immediately)
  if (!effect_active) {
    for (size_t i = 0; i < size; i++) {
      float rev_l = 0.0f, rev_r = 0.0f;
      reverb.Process(0.0f, 0.0f, &rev_l, &rev_r);
      out[0][i] = out[1][i] = std::clamp(in[0][i] + rev_l, -1.0f, 1.0f);
    }
    return;
  }

  // Bank selection via switch 2 + switch 3 combo
  Bank bank = GetBank();

  // Detect bank change and snapshot knob positions for pickup
  static Bank prev_bank = BANK_A;
  if (bank != prev_bank) {
    pickup[bank].OnBankEnter();
    prev_bank = bank;
  }

  KnobPickup &pk = pickup[bank];

  switch (bank) {
  case BANK_A:
    if (pk.Moved(0)) synth.cutoff = p_cutoff.Process();
    if (pk.Moved(1)) synth.res = p_res.Process();
    if (pk.Moved(2)) synth.attack = p_attack.Process();
    if (pk.Moved(3)) synth.decay = p_decay.Process();
    if (pk.Moved(4)) synth.sustain = p_sustain.Process();
    if (pk.Moved(5)) synth.release = p_release.Process();
    break;
  case BANK_B:
    if (pk.Moved(0)) detect.sensitivity = p_sensitivity.Process();
    if (pk.Moved(1)) detect.gain = p_gain.Process();
    if (pk.Moved(2)) detect.drywet = p_drywet.Process();
    if (pk.Moved(3)) detect.rev_send = p_rev_send.Process();
    if (pk.Moved(4)) detect.rev_decay = p_rev_decay.Process();
    if (pk.Moved(5)) detect.rev_tone = p_rev_tone.Process();
    break;
  case BANK_C:
    for (int j = 0; j < NUM_DELAYS; j++) {
      if (pk.Moved(j * 3 + 0)) dly.time[j] = p_d_time[j].Process();
      if (pk.Moved(j * 3 + 1)) dly.vol[j] = p_d_vol[j].Process();
      if (pk.Moved(j * 3 + 2)) dly.fb[j] = p_d_fb[j].Process();
    }
    break;
  case BANK_D:
    for (int j = 0; j < NUM_LFOS; j++) {
      if (pk.Moved(j)) lfo.rate[j] = p_lfo_rate[j].Process();
      if (pk.Moved(j + 3)) lfo.depth[j] = p_lfo_depth[j].Process();
    }
    break;
  default:
    break;
  }

  // Apply ADSR parameters
  env.SetAttackTime(synth.attack);
  env.SetDecayTime(synth.decay);
  env.SetSustainLevel(synth.sustain);
  env.SetReleaseTime(synth.release);

  // Toggle 1: waveform select
  osc.SetWaveform(
      waveforms[hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1)]);

  // Update LFO rates
  for (int j = 0; j < NUM_LFOS; j++)
    lfos[j].SetFreq(lfo.rate[j]);

  // Update reverb parameters
  reverb.SetFeedback(detect.rev_decay);
  reverb.SetLpFreq(detect.rev_tone);

  // Onset threshold with hysteresis
  float onset_thresh = detect.sensitivity;
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

    // Pitch detection
    (*pd)(input);
    float freq = pd->get_frequency();
    if (freq > 0.0f) {
      detected_freq = freq;
      osc.SetFreq(detected_freq);
    }

    // ADSR
    float env_out = env.Process(gate_open);

    // Process LFOs (always running so phase stays continuous)
    float lfo_val[NUM_LFOS];
    for (int j = 0; j < NUM_LFOS; j++)
      lfo_val[j] = lfos[j].Process();

    // Filter cutoff: envelope modulation + optional LFO 1
    float cutoff_mod = synth.cutoff * env_out;
    if (mod_active) {
      cutoff_mod *= (1.0f + lfo_val[0] * lfo.depth[0]);
    }
    flt.SetFreq(std::max(cutoff_mod, 20.0f));
    flt.SetRes(synth.res);

    // Synth output
    float synth_out = flt.Process(osc.Process()) * env_out;

    // Dry/wet mix
    float mixed = input * (1.0f - detect.drywet) + synth_out * detect.drywet;

    // Gain stage: knob at noon (0.5) = unity, CCW = attenuate, CW = tanh overdrive
    float g = detect.gain;
    if (g <= 0.5f) {
      mixed *= g * 2.0f;
    } else {
      float drive = 1.0f + (g - 0.5f) * 18.0f;
      float x = mixed * drive;
      mixed = x / (1.0f + std::abs(x)); // fast tanh approx
    }

    // Two parallel delay lines fed from post-mix signal
    // LFO 2 and 3 optionally modulate delay 1 and 2 times
    float d_out[NUM_DELAYS];
    for (int j = 0; j < NUM_DELAYS; j++) {
      float dt = dly.time[j];
      if (mod_active)
        dt = std::clamp(dt + lfo_val[j + 1] * lfo.depth[j + 1] * 0.5f,
                        0.0f, 1.0f);
      d_out[j] = delays[j].Process(mixed, dt, dly.fb[j], freeze);
    }

    float pre_verb = mixed;
    for (int j = 0; j < NUM_DELAYS; j++)
      pre_verb += d_out[j] * dly.vol[j];

    // Reverb (stereo in, stereo out — we feed mono and take one channel)
    float rev_out_l = 0.0f, rev_out_r = 0.0f;
    float rev_in = pre_verb * detect.rev_send;
    reverb.Process(rev_in, rev_in, &rev_out_l, &rev_out_r);

    float final_out = pre_verb + rev_out_l;

    out[0][i] = out[1][i] = std::clamp(final_out, -1.0f, 1.0f);
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

  // Bank B: detection/gain/mix/reverb parameters
  p_sensitivity.Init(hw.knobs[Hothouse::KNOB_1], 0.001f, 0.1f,
                     Parameter::LOGARITHMIC);
  p_gain.Init(hw.knobs[Hothouse::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
  p_drywet.Init(hw.knobs[Hothouse::KNOB_3], 0.0f, 1.0f, Parameter::LINEAR);
  p_rev_send.Init(hw.knobs[Hothouse::KNOB_4], 0.0f, 1.0f, Parameter::LINEAR);
  p_rev_decay.Init(hw.knobs[Hothouse::KNOB_5], 0.3f, 0.999f,
                   Parameter::LOGARITHMIC);
  p_rev_tone.Init(hw.knobs[Hothouse::KNOB_6], 500.0f, 16000.0f,
                  Parameter::LOGARITHMIC);

  // Bank C: delay parameters (3 knobs per delay line)
  p_d_time[0].Init(hw.knobs[Hothouse::KNOB_1], 0.0f, 1.0f, Parameter::LINEAR);
  p_d_vol[0].Init(hw.knobs[Hothouse::KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
  p_d_fb[0].Init(hw.knobs[Hothouse::KNOB_3], 0.0f, 0.95f, Parameter::LINEAR);
  p_d_time[1].Init(hw.knobs[Hothouse::KNOB_4], 0.0f, 1.0f, Parameter::LINEAR);
  p_d_vol[1].Init(hw.knobs[Hothouse::KNOB_5], 0.0f, 1.0f, Parameter::LINEAR);
  p_d_fb[1].Init(hw.knobs[Hothouse::KNOB_6], 0.0f, 0.95f, Parameter::LINEAR);

  // Bank D: LFO parameters (rate on top row, depth on bottom)
  for (int j = 0; j < NUM_LFOS; j++) {
    p_lfo_rate[j].Init(hw.knobs[Hothouse::KNOB_1 + j], 0.05f, 20.0f,
                       Parameter::LOGARITHMIC);
    p_lfo_depth[j].Init(hw.knobs[Hothouse::KNOB_4 + j], 0.0f, 1.0f,
                        Parameter::LINEAR);
  }

  // Pitch detector: guitar range ~80Hz (low E) to ~1200Hz (high frets)
  static q::pitch_detector pitch_det{80_Hz, 1200_Hz, sr, -30_dB};
  pd = &pitch_det;

  // Envelope follower: fast attack for onset detection, moderate release
  static q::ar_envelope_follower env_fol{1_ms, 100_ms, sr};
  input_env = &env_fol;

  // Synth oscillator
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

  // LFO oscillators (sine wave)
  for (int j = 0; j < NUM_LFOS; j++) {
    lfos[j].Init(sr);
    lfos[j].SetWaveform(Oscillator::WAVE_SIN);
    lfos[j].SetFreq(1.0f);
    lfos[j].SetAmp(1.0f);
  }

  // Delay lines
  float *delay_bufs[] = {delay_buf_1, delay_buf_2};
  for (int j = 0; j < NUM_DELAYS; j++)
    delays[j].Init(delay_bufs[j]);

  // Reverb
  reverb.Init(sr);
  reverb.SetFeedback(0.85f);
  reverb.SetLpFreq(10000.0f);

  // Initialize pickup state — mark all knobs as picked up for starting bank
  for (int b = 0; b < NUM_BANKS; b++) {
    for (int k = 0; k < NUM_KNOBS; k++) {
      pickup[b].picked_up[k] = (b == BANK_A); // only bank A starts active
      pickup[b].entry_pos[k] = 0.0f;
    }
  }

  // Modulation LED (LED 1 / left footswitch LED)
  led_mod.Init(hw.seed.GetPin(Hothouse::LED_1), false);

  // Bypass LED (LED 2 / right footswitch LED)
  led_bypass.Init(hw.seed.GetPin(Hothouse::LED_2), false);

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  while (true) {
    led_mod.Set(mod_active ? 1.0f : 0.0f);
    led_mod.Update();
    led_bypass.Set(effect_active ? 1.0f : 0.0f);
    led_bypass.Update();

    hw.DelayMs(10);
    hw.CheckResetToBootloader();
  }
}
