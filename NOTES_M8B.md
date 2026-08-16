# M8b — Audit fil audio et points de ré-câblage

## État actuel (HEAD = f1d1e8a)
- ✔ `facade_.processBlock()` déjà appelé ligne 1492 (fil audio branchéV2)
- ❌ `stepSequencer_.process()` ligne 1421 — DEAD (n'est que pour UI getCurrentStep, mais le V2 le gère)
- ❌ `dspPipeline_.getSampler()` à 13 sites — getter PCM et paramètres (DEAD dans le fil audio)
- ❌ `sceneManager_.*` à 30+ sites — mélange UI et métier

## Grep results summary (113 matches)
```
Line 1421: stepSequencer_.process(numSamples, dspPipeline_.getSampler());  ← DEAD CODE
Line 449:  cb.gain = dspPipeline_.getSampler().getSlotGain(slot);        ← UI binding (clip panel)
Line 450:  cb.muted = dspPipeline_.getSampler().isSlotMuted(slot);      ← UI binding (clip panel)
Line 475:  dspPipeline_.getSampler().setSlotGain(slot, cb.gain);        ← UI binding (paste)
Line 476:  dspPipeline_.getSampler().setSlotMuted(slot, cb.muted);      ← UI binding (paste)
... and 100+ more sceneManager/stepSequencer/sampler getters/setters
```

## Points de ré-câblage (triés par criticité)

### CRITIQUE — Fil audio
1. **L1421** : `stepSequencer_.process()` → **DELETE** (séquenceur V2 en marche, cet appel n'émet rien)

### HIGH — UI binding (panels de lecture)
2. **SamplerPanel** : getters V1 sampler (gain, mute, waveform, etc.) → **map to `facade_.getSlot*()`**
3. **StepSequencerPanel** : getters V1 sequencer (bar count, step state, phase) → **map to `facade_.getTrack*()`**

### MEDIUM — Scènes et métier
4. **sceneManager_.currentIdx()/setCurrentIdx()** → **facade_.setCurrentScene(idx)**
5. **stepSequencer_.setBpm()** → **facade_.setBpm()**
6. **dspPipeline_.getBpm()** → **facade_.getBpm()**
7. **Scene save/load** : `stepSequencer_.getStep/setStep`, `sceneManager_.scene()` accessors → port façade

### LOW — Helpers UI/comportement
8. `SerumHost` preset name → move to facade (already done? check)
9. Delay morph params → via `facade_.getDubDelayMorph()`

## Façade V2 API disponible (vérifier dans EngineFacade.h)
- `processBlock(left, right, numSamples, ...)`
- `setCurrentScene(idx)` / `getCurrentScene()`
- `setBpm(bpm)` / `getBpm()`
- `setStep(track, step, active)` / `getStep(track, step)`
- `setTrackBarCount(track, bars)` / `getTrackBarCount(track)`
- `setSlotGain(slot, gain)` / `getSlotGain(slot)` ← **exists?**
- `setSlotMute(slot, muted)` / `isSlotMuted(slot)` ← **exists?**
- Waveform, RMS, detected type, magic mix state → **audit en cours**

## Décision de phasing
- **Phase 1 : KILL stepSequencer_.process()** — commit séparé, smallest diff
- **Phase 2 : UI panel binding** — chaque panel rebranché sequentially (SamplerPanel, StepSequencerPanel)
- **Phase 3 : Scène/métier** — save/load, navigation, BPM
- **Phase 4 : Purge V1** — src/dsp/ supprimé, null-test régénéré

## Phasing détaillé (commits typés)

### Phase 1 : Expose getCurrentStep/Phase dans façade (commit façade)
- Ajouter `getCurrentStep()` → `facade_.sequencer_.getCurrentStep(transport_.state())`
- Ajouter `getCurrentPhase()` → fractional phase [0..1] du step courant
- Pas de changement audio, juste API V2 utile pour l'UI

### Phase 2 : Switch StepSequencerPanel → façade (commit UI)
- SamplerPanel : référence `seq_` → `facade_` (via MainComponent callback)
- StepSequencerPanel : `seq_.getCurrentStep()` → `facade_.getCurrentStep()`
- StepSequencerPanel : `seq_.getCurrentPhase()` → `facade_.getCurrentPhase()`
- V1 sequencer `.process()` n'est plus appelé après ce changement

### Phase 3 : DELETE L1421 stepSequencer_.process() (commit audio)
- Remove ligne 1421 dans getNextAudioBlock()
- Petit diff, très safe
- V2 sequencer (via façade.processBlock) gère déjà tous les triggers

### Phase 4 : Scene/métier binding
- Scene save/load via façade APIs
- Transitions, préparation des patterns

### Phase 5 : Scène purge V1 (après tests complets)
- Une fois toutes les UI panels re-câblées
- Créer le null-test final
- Supprimer src/dsp/ en bulk

## Actionables (start now)
- [ ] **Phase 1** : Add `getCurrentStep()` / `getCurrentPhase()` à `EngineFacade` + `Sequencer`
- [ ] Test compile
- [ ] **Phase 2** : Update StepSequencerPanel binding → commit séparé
- [ ] **Phase 3** : Delete L1421 → commit séparé
- [ ] Itérer jusqu'à null-test pass
