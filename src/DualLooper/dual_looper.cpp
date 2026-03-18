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
//   a) Playing            → stop playback (LED blinks 1×)
//   b) Stopped, has loop  → toggle loop ↔ one-shot mode (1× = one-shot, 2× = loop)
//   c) Recording, no loop → cancel recording (LED blinks 1×)
//
// Footswitch (hold ≥2 s, starting from stopped):
//   a) Stopped, has loop  → erase loop (LED blinks 3×)
//
// Both footswitches held ≥3 s → save both loops to QSPI flash.
//   Both LEDs solid during save.  Both LEDs blink 3× on completion.
//   On next power-up, saved loops are loaded automatically (both LEDs
//   double-flash to confirm).  Loops load in stopped state; press the
//   footswitch to start playback.
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
// QSPI flash layout (firmware runs from internal flash; full 8 MB free):
//   0x000000 – 0x000FFF  SaveHeader (4 KB)
//   0x001000 – 0x3FFFFF  Loop 1 audio, int16_t, up to ~43.7 s at 48 kHz
//   0x400000 – 0x7FEFFF  Loop 2 audio, int16_t, up to ~43.7 s at 48 kHz
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
using daisy::QSPIHandle;
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

// Hold thresholds
static constexpr uint32_t MODE_MS  = 1000;  // stop (if playing) or toggle loop/one-shot (if stopped)
static constexpr uint32_t ERASE_MS = 2000;  // erase (if hold started while stopped)
static constexpr uint32_t SAVE_HOLD_MS = 3000;  // both footswitches held → save

// Fade / splice constants
// FADE_RATE: one-pole coefficient for output and overdub envelopes.
//   τ ≈ 1 / (FADE_RATE × 48 000) ≈ 4 ms — fast enough to feel snappy,
//   slow enough to silence any click on every play/stop transition.
static constexpr float  FADE_RATE = 0.005f;
// XFADE_LEN: samples baked into each end of the loop buffer when a loop
//   is committed.  128 samples ≈ 2.7 ms — inaudible as a fade but enough
//   to guarantee silence at the splice point regardless of where the
//   musician pressed the footswitch.
static constexpr size_t XFADE_LEN = 128;

// ---------------------------------------------------------------------------
// QSPI flash layout constants
//
// Firmware runs from internal STM32H750 flash (no BOOT_QSPI), so the full
// 8 MB QSPI is available for audio storage.  There is no risk of overwriting
// firmware regardless of where we write in QSPI.
//
// Max saveable samples per loop:
//   Available bytes = 8 MB - 4 KB header = 8,384,512
//   Per loop (split evenly) = 4,192,256 bytes / 2 bytes per int16 = 2,096,128
//   At 48 kHz: 2,096,128 / 48,000 ≈ 43.67 seconds
// ---------------------------------------------------------------------------
static constexpr uint32_t QSPI_HEADER_OFFSET = 0x000000U;
static constexpr uint32_t QSPI_LOOP1_OFFSET  = 0x001000U;
static constexpr uint32_t QSPI_LOOP2_OFFSET  = 0x400000U;
static constexpr size_t   MAX_SAVE_SAMPLES   = 2096128;   // ~43.67 s at 48 kHz

static constexpr uint32_t SAVE_MAGIC   = 0xD00B100DU;
static constexpr uint32_t SAVE_VERSION = 1U;

struct SaveHeader {
  uint32_t magic;         // SAVE_MAGIC
  uint32_t version;       // SAVE_VERSION
  uint32_t loop1_length;  // samples saved for loop 1 (0 = no loop)
  uint32_t loop2_length;  // samples saved for loop 2 (0 = no loop)
  uint32_t checksum;      // magic ^ version ^ loop1_length ^ loop2_length
};

// Chunk buffer for float ↔ int16 conversion during save/load.
// Declared in BSS so it doesn't live on the stack.
static constexpr size_t SAVE_CHUNK_SIZE = 4096;
static int16_t io_chunk[SAVE_CHUNK_SIZE];

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
  bool loop_mode;  // true = loop continuously, false = one-shot

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

  // Smooth amplitude envelopes — prevent clicks on every state transition.
  // fade_gain    : output level, ramps 0→1 when playback starts, 1→0 when stopped.
  // overdub_gain : overdub-input mix level, ramps in/out so no transient is
  //               baked into the loop buffer when overdub is toggled.
  float fade_gain;
  float overdub_gain;

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
    loop_mode = true;
    speed = 0.0f;
    level = 1.0f;
    warble_depth = 0.0f;
    reverse = false;
    semitone_snap = false;
    rng_state = seed;
    noise_slow = 0.0f;
    noise_mid = 0.0f;
    noise_fast = 0.0f;
    fade_gain    = 0.0f;
    overdub_gain = 0.0f;
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
        // Bake a short linear fade into both ends of the buffer so the loop
        // splice point is silent regardless of where the footswitch was pressed.
        // Uses at most XFADE_LEN samples per end; capped at loop_length/2 so
        // the fade-in and fade-out regions never overlap on very short loops.
        size_t xfade = (XFADE_LEN < loop_length / 2) ? XFADE_LEN : loop_length / 2;
        for (size_t i = 0; i < xfade; ++i) {
          float t = static_cast<float>(i) / static_cast<float>(xfade);
          buffer[i]                   *= t;  // fade in  at loop start
          buffer[loop_length - 1 - i] *= t;  // fade out at loop end
        }
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

  // 1-second hold action
  void ModePress() {
    if (playing) {
      // Stop playback and any active overdub
      playing = false;
      recording = false;
      led_blink_count = 1;
    } else if (has_loop) {
      // Toggle loop ↔ one-shot; blink count confirms new mode
      loop_mode = !loop_mode;
      led_blink_count = loop_mode ? 2 : 1;  // 2× = loop, 1× = one-shot
    } else if (recording) {
      // Cancel recording in progress (no loop yet)
      recording = false;
      led_blink_count = 1;
    }
    // else: idle, nothing to do
  }

  // 2-second hold action (only fires when hold started from stopped state)
  void ErasePress() {
    if (!playing && (has_loop || recording)) {
      bool was_loop = has_loop;
      has_loop = false;
      recording = false;
      loop_length = 0;
      rec_pos = 0;
      read_pos = 0.0f;
      led_blink_count = was_loop ? 3 : 1;
    }
  }

  // --- Per-sample audio --------------------------------------------------

  float Process(float in) {
    // Initial recording — no loop exists yet.
    // Keep fade_gain at zero so output is silent throughout.
    if (recording && !has_loop) {
      if (rec_pos < MAX_LOOP_SAMPLES) {
        buffer[rec_pos++] = in;
      }
      fade_gain = 0.0f;
      return 0.0f;
    }

    if (!has_loop || loop_length < 2) {
      fade_gain = 0.0f;
      return 0.0f;
    }

    // --- Output amplitude envelope ----------------------------------------
    // Ramps smoothly toward 1.0 while playing and toward 0.0 while stopped.
    // This eliminates hard-cut pops on every play/stop/one-shot transition.
    float target_gain = playing ? 1.0f : 0.0f;
    fade_gain += FADE_RATE * (target_gain - fade_gain);

    // Once fully silent and not playing, skip all per-sample work.
    if (fade_gain < 0.001f && !playing) {
      overdub_gain = 0.0f;
      return 0.0f;
    }

    float flen = static_cast<float>(loop_length);

    // Normalise read position (read_pos is kept clean by the advance code,
    // but guard here in case of edge cases to avoid UB on size_t cast).
    float pos = read_pos;
    while (pos <  0.0f) pos += flen;
    while (pos >= flen) pos -= flen;

    // --- Random warble ----------------------------------------------------
    float effective_speed = speed;
    if (warble_depth > 0.001f && speed > 0.001f) {
      float raw = RandFloat(rng_state);
      noise_slow += OnePoleCoeff(1.0f)  * (raw - noise_slow);
      raw = RandFloat(rng_state);
      noise_mid  += OnePoleCoeff(4.0f)  * (raw - noise_mid);
      raw = RandFloat(rng_state);
      noise_fast += OnePoleCoeff(12.0f) * (raw - noise_fast);

      float wobble = 0.55f * noise_slow +
                     0.30f * noise_mid  +
                     0.15f * noise_fast;
      effective_speed = speed * (1.0f + warble_depth * wobble);
      if (effective_speed < 0.0f) effective_speed = 0.0f;
      // Cap upward excursion: at 4× the while-loop normalisation below stays
      // bounded, and aliasing from skipping samples stays inaudible.
      if (effective_speed > 4.0f) effective_speed = 4.0f;
    }

    // --- Overdub: mix input into buffer with a faded blend ----------------
    // Ramp overdub_gain in when overdub starts and out when it stops so no
    // sudden transient is baked permanently into the loop buffer.
    {
      float target_od = (recording && effective_speed > 0.001f) ? 1.0f : 0.0f;
      overdub_gain += FADE_RATE * (target_od - overdub_gain);
      if (overdub_gain > 0.001f) {
        size_t wr = static_cast<size_t>(pos) % loop_length;
        buffer[wr] = daisysp::SoftClip(buffer[wr] + in * overdub_gain);
      }
    }

    // --- Read with linear interpolation -----------------------------------
    size_t idx0 = static_cast<size_t>(pos);
    size_t idx1 = (idx0 + 1 < loop_length) ? idx0 + 1 : 0;
    float frac   = pos - static_cast<float>(idx0);
    float sample = buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;

    // --- Advance playback head (only while actually playing) --------------
    // Gating on `playing` means read_pos stays frozen during the fade-out
    // window after a stop, so the tail decays from a fixed position rather
    // than sweeping through the buffer while going quiet.
    if (playing && effective_speed > 0.001f) {
      float prev_pos = read_pos;
      if (reverse) {
        read_pos -= effective_speed;
        // Use while-loop so any effective_speed > flen (e.g., heavy warble
        // on a very short loop) still normalises correctly.
        while (read_pos < 0.0f) read_pos += flen;
      } else {
        read_pos += effective_speed;
        while (read_pos >= flen) read_pos -= flen;
      }

      // One-shot: stop automatically when the head wraps past the boundary.
      // fade_gain will decay to zero over the next ~4 ms — no hard cut.
      if (!loop_mode) {
        bool wrapped = reverse ? (read_pos > prev_pos) : (read_pos < prev_pos);
        if (wrapped) {
          playing   = false;
          recording = false;
          read_pos  = 0.0f;
        }
      }
    }

    return sample * level * fade_gain;
  }
};

// ---------------------------------------------------------------------------
// Footswitch hold-detection
//   Release before MODE_MS  → NormalPress
//   Hold ≥ MODE_MS          → ModePress (stop if playing; toggle mode if stopped)
//   Hold ≥ ERASE_MS         → ErasePress (only fires if hold started while stopped)
// ---------------------------------------------------------------------------
struct FootswitchTracker {
  bool prev_pressed;
  bool mode_triggered;
  bool erase_triggered;
  bool started_stopped;  // true when the hold began from a stopped state
  uint32_t press_start;

  void Process(bool pressed, Looper &looper) {
    if (pressed && !prev_pressed) {
      press_start = System::GetNow();
      mode_triggered = false;
      erase_triggered = false;
      started_stopped = looper.has_loop && !looper.playing;
    }

    uint32_t held_ms = System::GetNow() - press_start;

    if (pressed && !mode_triggered && held_ms >= MODE_MS) {
      looper.ModePress();
      mode_triggered = true;
    }

    if (pressed && mode_triggered && !erase_triggered &&
        started_stopped && held_ms >= ERASE_MS) {
      looper.ErasePress();
      erase_triggered = true;
    }

    if (!pressed && prev_pressed && !mode_triggered) {
      looper.NormalPress();
    }

    prev_pressed = pressed;
  }
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Looper looper1, looper2;
FootswitchTracker fs1 = {false, false, false, false, 0};
FootswitchTracker fs2 = {false, false, false, false, 0};
Led led1, led2;

// Save-request flag written by audio ISR, read and cleared by main loop.
static volatile bool save_requested = false;

// Dual-footswitch hold tracking (ISR-only state, no volatile needed).
static bool     dual_held       = false;
static uint32_t dual_hold_start = 0;
static bool     dual_triggered  = false;

// ---------------------------------------------------------------------------
// float ↔ int16 helpers
// ---------------------------------------------------------------------------
static inline int16_t FloatToI16(float f) {
  if (f >  1.0f) f =  1.0f;
  if (f < -1.0f) f = -1.0f;
  return static_cast<int16_t>(f * 32767.0f);
}

static inline float I16ToFloat(int16_t s) {
  return static_cast<float>(s) / 32767.0f;
}

// ---------------------------------------------------------------------------
// QSPI mode helpers
// Switch between MEMORY_MAPPED (read-only via GetData pointer) and
// INDIRECT_POLLING (required for Erase/Write).
// ---------------------------------------------------------------------------
static void QspiSwitchMode(QSPIHandle::Config::Mode mode) {
  QSPIHandle::Config cfg = hw.seed.qspi.GetConfig();
  if (cfg.mode == mode) return;
  cfg.mode = mode;
  hw.seed.qspi.DeInit();
  hw.seed.qspi.Init(cfg);
}

// ---------------------------------------------------------------------------
// LoadLoops — called once at startup before audio begins.
// Returns true if valid save data was found and loaded.
// ---------------------------------------------------------------------------
static bool LoadLoops() {
  QspiSwitchMode(QSPIHandle::Config::Mode::MEMORY_MAPPED);

  const uint8_t *base = static_cast<const uint8_t *>(hw.seed.qspi.GetData());
  const SaveHeader *hdr =
      reinterpret_cast<const SaveHeader *>(base + QSPI_HEADER_OFFSET);

  // Validate header
  if (hdr->magic != SAVE_MAGIC) return false;
  if (hdr->version != SAVE_VERSION) return false;
  uint32_t expected =
      hdr->magic ^ hdr->version ^ hdr->loop1_length ^ hdr->loop2_length;
  if (hdr->checksum != expected) return false;
  if (hdr->loop1_length > MAX_SAVE_SAMPLES) return false;
  if (hdr->loop2_length > MAX_SAVE_SAMPLES) return false;

  // Load loop 1
  if (hdr->loop1_length > 0) {
    const int16_t *src =
        reinterpret_cast<const int16_t *>(base + QSPI_LOOP1_OFFSET);
    size_t len = hdr->loop1_length;
    for (size_t i = 0; i < len; ++i) {
      looper1.buffer[i] = I16ToFloat(src[i]);
    }
    looper1.loop_length = len;
    looper1.has_loop    = true;
    looper1.playing     = false;
    // Apply the same end-fade that NormalPress bakes in so the loop splice
    // is silent on first playback.
    size_t xfade = (XFADE_LEN < len / 2) ? XFADE_LEN : len / 2;
    for (size_t i = 0; i < xfade; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(xfade);
      looper1.buffer[i]           *= t;
      looper1.buffer[len - 1 - i] *= t;
    }
  }

  // Load loop 2
  if (hdr->loop2_length > 0) {
    const int16_t *src =
        reinterpret_cast<const int16_t *>(base + QSPI_LOOP2_OFFSET);
    size_t len = hdr->loop2_length;
    for (size_t i = 0; i < len; ++i) {
      looper2.buffer[i] = I16ToFloat(src[i]);
    }
    looper2.loop_length = len;
    looper2.has_loop    = true;
    looper2.playing     = false;
    size_t xfade = (XFADE_LEN < len / 2) ? XFADE_LEN : len / 2;
    for (size_t i = 0; i < xfade; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(xfade);
      looper2.buffer[i]           *= t;
      looper2.buffer[len - 1 - i] *= t;
    }
  }

  return (hdr->loop1_length > 0 || hdr->loop2_length > 0);
}

// ---------------------------------------------------------------------------
// SaveLoops — called from the main loop (never from the audio ISR).
// Converts both loop buffers to int16 and writes them to QSPI flash.
// Loops longer than MAX_SAVE_SAMPLES are truncated.
// ---------------------------------------------------------------------------
static void SaveLoops() {
  QspiSwitchMode(QSPIHandle::Config::Mode::INDIRECT_POLLING);

  size_t len1 = (looper1.loop_length < MAX_SAVE_SAMPLES)
                    ? looper1.loop_length
                    : MAX_SAVE_SAMPLES;
  size_t len2 = (looper2.loop_length < MAX_SAVE_SAMPLES)
                    ? looper2.loop_length
                    : MAX_SAVE_SAMPLES;

  // Erase header + loop-1 region, then loop-2 region separately.
  // Using two calls avoids erasing the potentially large gap between them.
  uint32_t erase1_end = QSPI_LOOP1_OFFSET + static_cast<uint32_t>(len1 * 2);
  erase1_end = (erase1_end + 0xFFFu) & ~0xFFFu;  // round up to 4 KB sector
  if (erase1_end < 0x1000u) erase1_end = 0x1000u;
  hw.seed.qspi.Erase(QSPI_HEADER_OFFSET, erase1_end);

  if (len2 > 0) {
    uint32_t erase2_end =
        QSPI_LOOP2_OFFSET + static_cast<uint32_t>(len2 * 2);
    erase2_end = (erase2_end + 0xFFFu) & ~0xFFFu;
    hw.seed.qspi.Erase(QSPI_LOOP2_OFFSET, erase2_end);
  }

  // Write header
  SaveHeader hdr;
  hdr.magic        = SAVE_MAGIC;
  hdr.version      = SAVE_VERSION;
  hdr.loop1_length = static_cast<uint32_t>(len1);
  hdr.loop2_length = static_cast<uint32_t>(len2);
  hdr.checksum     = hdr.magic ^ hdr.version ^ hdr.loop1_length ^ hdr.loop2_length;
  hw.seed.qspi.Write(QSPI_HEADER_OFFSET, sizeof(SaveHeader),
                     reinterpret_cast<uint8_t *>(&hdr));

  // Write loop 1 in chunks (float → int16)
  for (size_t i = 0; i < len1; i += SAVE_CHUNK_SIZE) {
    size_t n = ((i + SAVE_CHUNK_SIZE) <= len1) ? SAVE_CHUNK_SIZE : (len1 - i);
    for (size_t j = 0; j < n; ++j) io_chunk[j] = FloatToI16(looper1.buffer[i + j]);
    hw.seed.qspi.Write(QSPI_LOOP1_OFFSET + static_cast<uint32_t>(i * 2),
                       static_cast<uint32_t>(n * 2),
                       reinterpret_cast<uint8_t *>(io_chunk));
  }

  // Write loop 2 in chunks (float → int16)
  for (size_t i = 0; i < len2; i += SAVE_CHUNK_SIZE) {
    size_t n = ((i + SAVE_CHUNK_SIZE) <= len2) ? SAVE_CHUNK_SIZE : (len2 - i);
    for (size_t j = 0; j < n; ++j) io_chunk[j] = FloatToI16(looper2.buffer[i + j]);
    hw.seed.qspi.Write(QSPI_LOOP2_OFFSET + static_cast<uint32_t>(i * 2),
                       static_cast<uint32_t>(n * 2),
                       reinterpret_cast<uint8_t *>(io_chunk));
  }

  // Return to memory-mapped mode for normal operation
  QspiSwitchMode(QSPIHandle::Config::Mode::MEMORY_MAPPED);
}

// ---------------------------------------------------------------------------
// Audio callback
// ---------------------------------------------------------------------------
void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  hw.ProcessAllControls();

  // ---- Dual-footswitch hold detection (save trigger) --------------------
  // When both switches are held simultaneously for SAVE_HOLD_MS, a save is
  // requested.  Individual footswitch processing is suppressed during the
  // dual hold to prevent ErasePress or unintended ModePress from firing.
  bool fs1_pressed = hw.switches[Hothouse::FOOTSWITCH_1].Pressed();
  bool fs2_pressed = hw.switches[Hothouse::FOOTSWITCH_2].Pressed();
  bool both_pressed = fs1_pressed && fs2_pressed;

  if (both_pressed) {
    if (!dual_held) {
      dual_held       = true;
      dual_hold_start = System::GetNow();
      dual_triggered  = false;
    } else if (!dual_triggered &&
               (System::GetNow() - dual_hold_start) >= SAVE_HOLD_MS) {
      save_requested = true;
      dual_triggered = true;
    }
  } else {
    dual_held      = false;
    dual_triggered = false;
  }

  // ---- Individual footswitch handling (suppressed during dual hold) ------
  if (!dual_held) {
    fs1.Process(fs1_pressed, looper1);
    fs2.Process(fs2_pressed, looper2);
  } else {
    // Pass false so individual hold timers reset and no actions fire while
    // the dual-hold gesture is in progress.
    fs1.Process(false, looper1);
    fs2.Process(false, looper2);
  }

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

  // Attempt to load saved loops from QSPI.  This runs before audio starts so
  // the buffers are fully populated before the first audio callback fires.
  bool loaded = LoadLoops();

  // Double-flash both LEDs to confirm a successful load.
  if (loaded) {
    for (int i = 0; i < 2; ++i) {
      led1.Set(1.0f); led1.Update();
      led2.Set(1.0f); led2.Update();
      hw.DelayMs(150);
      led1.Set(0.0f); led1.Update();
      led2.Set(0.0f); led2.Update();
      hw.DelayMs(150);
    }
  }

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  int blink1_remaining = 0, blink1_timer = 0;
  int blink2_remaining = 0, blink2_timer = 0;
  uint32_t tick = 0;

  while (true) {
    hw.DelayMs(10);
    tick++;

    // ---- Handle save request -------------------------------------------
    if (save_requested) {
      save_requested = false;

      // Both LEDs solid while the blocking erase+write runs.
      led1.Set(1.0f); led1.Update();
      led2.Set(1.0f); led2.Update();

      SaveLoops();

      // Queue a 3× confirmation blink on both LEDs.
      blink1_remaining = 3; blink1_timer = 0;
      blink2_remaining = 3; blink2_timer = 0;
    }

    UpdateLed(led1, looper1, blink1_remaining, blink1_timer, tick);
    UpdateLed(led2, looper2, blink2_remaining, blink2_timer, tick);
  }

  return 0;
}
