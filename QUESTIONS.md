# QUESTIONS.md

Toutes les questions M1-M7 sont closes.

---

## M8a — Surface UI inventoriée (299 appels DSP)

Objets à remplacer via EngineFacade :

| Ancien objet | V2 équivalent |
|---|---|
| `dspPipeline_.getSampler()` | `EngineFacade::slot*()` → SlotPlayer |
| `dspPipeline_.processStereo/process()` | `EngineFacade::processBlock()` → AudioGraph |
| `dspPipeline_.getDubDelay()` | `EngineFacade::delay()` → engine::fx::PingPongDelay |
| `dspPipeline_.getMasterLimiter()` | interne AudioGraph |
| `stepSequencer_.*` | `EngineFacade::sequencer*()` → Sequencer |
| `sceneManager_.*` | `EngineFacade::scene*()` → SceneStore + TransitionEngine |
| `samplerEngine_.*` | `EngineFacade::import*()` → ImportPipeline + AutoMixDub |
| `looperEngine_.*` | **DÉSACTIVÉ** (§10.4) — UI masquée |
| `serumHost_.*` | bridgé tel quel (JUCE deps — reste dans src/dsp/) |

Décisions M8b :
- `DUB_ENGINE_V2` flag dans CMakeLists.txt + MainComponent
- LooperEngine : tous les appels masqués sous `#ifndef DUB_ENGINE_V2`
- SmartSamplerEngine : importAsync() → ImportPipeline, mix → AutoMixDub
- SerumHost : reste src/dsp/, `EngineFacade` garde un pointeur injecté
- `autoMatchSampleAsync` : remplacé par `EngineFacade::importSampleAsync()`
