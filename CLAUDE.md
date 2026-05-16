# CLAUDE.md — DubEngine / SaxFX Live

Fichier de contexte pour assistants IA. Lire aussi [AGENTS.md](AGENTS.md) (architecture,
threading, pipeline audio) et [README.md](README.md) (features, build, roadmap).

---

## Résumé projet (30 secondes)

Application **desktop JUCE C++17** de performance live **dub techno**.
- Instrument : **AKAI EWI** (vent MIDI) → traitement EWI temps réel (EffectChain)
- Playback : **sampler 9 slots** piloté par step sequencer (jusqu'à 512 pas/track)
- IA intégrée : classifieur ONNX (8 types) + optimiseur de mix ONNX (8 slots)
- **Synth VST3 : Serum V2** hosté en interne (`SerumHost`)
- **8 scènes** avec transitions adaptatives (crossfade variable 120–600 ms)

---

## Build & exécutable

```bash
cmake -B build -DJUCE_ASIO_SDK_PATH=third_party/ASIO   # ASIO optionnel
cmake --build build --config Release --target SaxFXLive
# Binaire : bin/SaxFX Live.exe   ← pas dans build/, dans bin/
```

Tests : `cmake --build build --config Release --target SaxFXTests`

---

## Ce qu'un AI dev doit savoir (non-obvious)

### 1. Slots ont des rôles sémantiques fixes

| Slot | Rôle | Notes |
|------|------|-------|
| 0 | MST (master loop) | Toujours actif |
| 1 | BSS (bass) | |
| 2 | KCK (kick) | |
| 3 | SNR (snare) | |
| 4 | HAT (hihat) | |
| 5 | PAD | Delay send élevé (0.8) |
| 6 | SYN (synth sample) | |
| 7 | PRC (percussion) | |
| 8 | DRM (boucle Serum / loop) | Heuristique uniquement — ONNX couvre 0–7 |

**Ne pas re-classer les slots via AiContentClassifier** — leurs types sont fixés à la
création de scene. Slot 8 = `ContentType::LOOP` en dur.

### 2. Threading — règles strictes

```
Audio thread   → lit atomiques, files SPSC (LockFreeQueue), jamais de malloc/lock
Message thread → écrit états, charge samples, appelle prepare(), UI, timerCallback
Worker threads → ONNX inference, BPM detection, pitch shifting
```

- **Double-buffer Sampler** : GUI écrit dans `data[1-activeDataIdx]`, flip atomique côté audio.
  → Ne jamais écrire dans `data[activeDataIdx]` depuis le GUI.
- **Double-buffer StepSequencer** : `prepareStepBuffer()` écrit dans `swapBufs_[writeSlot]`,
  `flipIfPrepared()` fait le swap atomique dans le callback.
  → Ne jamais modifier `swapBufs_` directement.
- **SpinLock Serum** : `serumSnapLock_` doit protéger tout appel à `FeatureExtractor::extract()`
  dans `timerCallback()`.

### 3. Crossfade adaptatif (SceneManager)

`SceneManager::armAdaptiveCrossfade()` choisit durée + courbe via `chooseProfile()` (public) :

| from → to | Durée | Courbe | Ressenti |
|-----------|-------|--------|---------|
| Musical → Calme | 400 ms | EaseIn (cubique) | fondu dramatique |
| Calme → Musical | 120 ms | EaseOut (cubique) | attaque percutante |
| Musical → Musical | 200 ms | Linéaire | blend propre |
| Calme → Calme | 250 ms | Smoothstep | glissement organique |

Énergie = `SceneManager::computeSceneEnergy(SceneData&)` — 0.0 (silence) à 1.0 (tous slots pleins).
Seuil musical/calme : `kT = 0.20f`.

**Gain floor** : pour les profils lents (Musical→Calme, Calme→Calme), les slots dont le
gain de départ est 0 sont floored à –60 dB (0.001f) pour éviter une attaque molle.
Appliqué dans `applyScene()` avant `armAdaptiveCrossfade`.

**Sidechain automatique** : après `triggerAI()`, kick → bass/pad/synth/loop (max 4 paires)
configuré dans `onTypesDetected`. Guard anti-rebuild si la config n'a pas changé.
API : `Sampler::setSidechainPair(source, target)` / `clearSidechain()` — GUI thread uniquement.

### 4. SerumHost — limitations connues

- `getProgramName(getCurrentProgram())` retourne toujours **"prog 1"** — Serum V2 VST3
  ne remonte pas le nom du preset via l'API standard JUCE.
- `AudioProcessorListener` + parsing d'état (512 premiers octets) implémentés mais
  inefficaces pour Serum : le preset name reste inaccessible pour l'instant.
- → Afficher un message fallback côté UI ; ne pas bloquer dessus.

### 5. Conventions importantes

- **`processAdd` = wet-only additif** : ne pas remplacer le buffer de sortie.
- **Delay sends** : configurés par l'IA (`SmartSamplerEngine::onTypesDetected`), ne pas
  exposer de contrôles manuels pour ces paramètres.
- **Ducking désactivé par défaut** : activer via `dspPipeline_.setDuckingEnabled(true)`.
- **BPM global** : partagé par toutes les scènes. `setBpm()` une seule fois.
- **SynthEffect toujours activé** : volume à 0.0 au démarrage. Utiliser `setParamValue(7, 0.0)`
  pour le silencer, jamais `setEnabled(false)`.
- **Plein écran** : double-clic sur zone vide de l'UI → fullscreen ;
  Escape → retour fenêtre (géré dans `Main.cpp::MainWindow::keyPressed`).
- **`spatialViz_` auto-peuplé** : `applyScene()` appelle `spatialViz_.setSlotState()` en fin
  de fonction — plus besoin de lancer l'IA pour peupler la visualisation spatiale.
- **Magic mix normalise Serum** : après chaque run IA, si Serum est chargé et actif
  (RMS > 0.01), `serumUserGain_` est calé vers −14 dBFS RMS (`kSerumTargetRms = 0.20`),
  clampé à `[0.2, 3.0]`. Persisté dans `sc.serumGain`. Les réglages MIDI learn manuels
  ne sont PAS écrasés entre deux runs IA.

---

## Backlog / blocages connus

| Item | Statut | Piste |
|------|--------|-------|
| Nom preset Serum | Bloqué — API VST3 renvoie "prog 1" | Inspecter state binaire hex (DBG) ou IUnitInfo bas niveau |
| ONNX slot 8 | Heuristique uniquement | Retrain modèle sur 9 slots |
| MIDI CC par paramètre d'effet | Non implémenté | Requiert refactor MidiLearnMap |
| Pitch tracking < 85 Hz | YIN filtré 40 Hz HP | `setMinFrequency()` au risque d'instabilité |
| Serum auto-gain | Implémenté dans `onDone` | `targetGain = 0.20 / rms`, clampé `[0.2, 3.0]` |

---

## Fichiers clés à connaître

| Fichier | Rôle |
|---------|------|
| `src/MainComponent.cpp` | ~2900 lignes — orchestrateur central. Chercher par section commentée `──`. |
| `src/dsp/SceneManager.h` | Scènes + crossfade adaptatif (tout en ligne dans le header) |
| `src/dsp/DspPipeline.cpp` | Pipeline audio stéréo complet |
| `src/dsp/Sampler.h` | Double-buffer, lock-free, stop modes |
| `src/dsp/StepSequencer.h` | Patterns + quantisation bar-boundary |
| `src/dsp/SerumHost.h/.cpp` | Hôte VST3 + AudioProcessorListener |
| `src/ui/ScaleStaffComponent.h/.cpp` | Portée musicale (gammes jouables selon tonalité) |
| `src/ui/Colours.h` | Palette UI (`SaxFXColours::accent`, `aiBadge`, `neonCyan`…) |
| `docs/project-format.md` | Schéma JSON `.saxfx` v8 |

---

## Format projet `.saxfx`

Version courante : **v16** (`"version": 16` en JSON).
Migrations v1→v16 dans `ProjectLoader.cpp`. Ne jamais baisser la version.
Slot guard : rejette `slot >= 9` au chargement.
Migration v16 : tout `userGains[i] < 0.50` sur slot non vide est réinitialisé à 1.0
au chargement (ancienne calibration IA trop basse).
