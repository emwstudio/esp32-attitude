This directory is baked into the SPIFFS `storage` partition at build time.
Put fallback alert WAVs in `voice/` (16 kHz 16-bit mono WAV):
  system_ready.wav  pitch_high.wav  bank_angle.wav  sink_rate.wav  stall.wav  pull_up.wav
Generate them with ../voice-pipeline (see README there).
