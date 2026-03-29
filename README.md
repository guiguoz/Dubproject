# SaxFX Live

Real-time audio processing application for live saxophone performance.

**Entrée** : Focusrite Scarlett Solo 2e gen (ASIO)
**Effets** : Réverb, Flanger, Harmoniseur, Sampler
**Contrôle** : Pédalier MIDI (ex. Behringer FCB1010)
**Latence cible** : ≤ 20 ms

---

## Prérequis

| Outil | Version | Installation |
|-------|---------|-------------|
| CMake | ≥ 3.22 | `winget install Kitware.CMake` |
| MSVC | 2022 | [Visual Studio Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe) |
| Git | récent | `winget install Git.Git` |
| ASIO SDK | 2.3+ | [Steinberg.net](https://www.steinberg.net/developers/) (gratuit, compte requis) |
| Python | 3.9+ | `winget install Python.Python.3` (pour test latence) |

> **Note ASIO :** Le SDK Steinberg n'est pas redistributable. Télécharge-le manuellement
> et extrais-le dans `third_party/ASIO/`. Sans ce SDK, l'app utilisera WASAPI en fallback
> (latence ~10–20 ms, suffisante pour les tests).

---

## Build

### 1. Cloner le repo avec JUCE

```bash
git clone --recurse-submodules https://github.com/<ton-compte>/saxfx-live.git
cd saxfx-live
```

Ou si déjà cloné sans submodules :
```bash
git submodule update --init --recursive
```

### 2. Configurer CMake

**Sans ASIO SDK (WASAPI fallback) :**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

**Avec ASIO SDK :**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DJUCE_ASIO_SDK_PATH=third_party/ASIO
```

### 3. Compiler

```bash
cmake --build build --config Release --parallel
```

### 4. Lancer

```bash
./build/SaxFXLive_artefacts/Release/SaxFXLive.exe
```

---

## Test de latence

```bash
pip install pyaudio
python scripts/latency_test.py
```

Cible : ≤ 20 ms avec ASIO, ≤ 30 ms avec WASAPI.

---

## Structure du projet

```
saxfx-live/
├── src/                  Code source C++ (JUCE)
├── tests/                Tests unitaires (Catch2)
├── docs/                 Documentation technique
│   └── project-format.md Spec du format .saxfx
├── scripts/              Outils (setup, test latence)
├── third_party/
│   ├── JUCE/             Submodule JUCE 8
│   └── ASIO/             SDK Steinberg (non versionné)
└── .github/workflows/    CI GitHub Actions
```

---

## Roadmap

| Sprint | Objectif | Critère |
|--------|----------|---------|
| **Sprint 1** (MVP) | Pipeline audio + pass-through | Signal casque < 20 ms |
| **Sprint 2** | Pitch tracker + harmoniseur + effets | Latence ≤ 20 ms, tests > 80% |
| **Sprint 3** | Sampler + MIDI + presets | Live-ready, CI verte |
| **Sprint 4** | EffectChain & UI refactor | Modularity + src/ui/ |
| **Sprint 5** | FX Pack (Delay, Octaver, etc.)| 5 new stage FX |
| **Sprint 6** | Selection UI & Polish | Presets + MIDI mapping |

### Sprint 7–10 : Auto-mastering IA (Niveau 3)

Objectif : remplacer les règles heuristiques de `SmartSamplerEngine` et
`SmartMixEngine` par un modèle IA capable d'analyser le mix global
(sax + 8 slots) et de produire gain, EQ et compression optimaux.

Chaque étape est **autonome, testable et mergeable** indépendamment.

#### Sprint 7 — Infrastructure ONNX Runtime

| Étape | Tâche | Test / Critère de validation |
|-------|-------|------------------------------|
| 7.1 | ✅ Intégrer ONNX Runtime dans `CMakeLists.txt` + `cmake/FindOnnxRuntime.cmake` | Build OK, 150/151 tests verts, DLL copiée |
| 7.2 | ✅ Créer `src/dsp/OnnxInference.h` — wrapper header-only chargement + inférence | 5 tests ONNX verts (load, identity, sine, inputSize, invalid path) |
| 7.3 | ✅ `InferenceThread.h` — thread IA avec `LockFreeQueue` (submit/poll) | 3 tests verts : single request, 100 sans perte, latence < 5 ms |
| 7.4 | ✅ Benchmark : 50 runs median + P95 + throughput 100 inférences | 2 tests verts, median < 2 ms, avg < 2 ms |

#### Sprint 8 — Classification IA des samples

| Étape | Tâche | Test / Critère de validation |
|-------|-------|------------------------------|
| 8.1 | ✅ `scripts/prepare_dataset.py` — génère 210 samples synthétiques (7 catégories × 30) | 210 WAV dans `data/dataset/` |
| 8.2 | ✅ `scripts/train_classifier.py` → `models/content_classifier.onnx` | Accuracy ≥ 85% vérifiée à l'entraînement |
| 8.3 | ✅ `src/dsp/AiContentClassifier.h/.cpp` + `tests/test_ai_classifier.cpp` | 5 tests verts : kick→KICK, hihat→HIHAT, snare→SNARE, silence, resample |
| 8.4 | ✅ Remplacer `detectContentType()` heuristique par `AiContentClassifier` (avec fallback) | `#ifdef SAXFX_HAS_ONNX` dans `SmartSamplerEngine`, tests existants verts |
| 8.5 | ✅ `scripts/ab_test_classifier.py` — A/B test IA vs heuristique sur 50 samples | Log comparatif dans `docs/ab_test_classifier_results.txt`, IA ≥ heuristique |

#### Sprint 9 — Mix IA adaptatif (EQ + gain multi-pistes)

| Étape | Tâche | Test / Critère de validation |
|-------|-------|------------------------------|
| 9.1 | ✅ Définir format d'entrée IA : features par slot (RMS, centroid spectral, type, crest factor) | Struct `MixFeatures`, test extraction sur samples connus |
| 9.2 | ✅ Créer `src/dsp/FeatureExtractor.h/.cpp` — extraction features temps réel | Test : sine 440 Hz → centroid ≈ 440 Hz, crest ≈ 1.41 |
| 9.3 | ✅ Entraîner modèle mix (features 8 slots → EQ gains + volume par slot) | Script `scripts/train_mix_model.py`, val MSE = 0.245 (300 epochs, lr=0.003) |
| 9.4 | ✅ Créer `src/dsp/AiMixEngine.h/.cpp` — remplace `applyRoleEQ` + `applyUnmasking` + `targetGainForType` | Test : 4 slots chargés → EQ + gains cohérents, pas de clipping |
| 9.5 | ✅ Intégrer dans `SmartSamplerEngine::applyNeutronMix` (IA si dispo, sinon fallback heuristique) | `setUseAiMix(bool)` + EQ 3-band biquad (lowShelf 250 Hz / peak 1 kHz / highShelf 4 kHz) |
| 9.6 | Ajouter analyse du signal sax live dans le mix IA (sax = slot "virtuel") | Test : sax 440 Hz → IA réduit les mids des slots qui masquent le sax |

#### Sprint 10 — Compression IA + polish

| Étape | Tâche | Test / Critère de validation |
|-------|-------|------------------------------|
| 10.1 | Ajouter compression dynamique par slot dans `AiMixEngine` | Test : signal avec crête à -1dB → compressé, RMS stable |
| 10.2 | Master limiter léger en sortie (peak < 0 dBFS garanti) | Test : signal saturé en entrée → sortie ≤ 0 dBFS |
| 10.3 | UI : affichage des décisions IA (EQ curves, gains, compression) | Visuel, pas de crash |
| 10.4 | Benchmark latence globale pipeline (sax + 8 slots + IA) | Assert total ≤ 20 ms |
| 10.5 | A/B test live : mix heuristique vs mix IA sur 5 morceaux | Évaluation subjective, documenter résultats dans `docs/` |

---

## Conventions Git

Format : `type(scope): description`

Types : `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`

Exemples :
```
feat(audio): add ASIO pass-through pipeline
feat(sampler): support wav/sfz sample loading
fix(pitch): correct octave detection error
```

---

## Licence

MIT © 2026 Guillaume — voir [LICENSE](LICENSE)
