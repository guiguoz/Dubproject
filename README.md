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
