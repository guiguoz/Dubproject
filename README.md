# SONIC MONOLITH | SAX-OS

Real-time audio effects processor for live saxophone performance, built with JUCE/C++.
Designed for dub techno live sets with a neon dark "SAX-OS" interface.

**Input** : Focusrite Scarlett Solo 2nd gen (ASIO / WASAPI fallback)
**Effects** : 12 effect types with drag-and-drop effect chain
**Sampler** : 8-track step sequencer with AI-assisted mixing
**Control** : MIDI pedalboard (e.g. Behringer FCB1010)
**Target latency** : <= 20 ms

---

## Features

### Effect Chain (12 types)

| Effect | Description | Presets |
|--------|-------------|---------|
| Reverb | JUCE freeverb (room, damping, width, mix) | 6 |
| Delay | Tempo-synced with note divisions | 5 |
| Flanger | LFO-modulated comb filter | 5 |
| Harmonizer | 2-voice pitch-shifted harmony (WSOLA) | 7 |
| Envelope Filter | Dynamics-driven auto-wah | 5 |
| Octaver | Sub-octave generator (-1, -2 oct) | 5 |
| PitchFork | Fixed pitch shift | 5 |
| Whammy | Expression-controlled pitch bend | 5 |
| AutoPitch | Chromatic pitch correction | 4 |
| Slicer | Rhythmic gate/tremolo | 6 (PresetLibrary) + 15 built-in |
| Accordeur | Chromatic tuner (A=415-465 Hz) | 2 |
| Synth | Pitch-tracking synth (PolyBLEP, SuperSaw, Moog filter) | 22 built-in |

Effects are organized in a modular chain with per-effect enable/disable,
drag-and-drop reordering, and SmartMixEngine auto-optimization.

### Synth Effect

Full pitch-tracking synthesizer that follows the saxophone input:

- **Oscillators**: Saw, Square, Triangle, Sine, SuperSaw (7-voice unison with detune)
- **Filter**: Moog ladder (4-pole resonant lowpass)
- **Envelope**: Amplitude envelope follower with attack/release
- **8 parameters**: Waveform, Octave, Detune, Cutoff, Resonance, Attack, Release, Mix
- **22 presets**: Dub Techno (sub bass, chord stab, deep pad, dub siren, techno lead),
  Techno (acid bass, reese, hoover, Detroit pad), Leads, Ambient, Percussive, FX/Creative

### Sampler & Step Sequencer

- 8 sample slots with WAV loading (drag-and-drop from Explorer / Sononym)
- 16-step sequencer per track with velocity
- BPM sync, swing, per-track mute/solo
- AI content classification (ONNX) for automatic sample categorization
- **Auto-Match** on load: BPM + key detected, sample time-stretched + pitch-shifted to project tempo/key
  - 3-method BPM detection with confidence score (RMS autocorrelation, onset-strength, comb-filter)
  - Hermite 4-point interpolation resample for high-quality tempo alignment
  - Pitch-first dual-pass: WSOLA pitch shift on clean signal → Hermite tempo stretch
  - Sample-rate normalisation: 48 kHz files resampled to project rate automatically
  - Popup confirmation when BPM confidence < 50% (use detected / enter manually / cancel)
  - BPM confidence indicator per slot (green ✓ / amber ~ / red ?)

### AI / ONNX Integration

- **AiContentClassifier**: Neural network sample classifier (KICK, SNARE, HIHAT, BASS, PAD, SYNTH, PERC)
- **AiMixEngine**: ML-driven EQ + gain optimization across 8 slots
- **FeatureExtractor**: Real-time spectral analysis (RMS, centroid, crest factor)
- **InferenceThread**: Lock-free async inference with < 5 ms latency
- Fallback to heuristic rules when ONNX models unavailable

### UI: SAX-OS Neon Dark Theme

- Near-black backgrounds (#0A0A0A, #131314, #1C1B1C)
- Neon green primary (#4CDFA8) with per-effect accent colours
- Dark metal rotary knobs with neon value arcs and glow
- Glow buttons, neon toggle circles, dark rounded popups
- VU meter with exponential smoothing (attack 0.3, release 0.05)
- Inter font family, uppercase tracking labels

---

## Prerequisites

| Tool | Version | Installation |
|------|---------|-------------|
| CMake | >= 3.22 | `winget install Kitware.CMake` |
| MSVC | 2022 | [Visual Studio Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe) |
| Git | recent | `winget install Git.Git` |
| ASIO SDK | 2.3+ | [Steinberg.net](https://www.steinberg.net/developers/) (free, account required) |
| ONNX Runtime | 1.24.4 | Auto-downloaded by CMake (`FetchContent`) |
| Python | 3.9+ | `winget install Python.Python.3` (for AI training scripts) |

> **ASIO note:** The Steinberg SDK is not redistributable. Download it manually
> and extract to `third_party/ASIO/`. Without it, the app uses WASAPI fallback
> (latency ~10-20 ms, sufficient for testing).

---

## Build

### 1. Clone with JUCE submodule

```bash
git clone --recurse-submodules https://github.com/guiguoz/Dubproject.git
cd Dubproject
```

Or if already cloned without submodules:
```bash
git submodule update --init --recursive
```

### 2. Configure CMake

**Without ASIO SDK (WASAPI fallback):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

**With ASIO SDK:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DJUCE_ASIO_SDK_PATH=third_party/ASIO
```

### 3. Build

```bash
cmake --build build --config Release --parallel
```

### 4. Run

```bash
./build/SaxFXLive_artefacts/Release/SaxFX\ Live.exe
```

### 5. Run tests

```bash
cmake --build build --config Release --target SaxFXTests --parallel
./build/tests/Release/SaxFXTests.exe
```

180 tests (Catch2 v3.5.4). Current status: 179/180 pass (1 flaky timing test).

---

## Project Structure

```
projet-dub/
├── src/
│   ├── MainComponent.h/.cpp      Main app (audio callback, UI layout)
│   ├── dsp/                      DSP engine
│   │   ├── IEffect.h             Effect interface + EffectType enum
│   │   ├── EffectChain.h/.cpp    Ordered effect chain container
│   │   ├── EffectFactory.h/.cpp  Effect creation by name/enum
│   │   ├── EffectChainOptimizer  AI-driven parameter optimization
│   │   ├── SmartMixEngine.h      Intelligent defaults + genre overrides
│   │   ├── SynthEffect.h/.cpp    Pitch-tracking synth (22 presets)
│   │   ├── DelayEffect.h/.cpp    Tempo-synced delay (5 presets)
│   │   ├── SlicerEffect.h/.cpp   Rhythmic gate (15 built-in presets)
│   │   ├── TunerEffect.h/.cpp    Chromatic tuner (A=442 ref)
│   │   ├── [12 more effects]     Reverb, Flanger, Harmonizer, etc.
│   │   ├── DspPipeline.h/.cpp    Audio processing pipeline
│   │   ├── Sampler.h/.cpp        8-slot sample player
│   │   ├── StepSequencer.h       16-step sequencer
│   │   ├── BpmDetector.h/.cpp    Tempo detection
│   │   ├── KeyDetector.h/.cpp    Key/scale detection
│   │   ├── YinPitchTracker.cpp   YIN pitch detection
│   │   ├── WsolaShifter.h/.cpp   WSOLA pitch shifting
│   │   ├── AiContentClassifier   ONNX sample classifier
│   │   ├── AiMixEngine.h/.cpp    ONNX mix optimizer
│   │   ├── FeatureExtractor      Spectral feature extraction
│   │   ├── OnnxInference.h       ONNX Runtime wrapper
│   │   └── InferenceThread.h     Async inference thread
│   ├── ui/                       UI components
│   │   ├── SaxOsLookAndFeel      Neon dark theme (SAX-OS)
│   │   ├── SaxFXLookAndFeel      Original theme
│   │   ├── Colours.h             Palette + per-effect accents
│   │   ├── EffectRackUnit.h      Effect card (icon, name, knobs)
│   │   ├── PedalboardPanel.h     Drag-and-drop effect chain
│   │   ├── EffectChainEditor.h   Chain editor wrapper
│   │   ├── PresetLibrary.h       Compile-time preset tables
│   │   ├── StepSequencerPanel.h  Sequencer grid UI
│   │   ├── SamplerPanel.h        Sample slot panel
│   │   └── [more UI components]  MagicButton, RotaryKnob, etc.
│   └── project/                  Project save/load
│       ├── ProjectData.h         Project data model (v4)
│       ├── ProjectLoader.h/.cpp  JSON serialization (.saxfx)
├── tests/                        Catch2 unit tests (180 tests)
├── models/                       Trained ONNX models
│   ├── content_classifier.onnx   Sample classifier
│   └── mix_model.onnx            Mix optimizer
├── scripts/                      Python training scripts
│   ├── train_classifier.py       Train sample classifier
│   ├── train_mix_model.py        Train mix model
│   └── prepare_dataset.py        Generate synthetic dataset
├── cmake/
│   └── FindOnnxRuntime.cmake     ONNX Runtime finder
├── third_party/
│   ├── JUCE/                     Submodule JUCE 8
│   └── ASIO/                     Steinberg SDK (not versioned)
└── CMakeLists.txt                Build configuration
```

---

## Roadmap

| Sprint | Status | Objective |
|--------|--------|-----------|
| **Sprint 1** (MVP) | Done | Audio pipeline + pass-through |
| **Sprint 2** | Done | Pitch tracker + harmonizer + flanger |
| **Sprint 3** | Done | Sampler 8 slots + MIDI + project loader |
| **Sprint 4** | Done | EffectChain + UI refactor (src/ui/) |
| **Sprint 5** | Done | FX Pack (Delay, Octaver, Whammy, Slicer, Tuner, etc.) |
| **Sprint 6** | Done | PresetLibrary + SynthEffect + SaxOS UI theme |
| **Sprint 7** | Done | ONNX Runtime integration + InferenceThread |
| **Sprint 8** | Done | AI content classifier (sample categorization) |
| **Sprint 9** | Done | AI mix engine (EQ + gain optimization) |
| **Sprint 10** | Done | Master limiter, dynamic ducking (anti-masking), drag-and-drop sampler |
| **Sprint 11** | Done | Audio quality overhaul + Auto-Match Tempo/Key |

### Sprint 11 — Audio quality overhaul + Auto-Match

| Fix | Description |
|-----|-------------|
| C1 | Exponential fade-in at trigger onset — eliminates click/pop |
| C2 | EMA ramp on ducking gain — eliminates ducking crackle |
| C3/C4 | AI EQ corrected: filter order + frequencies (100/2500/8000 Hz) + ±6 dB clamp |
| M2 | True-peak detection (4× oversampling ITU-R BS.1770) + −3 dBFS headroom |
| Phase 2 | `autoMatchSampleAsync`: 3-method BPM detection, Hermite resample, pitch-first dual-pass, SR normalisation, BPM confidence popup |

---

## Git Conventions

Format: `type(scope): description`

Types: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`

Examples:
```
feat(synth): add pitch-tracking synth with 22 presets
fix(project): prevent crash on load (forceRebuild after applyChain)
feat(ui): neon dark SaxOS look-and-feel
```

---

## License

MIT (c) 2026 Guillaume -- see [LICENSE](LICENSE)
