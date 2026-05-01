# Plan — SceneManager v2: Transitions de scène sans coupure

**État:** 🔄 En cours (Sprint 10)  
**Bloqueur antérieur:** "navigation entre projets" — reprendre après ce fix  
**Priorité:** P0 (audio pipeline critique)

---

## 🎯 Objectif

Extraire logique transition de scène de `MainComponent` → `SceneManager` dédié. **Swap quantisé à la frontière de bar** sans `stopAllSlots()` — garantit audio continu.

### Amélioration clé (v2 vs v1)

- ✅ `preparedBuf_` atomique explicite (évite race sur `loadBackBuffer`)
- ✅ Double-buffer `swapBufs_[2]` (pas d'écrasement durant lecture audio)
- ✅ Loader thread persistant (pas d'allocation temps-réel)
- ✅ Crossfade parallèle après swap (300ms fade per-slot)

---

## 📋 Fichiers à modifier

| Fichier | Action | Complexité |
|---------|--------|-----------|
| `src/dsp/StepSequencer.h` | Ajouter `StepBuf` + double-buffer | 🔵 Moyenne |
| `src/dsp/SceneManager.h` | **NOUVEAU** — classe maître transitions | 🔴 Haute |
| `src/MainComponent.h` | Remplacer membres scène par `SceneManager` | 🟡 Faible |
| `src/MainComponent.cpp` | Déléguer logique → `SceneManager` | 🟡 Faible |

---

## 🔧 Étape 1: StepSequencer.h — Double-buffer

### Ajouter à `public:`

```cpp
struct StepBuf {
    bool steps[9][512] {};
    int barCounts[9] { 1,1,1,1,1,1,1,1,1 };
};

// GUI thread: écrire dans le buffer de fond
void prepareStepBuffer(const StepBuf& data) noexcept {
    const int back = 1 - activeBuf_.load(std::memory_order_relaxed);
    stepBufs_[back] = data;
    preparedBuf_.store(back, std::memory_order_release);
}

// Audio thread: flip si un buffer est prêt
void flipIfPrepared() noexcept {
    const int idx = preparedBuf_.exchange(-1, std::memory_order_acq_rel);
    if (idx >= 0) activeBuf_.store(idx, std::memory_order_release);
}

// Audio thread: consommer le flag de fin de cycle
bool consumeSceneEndAudio() noexcept {
    const bool f = sceneEndForAudio_.exchange(false, std::memory_order_acq_rel);
    if (f) pendingTransLen_.store(0, std::memory_order_relaxed);
    return f;
}

// Adapter getStep() — utiliser buffer actif
bool getStep(int track, int step) const noexcept {
    const auto& buf = stepBufs_[activeBuf_.load(std::memory_order_relaxed)];
    if (track >= 0 && track < 9 && step >= 0 && step < 512)
        return buf.steps[track][step];
    return false;
}

int getTrackBarCount(int track) const noexcept {
    return stepBufs_[activeBuf_.load(std::memory_order_relaxed)].barCounts[track];
}

int getTrackStepCount(int track) const noexcept {
    return getTrackBarCount(track) * 16;
}

void setTrackStepCount(int track, int count) noexcept {
    stepBufs_[activeBuf_.load(std::memory_order_relaxed)].barCounts[track] = count / 16;
}
```

### Ajouter à `private:`

```cpp
StepBuf stepBufs_[2];
std::atomic<int> activeBuf_{ 0 };
std::atomic<int> preparedBuf_{ -1 };
std::atomic<bool> sceneEndForAudio_{ false };
```

### Adapter `process()` (~ligne 180)

**Avant:** `if (steps_[track][trackStep])`  
**Après:**
```cpp
flipIfPrepared();
const auto& stepBuf = stepBufs_[activeBuf_.load(std::memory_order_relaxed)];
const int trackSteps = stepBuf.barCounts[track] * 16;
const int trackStep = globalAfter % trackSteps;
if (stepBuf.steps[track][trackStep]) sampler.trigger(track);
```

### Bloc `atSceneBoundary()` (~ligne 182)

Ajouter:
```cpp
sceneEndForAudio_.store(true, std::memory_order_release);
```

---

## 🔧 Étape 2: src/dsp/SceneManager.h — Fichier nouveau

[Voir contenu complet dans la description originale — 350+ lignes]

**Points clés:**
- **API publique:** `getScene()`, `captureCurrentTo()`, `applyImmediate()`, `prepare()`
- **Thread audio:** `processBarBoundary()`, `processGainRamp()`
- **Thread GUI (timer):** `pollSwapComplete()`
- **Double-buffer:** `swapBufs_[2]`, `stepBufs_[2]`
- **Loader persistant:** Classe `Loader: public juce::Thread` évite alloc audio

---

## 🔧 Étape 3: MainComponent.h — Substitution

### Supprimer

- `SceneData struct` (lignes 148-159)
- `currentScene_`, `pendingScene_`, `CrossfadeState`, `crossfade_` (lignes 162-200)

### Ajouter

```cpp
#include "dsp/SceneManager.h"

private:
dsp::SceneManager sceneManager_ { 
    dspPipeline_.getSampler(), 
    stepSequencer_,
    44100.0  // sample rate par défaut
};
```

✅ Ajouter `void setSampleRate(double sr)` dans `SceneManager` (public)

---

## 🔧 Étape 4: MainComponent.cpp — Adaptations

### constructor() — Brancher callbacks

```cpp
sceneManager_.loadPcm = [this](int slot, const std::string& path, int ts, int te) {
    loadSampleIntoSlot(slot, path, ts, te);
};

sceneManager_.clearSlot = [this](int slot) {
    dspPipeline_.getSampler().clearSlot(slot);
    stepSeqPanel_.setSlotFilePath(slot, "");
};

sceneManager_.onSceneApplied = [this](int newIdx) {
    const auto& sc = sceneManager_.getScene(newIdx);
    for (int i = 0; i < 9; ++i) {
        const std::size_t si = static_cast<std::size_t>(i);
        stepSeqPanel_.setSlotFilePath(i, sc.filePaths[si]);
        stepSeqPanel_.setSlotMuted(i, sc.mutes[si]);
        const int n = sc.trackBarCounts[si] * 16;
        for (int s = 0; s < n; ++s)
            stepSeqPanel_.setStepState(i, s, sc.steps[si][static_cast<std::size_t>(s)]);
    }
    updateSceneLabel();
};
```

### prepareToPlay()

```cpp
sceneManager_.setSampleRate(sampleRate);
```

### navigateScene() — Remplacer entièrement

```cpp
void MainComponent::navigateScene(int delta) {
    const int target = juce::jlimit(0, dsp::SceneManager::kScenes - 1,
        sceneManager_.currentIndex() + delta);
    if (target == sceneManager_.currentIndex()) return;

    if (!stepSequencer_.isPlaying()) {
        sceneManager_.applyImmediate(target);
        return;
    }

    sceneNumLabel_.setText(
        "Scene " + juce::String(sceneManager_.currentIndex() + 1) +
        " → " + juce::String(target + 1),
        juce::dontSendNotification);
    sceneManager_.prepare(target);
}
```

### captureCurrentScene()

```cpp
void MainComponent::captureCurrentScene() {
    sceneManager_.captureCurrentTo(sceneManager_.currentIndex());
}
```

### getNextAudioBlock() — Avant stepSequencer_.process()

```cpp
sceneManager_.processBarBoundary();
sceneManager_.processGainRamp(numSamples);
stepSequencer_.process(numSamples, dspPipeline_.getSampler());
```

### timerCallback() — Remplacer bloc scène+crossfade

**Supprimer:** Lignes 1966-2002 (pendingScene_/consumeSceneEnd + crossfade timer)

**Ajouter:**
```cpp
int swappedTo = -1;
if (sceneManager_.pollSwapComplete(swappedTo))
    if (sceneManager_.onSceneApplied)
        sceneManager_.onSceneApplied(swappedTo);
```

### applyProjectData()

```cpp
for (int si = 0; si < dsp::SceneManager::kScenes; ++si)
    sceneManager_.setScene(si, data.scenes[si]);
sceneManager_.applyImmediate(data.currentScene);
```

---

## 🧵 Thread-Safety Invariants

| Donnée | Écrit par | Lu par | Mécanisme |
|--------|-----------|--------|-----------|
| `stepBufs_[back]` | GUI `prepareStepBuffer()` | Audio `flipIfPrepared()` | Release/acquire sur `preparedBuf_` |
| `swapBufs_[w]` | GUI `prepare()` | Audio `processBarBoundary()` | Release/acquire sur `pendingSwap_` |
| `pendingSwap_` | Worker (sets) + Audio (clears) | Audio (reads) | `std::atomic<int>` |
| `xfade*` | Audio uniquement | Audio uniquement | Single-threaded (audio) |
| `swapDone_` | Audio | GUI | `std::atomic<bool>` |
| `audioFilePaths_` | GUI `pollSwapComplete()` | GUI loader | GUI thread uniquement |
| `scenes_[]` | GUI uniquement | GUI uniquement | Pas de partage audio |

### P0.1: preparedBuf_ Race Prevention

`prepareStepBuffer()` → `stepBufs_[back].store(back, release)`  
Audio `flipIfPrepared()` → `exchange(-1, acq_rel)` **avant accès**  
→ Impossible d'accéder buffer en cours d'écriture ✅

### P0.2: swapBufs_[2] Double-Buffer

GUI écrit dans `writeSlot_` (0 ou 1), puis bascule `writeSlot_`  
Audio lit depuis `pendingSwap_`  
→ Aucun accès concurrent sur même struct ✅

---

## ✅ Checklist de validation

- [ ] `cmake --build build --config Release --parallel` → zéro warning
- [ ] Scènes identiques → transition imperceptible (audio continu)
- [ ] Scène avec slot différent → seul ce slot fait fade-out 350 ms
- [ ] Séquenceur arrêté → swap immédiat (identique à avant)
- [ ] Spam navigation rapide → `loadGen_` annule anciens jobs, pas de crash
- [ ] ThreadSanitizer → zéro data race
- [ ] Tests existants restent verts

---

## 📝 Notes

- **Durée crossfade:** 300ms = `xfadeDuration_ = sr * 0.3` samples
- **Loader thread:** Persistent, pas alloc temps-réel — prépare buffers en arrière-plan
- **Gain ramp:** Appliqué après swap quantisé (lors de `atSceneBoundary()`)
- **Backward compat:** Ancien `applyScene()` → `applyImmediate()` (arrêté)
