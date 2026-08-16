# Phase 4: Scene/Métier Binding via Facade APIs

## Goal
Rediriger tous les appels V1 (`sceneManager_`, `stepSequencer_`, `dspPipeline_.getSampler()`) 
vers les APIs façade. Cela libère la voie pour éliminer complètement les V1 objets en Phase 5.

---

## ✅ Phase 4a — Fait (EngineFacade.h/.cpp, non commité)

APIs Tier 1/2/3 implémentées sur `EngineFacade` :
- Transition : `pendingSceneIdx`, `setPendingScene`, `consumePendingScene`, `hasPendingScene`
- Fin de scène quantisée : `hasPendingTransition`, `setPendingTransitionLen`, `consumeSceneEnd`
- Morphing delay : `startDubDelayMorph`, `updateMorphing`, `isMorphing`, `getMorphProgress`,
  `getMorphFromSceneIdx`, `getMorphToSceneIdx`
- Énergie : `setSceneEnergy`, `getSceneEnergy`
- Helpers : `prepareStepBuffer(::dsp::StepSequencer::StepBuf)`, `stopAllSlots(::dsp::Sampler::StopMode)`

**Signal « fin de scène » (décision prise)** : le V1 `stepSequencer_.process()` est mort
(commit e9a2399) → `consumeSceneEnd()` V1 ne se déclenchait plus jamais. La façade détecte
désormais la frontière dans `processBlock()` via `TransitionEngine` : au passage
`Armed→Executing`, elle pose `sceneEndFlag_` et efface `pendingTransLen_`.

**Fix build** : `EngineFacade.h` incluait `dsp/StepSequencer.h`/`dsp/Sampler.h` dont les
inline (`juce::jlimit`) cassaient la compilation avant `<JuceHeader.h>`. Correctifs :
include `<JuceHeader.h>` en tête de `EngineFacade.h`, et qualification `::dsp::` (ambiguïté
avec `juce::dsp` sous `using namespace juce`).

**Vérifié** : SaxFXLive link (`bin/SaxFX Live.exe`), 84/84 tests, EngineTests 81/81,
NULL1 bit-exact (`0xa00f0dd7fb70bbd0`).

## Audit: Appels V1 Actuels (108 totaux)

### sceneManager_ (~40 appels)
- **Index & mutation**: `currentIdx()`, `setCurrentIdx()` ← façade expose `currentSceneIdx()`, `setCurrentScene()`
- **Accès scene**: `scene(idx)` ← façade expose `scene(idx)` ✓
- **Pending**: `pendingIdx()`, `setPendingScene()`, `consumePendingScene()` ← **MANQUE**
- **Transition**: `hasPendingTransition()` ← **MANQUE** (step sequencer, not scene manager)
- **Morphing**: `isMorphing()`, `getMorphProgress()`, `startDubDelayMorph()`, `updateMorph()` ← **MANQUE**
- **Energy**: `setSceneEnergy()`, `computeSceneEnergy()` ← **MANQUE**
- **Morph query**: `getScene()`, `getMorphFromScene()`, `getMorphToScene()` ← **MANQUE**

### stepSequencer_ (~30 appels)
- **Transport**: `setBpm()`, `getBpm()` ← façade expose ✓
- **Swing**: `setSwing()`, `getSwing()` ← façade expose ✓
- **Pattern**: `setStep()`, `getStep()`, `getTrackStepCount()`, `setTrackBarCount()`, `getTrackBarCount()` ← façade expose ✓
- **Playback**: `isPlaying()`, `setPlaying()` ← façade expose `isPlaying()`, `play()`, `stop()` ✓
- **Playhead**: `getCurrentPhase()` ← façade expose ✓
- **Transition**: `hasPendingTransition()`, `setPendingTransitionLen()`, `consumeSceneEnd()` ← **MANQUE**
- **Pattern buffer**: `prepareStepBuffer()` ← **MANQUE**
- **Initialization**: `prepare(sampleRate)` ← **MANQUE** (vérifie si nécessaire avec façade.prepare)

### Sampler (~20 appels via dspPipeline_.getSampler())
- **Gains**: `getSlotGain()`, `setSlotGain()` ← façade expose ✓
- **Mute**: `isSlotMuted()`, `setSlotMuted()` ← façade expose ✓
- **Clear**: `clearSlot()` ← façade expose ✓
- **Stop all**: `stopAllSlots()` ← **MANQUE**

---

## APIs Manquantes à Exposer sur EngineFacade

### Tier 1: Critique (débloque majorité des appels)
1. `pendingSceneIdx()` — lire index scène en attente
2. `setPendingScene(idx)` — marquer scène pour transition
3. `consumePendingScene()` — consommer et appliquer la transition
4. `hasPendingTransition()` — vérifier si transition en cours
5. `setPendingTransitionLen(len)` — durée transition
6. `consumeSceneEnd()` — vérifier si end-of-scene atteint

### Tier 2: Morphing & Énergie
7. `startDubDelayMorph(from, to, durationMs)` — morphing delay param
8. `updateMorphing()` — step timer (50ms callback)
9. `isMorphing()` — vérifier état morphing
10. `getMorphProgress()` — retourner [0..1] progress
11. `setSceneEnergy(idx, energy)` — stocker énergie calculée
12. `computeSceneEnergy(scene)` — calculer depuis SceneData

### Tier 3: Patch/Utilities
13. `getSceneAt(idx)` — accès read-only (vs `scene(idx)` qui est read-write)
14. `getMorphFromSceneIdx()`, `getMorphToSceneIdx()` — pour UI
15. `stopAllSlots(mode)` — arrêter tous les slots
16. `prepareStepBuffer(buf)` — préc. buffer transition (si nécessaire avec V2)

---

## Stratégie Implémentation

### Phase 4a: Tier 1 APIs (Critique)
- Expose depuis `SceneManager` et `StepSequencer` internes
- Implémente sur `EngineFacade` sans logique complexe (delegation uniquement)
- Test: Compile sans erreurs

### Phase 4b: Redirect Appels MainComponent 
- Remplace ~40 appels sceneManager_ par façade
- Remplace ~10 appels stepSequencer_ (transition-related)
- Remplace ~5 appels sampler `stopAllSlots()`
- Test: Compile, audio reste identique

### Phase 4c: Tier 2 APIs (Morphing)
- Expose morphing API si utilisée
- Implémente callback update
- Test: Scène morphing + delay morphing fonctionnent

### Phase 4d: Cleanup & Validation
- Audit appels V1 restants (devraient être << 20)
- Vérifier que seul métadonnées de scène accèdent directement SceneData
- Plan Phase 5 purge

---

## Files V1 à Garder (après Phase 4)
- `stepSequencer_` — métadonnées globales uniquement (BPM courant, swing, patterns)
- `sceneManager_` — storage SceneData seulement
- `dspPipeline_.getSampler()` — **À PENSER**: remplacer par façade ou garder?

Goal: Phase 5 = supprimer tout sauf métadonnées essentielles.

---

## Comptage réel (audit exhaustif MainComponent.cpp, 2903 lignes)

| Objet | Total | RÉPLACE (façade) | A GARDER (Data/storage V1) | V2-À-ÉCRIRE |
|-------|-------|------------------|----------------------------|-------------|
| `sceneManager_.` | 67 | 28 | 39 (accès `dsp::SceneData`) | 2 × `computeSceneEnergy` statique (à porter sur `engine::SceneData`) |
| `stepSequencer_.` | 43 | 43 (tous) | 0 | 0 (⚠️ `prepare` L1342 supprimable ; ⚠️ sémantique buffer flip quantisé vs flip immédiat) |
| `dspPipeline_.getSampler()` | 14 (20 appels) | 15 | 3 (loadSample/setSlotOneShot/setSlotLoop — alimentation IA V1) | 2 (setSlotPan/HaasDelay → couvert par `setSlotMixState` déjà appelé) |
| `dspPipeline_.` hors sampler | 4 | 3 (setBpm ×2, resetAllDelays) | 1 (getMidiEventQueue, header) | 0 |

### Points d'attention Phase 4b
1. `scene(idx)` co-occurrent avec `currentIdx()` : ne remplacer que l'indice par
   `facade_.currentSceneIdx()` si la synchro `applyScene`→`setCurrentScene` est garantie.
2. `setCurrentIdx` (1223, 2158, 2740) : `facade_.setCurrentScene(idx)` applique le graphe ;
   retirer le doublon dans `applyScene` (L2522) lors du remplacement.
3. Lecture pattern : V1 = buffer actif post-flip quantisé ; V2 = `writePatterns_` invarié
   par `prepareStepBuffer`. La capture (L2380) doit rester AVANT tout `prepareStepBuffer`.
4. `stopAllSlots` façade ignore le `mode` (absorbé par la file de stops audio).

## Estimated Changes
- EngineFacade.h/.cpp : ✅ fait (Tier 1/2/3)
- MainComponent.cpp : ~75 replacements (28 sceneManager_ + 43 stepSequencer_ + sampler) — Phase 4b
- Test: 2-3 compiles, commits séparés par tier
