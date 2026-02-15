// GuitarMidiPitchDetector for Hothouse DIY DSP Platform
// Polyphonic pitch detection from guitar audio to MIDI over USB
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

#include "daisysp.h"
#include "hothouse.h"

#include <cmath>
#include <cstring>

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::MidiUsbHandler;
using daisy::SaiHandle;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static constexpr size_t FFT_SIZE = 2048;
static constexpr size_t FFT_HALF = FFT_SIZE / 2;
static constexpr size_t HOP_SIZE = 512;  // 75% overlap
static constexpr float  SAMPLE_RATE_F = 48000.0f;
static constexpr float  BIN_FREQ = SAMPLE_RATE_F / static_cast<float>(FFT_SIZE);

// Guitar MIDI range: E2 (40) to E6 (88) -- wide enough for harmonics
static constexpr uint8_t MIDI_NOTE_MIN = 40;   // E2 ~82 Hz
static constexpr uint8_t MIDI_NOTE_MAX = 96;   // C7 ~2093 Hz
static constexpr size_t  MAX_POLYPHONY = 12;

// Minimum bin index to consider (skip DC and very low frequencies)
// E2 = 82.4 Hz -> bin ~3.5 at 2048/48kHz
static constexpr size_t MIN_BIN = 3;
// Maximum bin index (C7 = 2093 Hz -> bin ~89)
static constexpr size_t MAX_BIN = 100;

// Hysteresis frames for note on/off
static constexpr int NOTE_ON_FRAMES = 2;
static constexpr int NOTE_OFF_FRAMES = 4;

// ---------------------------------------------------------------------------
// In-place radix-2 Cooley-Tukey FFT (float interleaved re/im)
// ---------------------------------------------------------------------------

static void fft_bit_reverse(float* data, size_t n) {
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (j > i) {
      float tr = data[2 * j];
      float ti = data[2 * j + 1];
      data[2 * j] = data[2 * i];
      data[2 * j + 1] = data[2 * i + 1];
      data[2 * i] = tr;
      data[2 * i + 1] = ti;
    }
    size_t m = n >> 1;
    while (m >= 1 && j >= m) {
      j -= m;
      m >>= 1;
    }
    j += m;
  }
}

static void fft_forward(float* data, size_t n) {
  fft_bit_reverse(data, n);
  for (size_t len = 2; len <= n; len <<= 1) {
    float angle = -2.0f * 3.14159265358979323846f / static_cast<float>(len);
    float wr_step = cosf(angle);
    float wi_step = sinf(angle);
    for (size_t i = 0; i < n; i += len) {
      float wr = 1.0f;
      float wi = 0.0f;
      for (size_t j = 0; j < len / 2; j++) {
        size_t a = i + j;
        size_t b = a + len / 2;
        float tr = wr * data[2 * b] - wi * data[2 * b + 1];
        float ti = wr * data[2 * b + 1] + wi * data[2 * b];
        data[2 * b] = data[2 * a] - tr;
        data[2 * b + 1] = data[2 * a + 1] - ti;
        data[2 * a] += tr;
        data[2 * a + 1] += ti;
        float new_wr = wr * wr_step - wi * wi_step;
        wi = wr * wi_step + wi * wr_step;
        wr = new_wr;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Frequency <-> MIDI note conversion
// ---------------------------------------------------------------------------

static float midi_to_freq(uint8_t note) {
  return 440.0f * powf(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
}

static float freq_to_midi_f(float freq) {
  if (freq <= 0.0f) return 0.0f;
  return 69.0f + 12.0f * log2f(freq / 440.0f);
}

// ---------------------------------------------------------------------------
// Parabolic interpolation for more accurate peak frequency
// ---------------------------------------------------------------------------

static float parabolic_interp(float ym1, float y0, float yp1) {
  float denom = ym1 - 2.0f * y0 + yp1;
  if (fabsf(denom) < 1e-12f) return 0.0f;
  return 0.5f * (ym1 - yp1) / denom;
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

Hothouse hw;
MidiUsbHandler midi;

// Audio input ring buffer for FFT
static float input_buffer[FFT_SIZE];
static size_t input_write_pos = 0;
static size_t samples_since_last_fft = 0;
static volatile bool fft_ready = false;

// FFT working buffers
static float fft_data[FFT_SIZE * 2];  // interleaved complex
static float window[FFT_SIZE];
static float magnitude[FFT_HALF];

// Detected peaks
struct Peak {
  float freq;
  float mag;
  uint8_t midi_note;
};
static Peak detected_peaks[MAX_POLYPHONY];
static size_t num_peaks = 0;

// Note tracking
struct NoteState {
  bool active;
  int on_counter;   // frames peak has been present
  int off_counter;  // frames peak has been absent
  uint8_t velocity;
};
static NoteState note_states[128];

// Active notes for MIDI output (managed from main loop)
static bool midi_notes_on[128];

// New notes to send from main loop
static volatile bool new_detection_ready = false;
static bool detected_notes[128];
static uint8_t detected_velocities[128];

// Controls
static float threshold = 0.01f;
static float velocity_scale = 1.0f;
static float note_lo = MIDI_NOTE_MIN;
static float note_hi = MIDI_NOTE_MAX;
static int sensitivity_mode = 1;  // 0=low, 1=mid, 2=high

// Bypass
Led led_bypass, led_activity;
volatile bool bypass = true;

// ---------------------------------------------------------------------------
// Pre-compute Hanning window
// ---------------------------------------------------------------------------

static void init_window() {
  for (size_t i = 0; i < FFT_SIZE; i++) {
    window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979323846f *
                                     static_cast<float>(i) /
                                     static_cast<float>(FFT_SIZE - 1)));
  }
}

// ---------------------------------------------------------------------------
// Find spectral peaks using local maximum detection
// ---------------------------------------------------------------------------

static void find_peaks(float thresh_mag) {
  num_peaks = 0;

  for (size_t i = MIN_BIN; i < MAX_BIN && i < FFT_HALF - 1; i++) {
    if (magnitude[i] < thresh_mag) continue;

    // Local maximum: greater than both neighbors
    if (magnitude[i] > magnitude[i - 1] && magnitude[i] > magnitude[i + 1]) {
      // Parabolic interpolation for sub-bin accuracy
      float delta = parabolic_interp(
          magnitude[i - 1], magnitude[i], magnitude[i + 1]);
      float peak_bin = static_cast<float>(i) + delta;
      float peak_freq = peak_bin * BIN_FREQ;
      float midi_f = freq_to_midi_f(peak_freq);

      // Round to nearest MIDI note
      int midi_note = static_cast<int>(midi_f + 0.5f);
      if (midi_note < MIDI_NOTE_MIN || midi_note > MIDI_NOTE_MAX) continue;

      // Check if close enough to a real MIDI note (within ~40 cents)
      float note_freq = midi_to_freq(static_cast<uint8_t>(midi_note));
      float cents = 1200.0f * log2f(peak_freq / note_freq);
      if (fabsf(cents) > 40.0f) continue;

      // Check for duplicate note (keep the louder one)
      bool duplicate = false;
      for (size_t p = 0; p < num_peaks; p++) {
        if (detected_peaks[p].midi_note == static_cast<uint8_t>(midi_note)) {
          if (magnitude[i] > detected_peaks[p].mag) {
            detected_peaks[p].freq = peak_freq;
            detected_peaks[p].mag = magnitude[i];
          }
          duplicate = true;
          break;
        }
      }

      if (!duplicate && num_peaks < MAX_POLYPHONY) {
        detected_peaks[num_peaks].freq = peak_freq;
        detected_peaks[num_peaks].mag = magnitude[i];
        detected_peaks[num_peaks].midi_note = static_cast<uint8_t>(midi_note);
        num_peaks++;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Process FFT and update note states
// ---------------------------------------------------------------------------

static void process_fft() {
  // Copy input buffer into FFT buffer with windowing
  for (size_t i = 0; i < FFT_SIZE; i++) {
    size_t idx = (input_write_pos + i) % FFT_SIZE;
    fft_data[2 * i] = input_buffer[idx] * window[i];
    fft_data[2 * i + 1] = 0.0f;
  }

  // Compute FFT
  fft_forward(fft_data, FFT_SIZE);

  // Compute magnitude spectrum
  float max_mag = 0.0f;
  for (size_t i = 0; i < FFT_HALF; i++) {
    float re = fft_data[2 * i];
    float im = fft_data[2 * i + 1];
    magnitude[i] = sqrtf(re * re + im * im);
    if (magnitude[i] > max_mag) max_mag = magnitude[i];
  }

  // Adaptive threshold based on signal level and sensitivity mode
  float sens_mult = (sensitivity_mode == 2) ? 0.5f
                  : (sensitivity_mode == 0) ? 2.0f
                  : 1.0f;
  float thresh_mag = threshold * max_mag * sens_mult;
  if (thresh_mag < 0.001f) thresh_mag = 0.001f;

  // Find peaks
  find_peaks(thresh_mag);

  // Build set of currently detected notes
  memset(detected_notes, 0, sizeof(detected_notes));
  memset(detected_velocities, 0, sizeof(detected_velocities));

  for (size_t p = 0; p < num_peaks; p++) {
    uint8_t note = detected_peaks[p].midi_note;
    // Map magnitude to velocity (1-127)
    float vel_f = (detected_peaks[p].mag / (max_mag + 1e-10f)) * 127.0f *
                  velocity_scale;
    uint8_t vel = static_cast<uint8_t>(
        fminf(127.0f, fmaxf(1.0f, vel_f)));
    detected_notes[note] = true;
    if (vel > detected_velocities[note]) {
      detected_velocities[note] = vel;
    }
  }

  // Update note states with hysteresis
  for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; n++) {
    if (static_cast<float>(n) < note_lo || static_cast<float>(n) > note_hi) {
      // Outside configured range: treat as absent
      detected_notes[n] = false;
    }

    if (detected_notes[n]) {
      note_states[n].off_counter = 0;
      note_states[n].on_counter++;
      if (detected_velocities[n] > note_states[n].velocity) {
        note_states[n].velocity = detected_velocities[n];
      }
      if (note_states[n].on_counter >= NOTE_ON_FRAMES && !note_states[n].active) {
        note_states[n].active = true;
      }
    } else {
      note_states[n].on_counter = 0;
      note_states[n].off_counter++;
      if (note_states[n].off_counter >= NOTE_OFF_FRAMES && note_states[n].active) {
        note_states[n].active = false;
        note_states[n].velocity = 0;
      }
    }
  }

  new_detection_ready = true;
}

// ---------------------------------------------------------------------------
// Audio callback - collects samples and triggers FFT
// ---------------------------------------------------------------------------

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  hw.ProcessAllControls();

  // Toggle bypass with FOOTSWITCH_2
  bypass ^= hw.switches[Hothouse::FOOTSWITCH_2].RisingEdge();

  // Read controls
  threshold = 0.005f + hw.GetKnobValue(Hothouse::KNOB_1) * 0.495f;
  velocity_scale = 0.2f + hw.GetKnobValue(Hothouse::KNOB_2) * 1.8f;

  // Knob 3: attack sensitivity (fewer frames = faster response)
  // Knob 4: release time
  // (These are read but the hysteresis constants are compile-time for
  //  real-time safety; the knobs adjust threshold behavior instead)

  // Knob 5/6: note range filter
  note_lo = MIDI_NOTE_MIN +
            hw.GetKnobValue(Hothouse::KNOB_5) *
                static_cast<float>(MIDI_NOTE_MAX - MIDI_NOTE_MIN);
  note_hi = MIDI_NOTE_MIN +
            hw.GetKnobValue(Hothouse::KNOB_6) *
                static_cast<float>(MIDI_NOTE_MAX - MIDI_NOTE_MIN);
  if (note_hi < note_lo) {
    float tmp = note_lo;
    note_lo = note_hi;
    note_hi = tmp;
  }

  // Toggle switch 1: sensitivity preset
  switch (hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1)) {
    case Hothouse::TOGGLESWITCH_UP:
      sensitivity_mode = 2;  // high sensitivity (more notes, more partials)
      break;
    case Hothouse::TOGGLESWITCH_DOWN:
      sensitivity_mode = 0;  // low sensitivity (fewer, cleaner notes)
      break;
    default:
      sensitivity_mode = 1;  // medium
      break;
  }

  for (size_t i = 0; i < size; i++) {
    // Pass audio through (guitar signal is unmodified)
    out[0][i] = in[0][i];
    out[1][i] = in[0][i];

    if (!bypass) {
      // Store sample in ring buffer
      input_buffer[input_write_pos] = in[0][i];
      input_write_pos = (input_write_pos + 1) % FFT_SIZE;
      samples_since_last_fft++;

      if (samples_since_last_fft >= HOP_SIZE) {
        samples_since_last_fft = 0;
        fft_ready = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Send MIDI messages
// ---------------------------------------------------------------------------

static void send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
  uint8_t msg[3] = {
      static_cast<uint8_t>(0x90 | (channel & 0x0F)), note, velocity};
  midi.SendMessage(msg, 3);
}

static void send_note_off(uint8_t channel, uint8_t note) {
  uint8_t msg[3] = {static_cast<uint8_t>(0x80 | (channel & 0x0F)), note, 0};
  midi.SendMessage(msg, 3);
}

static void send_all_notes_off(uint8_t channel) {
  for (int n = 0; n < 128; n++) {
    if (midi_notes_on[n]) {
      send_note_off(channel, static_cast<uint8_t>(n));
      midi_notes_on[n] = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
  hw.Init();
  hw.SetAudioBlockSize(48);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

  // Initialize LEDs
  led_bypass.Init(hw.seed.GetPin(Hothouse::LED_2), false);
  led_activity.Init(hw.seed.GetPin(Hothouse::LED_1), false);

  // Initialize MIDI USB
  MidiUsbHandler::Config midi_cfg;
  midi_cfg.transport_config.periph =
      daisy::MidiUsbTransport::Config::INTERNAL;
  midi.Init(midi_cfg);

  // Initialize window function
  init_window();

  // Clear state
  memset(input_buffer, 0, sizeof(input_buffer));
  memset(note_states, 0, sizeof(note_states));
  memset(midi_notes_on, 0, sizeof(midi_notes_on));
  memset(detected_notes, 0, sizeof(detected_notes));

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  uint8_t midi_channel = 0;
  bool was_bypassed = true;
  bool any_note_on = false;
  uint32_t activity_led_timer = 0;

  while (true) {
    // MIDI channel from toggle switch 2
    switch (hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2)) {
      case Hothouse::TOGGLESWITCH_UP:
        midi_channel = 0;
        break;
      case Hothouse::TOGGLESWITCH_DOWN:
        midi_channel = 2;
        break;
      default:
        midi_channel = 1;
        break;
    }

    // Process FFT if ready (runs in main loop, not audio callback)
    if (fft_ready && !bypass) {
      fft_ready = false;
      process_fft();
    }

    // Handle bypass transitions
    if (bypass && !was_bypassed) {
      // Just entered bypass - send all notes off
      send_all_notes_off(midi_channel);
      memset(note_states, 0, sizeof(note_states));
      new_detection_ready = false;
    }
    was_bypassed = bypass;

    // Send MIDI note on/off based on detection results
    if (new_detection_ready && !bypass) {
      new_detection_ready = false;
      any_note_on = false;

      for (int n = MIDI_NOTE_MIN; n <= MIDI_NOTE_MAX; n++) {
        bool should_be_on = note_states[n].active;

        if (should_be_on && !midi_notes_on[n]) {
          // Note on
          send_note_on(midi_channel, static_cast<uint8_t>(n),
                       note_states[n].velocity);
          midi_notes_on[n] = true;
        } else if (!should_be_on && midi_notes_on[n]) {
          // Note off
          send_note_off(midi_channel, static_cast<uint8_t>(n));
          midi_notes_on[n] = false;
        }

        if (midi_notes_on[n]) any_note_on = true;
      }

      if (any_note_on) {
        activity_led_timer = daisy::System::GetNow();
      }
    }

    // Listen for incoming MIDI (not used but keeps USB alive)
    midi.Listen();
    while (midi.HasEvents()) {
      midi.PopEvent();
    }

    // Update LEDs
    led_bypass.Set(bypass ? 0.0f : 1.0f);
    led_bypass.Update();

    // Activity LED: on when notes detected, with short persistence
    bool show_activity =
        any_note_on ||
        (daisy::System::GetNow() - activity_led_timer < 50);
    led_activity.Set((!bypass && show_activity) ? 1.0f : 0.0f);
    led_activity.Update();

    hw.DelayMs(1);
    hw.CheckResetToBootloader();
  }

  return 0;
}
