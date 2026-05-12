# PingPongDelay

RT-safe stereo ping-pong delay avec tone/drive, intégré dans `DspPipeline` en post-ducking.

## API

```cpp
void prepare(double sr, int maxBlock) noexcept;
void reset()   noexcept;
void setBpm(float bpm)        noexcept;  // sync BPM transport
void setEnabled(bool)         noexcept;
void setWet(float 0..1)       noexcept;  // default 0.28
void setFeedback(float 0..1)  noexcept;  // clamp 0..0.95, default 0.40
void setTone(float 0..1)      noexcept;  // LP/HP tone shaping, default 0.5
void setDrive(float 0..1)     noexcept;  // saturation tanh, default 0.15
void setDiv(int)              noexcept;  // division index (static_cast<int>(GridDiv))
void setFreeze(bool)          noexcept;  // bypass processing (transitions scène)
void processAdd(const float* inL, const float* inR,
                float* outL, float* outR, int n) noexcept;
```

## Intégration DspPipeline

- `getDubDelay()` retourne le `PingPongDelay` primaire.
- BPM propagé à chaque bloc via `setBpm(bpm)` dans `DspPipeline::setBpm()`.
- `processAdd` appelé en post-ducking sur les buffers sampler (`tempBufL_/R_`).
- `setDiv()` reçoit un `static_cast<int>(GridDiv)` depuis le combo UI.

## Chaîne de traitement

```
inL/R → delay line (quarter-note, ping-pong L↔R feedback)
      → LP 1-pôle → HP (différence signal - LP)
      → saturation tanh (drive)
      → LP 1-pôle final
      → outL/R
```

## Thread-safety

Tous les paramètres (`bpm_`, `wet_`, `fb_`, `tone_`, `drive_`, `div_`, `freeze_`) sont `std::atomic`. `processAdd` est RT-safe.

## Tests

`tests/test_pingpongdelay.cpp` — 2 cas, 2304 assertions :
- Basic init : impulse → sorties finies (pas de NaN)
- Integration : 4 blocs × 256 samples, sorties finies après chaque bloc
