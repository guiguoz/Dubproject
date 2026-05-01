# Plan — SceneManager v2 : Corrections P0/P1 — IMPLÉMENTÉ

**État :** ✅ Sprint P0/P1 terminé

---

## Ce qui a été fait

### P0.3 — Bloquer spam `navigateScene()` ✅
- `StepSequencer::hasPendingTransition()` ajouté — retourne `true` si `pendingTransLen_ > 0`
- `navigateScene()` retourne immédiatement si une transition est déjà armée

### P1.1 + P1.2 — `StopMode` enum ✅

`Sampler::StopMode` remplace le setter global `setStopFadeOutMs()` :

```cpp
enum class StopMode : int {
    Normal    = 1,  // 350 ms — extinction douce (pads, basses)
    SceneSwap = 2,  // 20 ms  — coupure nette avant crossfade de scene
    Retrigger = 3,  // 6 ms   — choke rapide (hihat, perc)
    Instant   = 4   // 0 ms   — coupure immediate
};
void stop(int slot, StopMode mode = StopMode::Normal) noexcept;
void stopAllSlots(StopMode mode = StopMode::Normal) noexcept;
```

- `stopPending` : `atomic<bool>` → `atomic<int>` (encode le mode, 0 = rien en attente)
- `stopFadeSamples(mode)` calcule la durée une seule fois au moment du stop (private helper)
- `exchange(0, acq_rel)` au lieu de `load` + `store` séparés → élimine la race condition P1 point 1
- `StopMode::Instant` : `playing = false` direct, pas de tour inutile de retriggering
- Callsites dans `MainComponent` :
  - `applyScene()` → `stopAllSlots(StopMode::SceneSwap)` — 20 ms, une ligne propre
  - `onPlayChanged` → `stopAllSlots(StopMode::Normal)` — 350 ms explicite
  - `onStepChanged` → `stop(track, StopMode::Retrigger)` — 6 ms inertie step

---

## P0.1 / P0.2 — Non applicables en l'état

Ces corrections concernent un `SceneManager` avec loader thread dédié qui n'existe pas encore.
`applyScene()` s'exécute entièrement sur le GUI thread (appelé depuis `timerCallback()`) → pas de race.
À implémenter lors du sprint SceneManager v2 complet.

---

## Reste à faire (SceneManager v2 complet)

- [ ] `SwapPacket` avec `changed[9]` (pas de string côté audio)
- [ ] `StepSequencer::preparedBuf_` + `flipIfPrepared()`
- [ ] Loader thread dédié + mailbox last-wins
- [ ] `processBarBoundary()` audio thread
- [ ] Crossfade 300 ms uniquement sur slots inchangés (Décision D3)
- [ ] P0.1 snapshot `audioFilePaths_` avant loader job
- [ ] P0.2 callbacks loader sans UI

---

## Décisions de design retenues (v2)

### D0 — Politique "1 transition en vol"
Bloquer `prepare()` si transition déjà armée. Suffisant avec 2 buffers.

### D1 — `loadPcm/clearSlot` = DSP only
Aucun appel UI dans les callbacks loader. UI update via `onSceneApplied` dans `timerCallback()`.

### D2 — `StopMode` au lieu de setter global ✅ FAIT
Durées fixes par mode, choisies au callsite.

### D3 — Crossfade différencié par slot
- Slots **inchangés** : ramp externe 300 ms (continuité parfaite)
- Slots **changés** : `StopMode::SceneSwap` (20 ms) + gain direct vers cible (pas de ramp 300 ms)

### D4 — `exchange(0, acq_rel)` pour consommer `stopPending` ✅ FAIT
Élimine la race load/store entre GUI thread et audio thread.
