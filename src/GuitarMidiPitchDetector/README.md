# GuitarMidiPitchDetector

Realtime polyphonic pitch detector that converts guitar audio to MIDI notes
over USB. Uses FFT-based spectral analysis with peak detection and parabolic
interpolation for sub-bin frequency accuracy.

The audio signal passes through unmodified -- this effect only analyzes the
input and sends MIDI. Connect the Hothouse via USB to a computer or MIDI host
to receive the MIDI output.

**Note:** This is a best-effort polyphonic detector. Harmonics and partials of
played notes may trigger additional MIDI notes. Use the threshold and note
range knobs to tune behavior for your setup.

## Controls

| CONTROL | DESCRIPTION |
| --- | --- |
| KNOB 1 | Threshold -- sets the detection sensitivity. Turn right for higher threshold (fewer spurious notes), left for lower (more sensitive) |
| KNOB 2 | Velocity scale -- adjusts MIDI velocity output. Left = quieter, right = louder |
| KNOB 3 | (Reserved) |
| KNOB 4 | (Reserved) |
| KNOB 5 | Low note filter -- sets the lowest MIDI note to output |
| KNOB 6 | High note filter -- sets the highest MIDI note to output |
| TOGGLE 1 | Sensitivity preset: UP = high (more notes/partials), MID = medium, DOWN = low (fewer, cleaner notes) |
| TOGGLE 2 | MIDI channel: UP = channel 1, MID = channel 2, DOWN = channel 3 |
| TOGGLE 3 | (Unused) |
| FOOTSWITCH 1 | Hold 2s to reset to bootloader |
| FOOTSWITCH 2 | Enable / bypass pitch detection (LED 2 on = active) |
| LED 1 | Note activity indicator -- lights when MIDI notes are being sent |
| LED 2 | Active/bypass indicator |
