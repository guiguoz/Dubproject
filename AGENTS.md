# Guide agent / contributeur — DubEngine (SaxFX Live)

Ce fichier est destiné aux **assistants IA** et aux humains qui arrivent sur le dépôt : il résume l'architecture, les invariants et où intervenir. Le détail fonctionnel et la liste des effets sont dans [README.md](README.md). Le schéma JSON des projets est dans [docs/project-format.md](docs/project-format.md).

## Qu'est-ce que c'est ?

Application **desktop JUCE (C++17)** de performance live **dub techno**. Marque produit **DubEngine** ; binaire CMake / produit **SaxFX Live**.

### Contexte d'usage — à lire avant tout

- **Instrument principal : AKAI EWI** (vent MIDI) branché en entrée audio/MIDI. Il n'y a **plus de saxophone** ni de clavier physique — `KeyboardSynth` et `PianoKeyboardPanel` ont été supprimés définitivement.
- **Le sampler 9 slots est l'instrument central** : boucles, kicks, basses, pads — tout le groove vient de là. L'EffectChain traite le signal EWI en temps réel (effets modulaires).
- **L'utilisateur ne touche pas aux paramètres delay en live.** Les gains, sends, BPM-sync et activation du bus delay sont gérés automatiquement par l'IA ingé son via `MixWorker` / `MixEngine` (callback déclenché à chaque chargement de scène). Ne pas ajouter de contrôles manuels pour ces paramètres.
- **`SerumHost` est un hôte VST3 générique** : `loadSerumPlugin(path)` charge n'importe quel VST3 (Serum V2, SWAM Trumpet, etc.). SWAM s'intègre dans le même bus mix et la même IA que Serum — aucun changement DSP requis. Reste dans `src/dsp/SerumHost.h/.cpp` car dépend de JUCE.
- **`PingPongDelay` et `DubDelay` reçoivent un bus send séparé** (`tempSendL_/R_` dans le pipeline audio), pas le mix sampler complet. Chaque slot a un `delaySend` dans `engine::SlotConfig` configuré par l'IA selon le type de contenu (KICK/BASS = 0, PAD = 0.8, etc.).
- **Slot 8 (DRM)** : suit une voie heuristique (`ContentType::LOOP`) — le modèle ONNX de mix ne couvre que les slots 0-7.

## Prérequis build (Windows)

- CMake >= 3.22, MSVC 2022 (ou équivalent).
- Sous-module **JUCE** : `third_party/JUCE` — obligatoire (`git submodule update --init --recursive`).
- **ASIO** (optionnel) : SDK Steinberg dans `third_party/ASIO` ou `-DJUCE_ASIO_SDK_PATH=...` ; sinon **WASAPI**.
- **ONNX** : option `SAXFX_ENABLE_ONNX` (défaut ON) ; runtime récupéré par CMake ; modèles copiés vers le dossier de l'exe depuis `models/`.

## Cibles CMake principales

| Cible | Rôle |
|--------|------|
| `SaxFXLive` | Application graphique |
| `SaxFXTests` | Tests unitaires (Catch2), sous-dossier `tests/` |
| `EngineTests` | Tests moteur V2 (sans JUCE/ONNX), sous-dossier `tests/engine/` |

Artefacts typiques (Release) : `bin/SaxFX Live.exe`, `bin/SaxFXTests.exe`, `bin/EngineTests.exe`.

## Arborescence utile (hors `third_party/`)

| Chemin | Rôle |
|--------|------|
| `src/Main.cpp` | Point d'entrée JUCE |
| `src/MainComponent.h/.cpp` | Fenêtre principale, callback audio `getNextAudioBlock`, liaison UI <-> DSP |
| `src/dsp/SerumHost.h/.cpp` | Hôte VST3 (Serum, SWAM, etc.) — seul fichier restant dans `src/dsp/` |
| `src/engine/` | Moteur V2 complet : `EngineFacade`, `SceneStore`, `TransitionEngine`, `SlotPlayer`, `AudioGraph`, `Sequencer`, mix (`engine/mix/`), analyse (`engine/Analysis/`), effets (`engine/fx/`) |
| `src/ui/` | Thème neon, composants (rack d'effets, séquenceur, etc.) |
| `src/project/` | `ProjectData`, `ProjectLoader` — sérialisation `.saxfx` |
| `src/midi/` | MIDI (`MidiManager`, `MidiNoteMapper`) |
| `tests/` | Tests Catch2 (SaxFXTests) |
| `tests/engine/` | Tests moteur V2 (EngineTests, sans JUCE) |
| `models/` | `.onnx` (classifieur, mix) |
| `web/` | Compagnon navigateur (Web Audio), optionnel |
| `cmake/FindOnnxRuntime.cmake` | Intégration ONNX |

## Fil audio (à ne pas casser)

Référence : `EngineFacade` → `AudioGraph` → `SlotPlayer` (V2).

Ordre logique stéréo :

1. **Transport** : `engine::Transport` dérive la position (samples, beat, BPM) depuis l'horloge JUCE.
2. **Sequencer** : `engine::Sequencer` lit le pattern (double-buffer `StepBuf`) et déclenche/stope les slots.
3. **SlotPlayer** × 9 : lecture stéréo avec pan, Haas, trim, gain, mute par slot.
4. **EffectChain** : effets modulaires sur le signal EWI (reverb, delay, etc.).
5. **Mix** : `engine::mix::MixEngine` (heuristique) ou `MixAi` (inférence ONNX) calcule gain/spatial par slot.
6. **MonoSubFilter** (1er ordre 6 dB/oct, fc=120 Hz) — force le contenu sub en mono.
7. **MasterLimiter** sur L et R.

## Threads et synchronisation

- Le callback **audio** doit rester **lock-free** autant que possible : atomiques pour flags (`std::atomic`, `memory_order` cohérent).
- **ONNX / inference** : thread dédié — ne pas bloquer le callback audio sur l'inférence.
- **Magic mix** : `MixWorker` s'exécute sur un thread dédié,applique l'état (`MixState`) sur le message thread.
- Modifications de chaîne d'effets / gros états : typiquement **message thread** (GUI) + recréation `prepare()` si besoin.

## Transitions adaptatives entre scènes

`SceneStore::armAdaptiveCrossfade()` est appelé depuis `applyScene()` dans MainComponent.
L'énergie de chaque scène est calculée par `engine::SceneEnergy::compute(engine::SceneData)` (`src/engine/SceneEnergy.h`) :
- Score 0.0–1.0 basé sur densité de pas + mutes (pas d'analyse audio)
- Pré-calculé au chargement du projet (`applyProjectData`, sur le SceneStore après `syncV2Scenes()`) ; invalidé à chaque `captureCurrentScene()`
- Seuil musical/calme : `kT = 0.15f`

Durée et courbe du crossfade selon le delta d'énergie :

| Transition | Durée | Courbe |
|-----------|-------|--------|
| Musical → Calme | 600 ms | EaseIn cubique |
| Calme → Musical | 80 ms | EaseOut cubique |
| Musical → Musical | 250 ms | Linéaire |
| Calme → Calme | 350 ms | Smoothstep |

`armCrossfade()` (150 ms linéaire fixe) est conservé comme fallback.

`SceneStore::chooseProfile(fromE, toE)` est **public** → testable directement sans instancier
un crossfade. `CrossfadeProfile { int durationMs; CrossfadeCurve curve; }` est aussi public.

**PingPongDelay morphing** : `SceneStore::startDubDelayMorph(from, to, 4000f)` démarre
un morphing de 4 s des paramètres delay (feedback, wet, tone, drive) entre deux scènes.
`updateMorph()` avance le compteur (tick 33 ms depuis `timerCallback`). Params stockés dans `SceneData`
et persistés dans `.saxfx` v20 (`dubDelayFeedback/Wet/Tone/Drive` par scène).

## Zone info musicale (au-dessus du step sequencer)

Deux panneaux ajoutés (`kInfoZoneH = 180 px`) :
- **Gauche — SERUM** : affiche le nom du preset Serum courant (limité par API VST3 — voir CLAUDE.md backlog)
- **Droite — portée** : `ui::ScaleStaffComponent` dessine les notes jouables (clé de sol, ellipses néon) pour la tonalité + gamme sélectionnées. Clé de sol via "Segoe UI Symbol" U+1D11E.

`ScaleStaffComponent::setKey(int root, ScaleType)` → `rebuildNoteInfos()` précalcule les positions de chaque note. `paint()` ne recalcule rien. Types de gamme : `Major, Minor, PentatonicMaj, PentatonicMin, Blues, Dorian`.

## Plein écran

- **Double-clic** sur zone vide de `MainComponent` → `getPeer()->setFullScreen(!isFullScreen())`
- **Escape** → `getPeer()->setFullScreen(false)` — intercepté dans `Main.cpp::MainWindow::keyPressed()` (remonte si aucun enfant ne consomme la touche)

## Fichiers souvent touchés par type de changement

| Besoin | Fichiers / zones |
|--------|-----------------|
| Nouvel effet | `IEffect.h`, `EffectFactory`, nouvelle paire `*Effect.cpp/h`, `EffectType`, UI rack / icônes si besoin. |
| Pipeline / ordre traitement | `AudioGraph.*`, `SlotPlayer.*`, `EngineFacade.*` |
| Sampler / grille | `Sequencer.*`, `SlotPlayer.*`, UI `StepSequencerPanel` |
| Mix / magic mix | `engine/mix/` (MixEngine, MixAi, MixWorker, MixState), `AutoMixDub.*` |
| Sauvegarde projet | `ProjectData.h`, `ProjectLoader.cpp` (migrations **version** JSON), `SceneStore.h` |
| Thème / boutons | `SaxOsLookAndFeel`, `NeonButton`, `Colours`, `SaxFXLayout` / `SaxFXFonts` |

## Format projet `.saxfx`

- Ecriture actuelle : **`version: 20`** (entier JSON). Chargement : migrations depuis v1+ dans `ProjectLoader::load`.
- Détail des clés : [docs/project-format.md](docs/project-format.md). **Source de vérité** : `ProjectLoader.cpp` + `ProjectData.h`.

## Tests

```bash
cmake --build build --config Release --target SaxFXTests --parallel
cmake --build build --config Release --target EngineTests --parallel
```

**SaxFXTests** : 35 test cases (Catch2). **EngineTests** : 86 test cases (sans JUCE/ONNX).

**Null-test (§11.2)** : `renderOffline()` (`src/engine/OfflineRender.*`) rend une session déterministe complète (séquenceur + transitions + AutoMix v2.0 simulé toutes les 50 ms) sans device. Le test `[nulltest]` (NULL1) compare le rendu de la fixture (4 scènes, 6 samples, 2 transitions) bit à bit à une référence committée (hash FNV-1a + RMS par fenêtre). TOUT commit doit le passer ; un changement de rendu INTENTIONNEL régénère la référence (test caché NULL0) dans le même commit, avec justification.

## Conventions Git

Voir section **Git Conventions** dans [README.md](README.md) (`type(scope): description`).

## Licence

MIT — voir [LICENSE](LICENSE).
