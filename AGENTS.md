# Guide agent / contributeur — DubEngine (SaxFX Live)

Ce fichier est destiné aux **assistants IA** et aux humains qui arrivent sur le dépôt : il résume l'architecture, les invariants et où intervenir. Le détail fonctionnel et la liste des effets sont dans [README.md](README.md). Le schéma JSON des projets est dans [docs/project-format.md](docs/project-format.md).

## Qu'est-ce que c'est ?

Application **desktop JUCE (C++17)** de performance live **dub techno**. Marque produit **DubEngine** ; binaire CMake / produit **SaxFX Live**.

### Contexte d'usage — à lire avant tout

- **Instrument principal : AKAI EWI** (vent MIDI) branché en entrée audio/MIDI. Il n'y a **plus de saxophone** ni de clavier physique — `KeyboardSynth` et `PianoKeyboardPanel` ont été supprimés définitivement.
- **Le sampler 9 slots est l'instrument central** : boucles, kicks, basses, pads — tout le groove vient de là. L'EffectChain traite le signal EWI en temps réel (effets modulaires).
- **L'utilisateur ne touche pas aux paramètres delay en live.** Les gains, sends, BPM-sync et activation du bus delay sont gérés automatiquement par l'IA ingé son (`SmartSamplerEngine::onTypesDetected`, callback déclenché à chaque chargement de scene). Ne pas ajouter de contrôles manuels pour ces paramètres.
- **`PingPongDelay` et `DubDelay` reçoivent un bus send séparé** (`tempSendL_/R_` dans `DspPipeline`), pas le mix sampler complet. Chaque slot a un `delaySend` atomique (`Sampler::SampleSlot::delaySend`) configuré par l'IA selon le type de contenu (KICK/BASS = 0, PAD = 0.8, etc.).
- **`processAdd` = additif, wet-only.** `dubDelay_.processAdd(sendL, sendR, outL, outR, n)` : le signal dry est déjà dans `outL/outR`, `processAdd` y ajoute uniquement le signal traité. Ne jamais remplacer (`=`) le buffer de sortie dans cette fonction.
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

Artefacts typiques (Release) : `build/SaxFXLive_artefacts/Release/SaxFX Live.exe` (le nom exact peut varier selon `PRODUCT_NAME`).

## Arborescence utile (hors `third_party/`)

| Chemin | Rôle |
|--------|------|
| `src/Main.cpp` | Point d'entrée JUCE |
| `src/MainComponent.h/.cpp` | Fenêtre principale, callback audio `getNextAudioBlock`, liaison UI <-> DSP |
| `src/dsp/` | Pipeline, effets, sampler, séquenceur, YIN, ONNX, limiteur |
| `src/ui/` | Thème neon, composants (rack d'effets, séquenceur, etc.) |
| `src/project/` | `ProjectData`, `ProjectLoader` — sérialisation `.saxfx` |
| `src/midi/` | MIDI |
| `tests/` | Tests Catch2 |
| `models/` | `.onnx` (classifieur, mix) |
| `web/` | Compagnon navigateur (Web Audio), optionnel par rapport au binaire principal |
| `cmake/FindOnnxRuntime.cmake` | Intégration ONNX |

## Fil audio (à ne pas casser)

Référence d'implémentation : `DspPipeline::process` (mono) et `DspPipeline::processStereo` (stéréo).

Ordre logique stéréo (résumé) :

1. Analyse **YIN** et **BPM** sur le canal **gauche** (sax) — ne modifient pas le buffer seuls.
2. **RMS** lissé sur la gauche (VU, expression, ducking éventuel).
3. `std::copy(left -> right)` puis **`EffectChain::processStereo(left, right)`** — vrai stéréo : `ReverbEffect` utilise `juce::dsp::Reverb::processStereo()` (Freeverb filtres peigne séparés L/R), `DelayEffect` fait le ping-pong L->R->L->R (second `RingBuffer` heap-allocated). Les effets sans override utilisent le défaut dual-mono (`process(L); process(R)`), ce qui préserve toute divergence stéréo amont.
4. **ExpressionMapper** — peut pousser un paramètre d'effet selon le RMS.
5. **Sampler** en stéréo (pan / Haas par slot), mixé sur L+R ; **ducking** optionnel (souvent désactivé par défaut côté engine). Bus send par slot (`delaySend` atomique) alimente les delays séparément du mix principal.
6. **MonoSubFilter** (1er ordre 6 dB/oct, fc=120 Hz) — force le contenu sub en mono (PA compat.). Membre `monoSubFilter_` dans `DspPipeline`.
7. **MasterLimiter** sur L et R.

**Synth** : effet **100 % wet** sur l'EWI ; le signal sec disparaît quand le synth est audible — voir `SynthEffect`.

**Slot 8 (DRM)** : modèle ONNX limité à 8 slots ; slot 8 = voie heuristique `ContentType::LOOP` uniquement.

## Threads et synchronisation

- Le callback **audio** doit rester **lock-free** autant que possible : files SPSC pour MIDI -> sampler, atomiques pour flags (`std::atomic`, `memory_order` cohérent).
- **ONNX / inference** : thread dédié (`InferenceThread` etc.) — ne pas bloquer le callback audio sur l'inférence.
- Modifications de chaîne d'effets / gros états : typiquement **message thread** (GUI) + recréation `prepare()` si besoin.
- **`Sampler::loadSample()` / `reloadSlotData()`** : n'utilisent plus `loaded=false` comme garde pendant le swap — le double-buffer garantit qu'on écrit toujours dans le buffer de fond (non lu par les voix actives). `loaded` ne passe à `true` qu'au **premier** chargement d'un slot vide. Les voix en fadeOut (`retriggering=true`) se terminent proprement sans coupure.
- **`Sampler::stop(slot, StopMode)`** : le mode de fade-out est encodé dans `stopPending` (`atomic<int>`, 0 = rien en attente). Modes disponibles : `Normal` (350 ms), `SceneSwap` (20 ms), `Retrigger` (6 ms), `Instant` (0 ms). Consommé par `exchange(0, acq_rel)` dans `process()`/`processStereo()` pour éviter la race load/store. Callsites : `applyScene()` -> `SceneSwap`, `onPlayChanged` -> `Normal`, `onStepChanged` -> `Retrigger`.
- **`StepSequencer::hasPendingTransition()`** : retourne `true` si `pendingTransLen_ > 0` — utilisé dans `navigateScene()` pour bloquer le spam de transitions quantisées.

## Transitions adaptatives entre scènes

`SceneManager::armAdaptiveCrossfade()` remplace `armCrossfade()` dans `applyScene()`.
L'énergie de chaque scène est calculée par `SceneManager::computeSceneEnergy(SceneData&)` :
- Score 0.0–1.0 basé sur densité de pas + mutes (pas d'analyse audio)
- Pré-calculé au chargement du projet ; invalidé à chaque `captureCurrentScene()`
- Seuil musical/calme : `kT = 0.20f`

Durée et courbe du crossfade selon le delta d'énergie :

| Transition | Durée | Courbe |
|-----------|-------|--------|
| Musical → Calme | 400 ms | EaseIn cubique |
| Calme → Musical | 120 ms | EaseOut cubique |
| Musical → Musical | 200 ms | Linéaire |
| Calme → Calme | 250 ms | Smoothstep |

`armCrossfade()` (150 ms linéaire fixe) est conservé comme fallback.

Gain floor –60 dB sur les slots naissants pour profils lents (Musical→Calme, Calme→Calme) :
appliqué dans `applyScene()` avant `armAdaptiveCrossfade`.

`SceneManager::chooseProfile(fromE, toE)` est **public** → testable directement sans instancier
un crossfade. `CrossfadeProfile { int durationMs; CrossfadeCurve curve; }` est aussi public.

Sidechain automatique kick→cibles configuré dans `onTypesDetected` :
priorité BASS > PAD > SYNTH > LOOP, max 4 paires (`Sampler::kMaxSidechainPairs`).
Guard dans `MainComponent` (`lastSidechainKick_` / `lastSidechainTargets_`) évite de rebuilder
si la config n'a pas changé entre deux appels.

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
| Nouvel effet | `IEffect.h`, `EffectFactory`, nouvelle paire `*Effect.cpp/h`, `EffectType`, UI rack / icônes si besoin. Si l'effet a un comportement stéréo (L!=R), surcharger `processStereo()` ; sinon le défaut dual-mono suffit. |
| Pipeline / ordre traitement | `DspPipeline.*`, éventuellement `MainComponent` (routing) |
| Sampler / grille | `Sampler.*`, `StepSequencer.*`, `SmartSamplerEngine.*`, UI `StepSequencerPanel` |
| Sauvegarde projet | `ProjectData.h`, `ProjectLoader.cpp` (migrations **version** JSON), toute UI qui sérialise |
| Thème / boutons | `SaxOsLookAndFeel`, `NeonButton`, `Colours`, `SaxFXLayout` / `SaxFXFonts` |

## Format projet `.saxfx`

- Ecriture actuelle : **`version: 8`** (entier JSON). Chargement : migrations depuis v1+ dans `ProjectLoader::load`.
- Détail des clés : [docs/project-format.md](docs/project-format.md). **Source de vérité** : `ProjectLoader.cpp` + `ProjectData.h`.

## Tests

```bash
cmake --build build --config Release --target SaxFXTests --parallel
```

Exécuter l'exe de tests généré sous `build/tests/Release/` (ou équivalent). **204/204 tests passent** (Catch2, état courant).

## Conventions Git

Voir section **Git Conventions** dans [README.md](README.md) (`type(scope): description`).

## Licence

MIT — voir [LICENSE](LICENSE).
