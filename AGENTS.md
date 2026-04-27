# Guide agent / contributeur — DubEngine (SaxFX Live)

Ce fichier est destiné aux **assistants IA** et aux humains qui arrivent sur le dépôt : il résume l’architecture, les invariants et où intervenir. Le détail fonctionnel et la liste des effets sont dans [README.md](README.md). Le schéma JSON des projets est dans [docs/project-format.md](docs/project-format.md).

## Qu’est-ce que c’est ?

Application **desktop JUCE (C++17)** : traitement audio temps réel pour saxophone live (effets modulaires, sampler 9 pistes + séquenceur jusqu’à 512 steps, MIDI, IA ONNX pour classification de samples et mix assisté). Marque produit **DubEngine** ; binaire CMake / produit **SaxFX Live**.

## Prérequis build (Windows)

- CMake ≥ 3.22, MSVC 2022 (ou équivalent).
- Sous-module **JUCE** : `third_party/JUCE` — obligatoire (`git submodule update --init --recursive`).
- **ASIO** (optionnel) : SDK Steinberg dans `third_party/ASIO` ou `-DJUCE_ASIO_SDK_PATH=...` ; sinon **WASAPI**.
- **ONNX** : option `SAXFX_ENABLE_ONNX` (défaut ON) ; runtime récupéré par CMake ; modèles copiés vers le dossier de l’exe depuis `models/`.

## Cibles CMake principales

| Cible | Rôle |
|--------|------|
| `SaxFXLive` | Application graphique |
| `SaxFXTests` | Tests unitaires (Catch2), sous-dossier `tests/` |

Artefacts typiques (Release) : `build/SaxFXLive_artefacts/Release/SaxFX Live.exe` (le nom exact peut varier selon `PRODUCT_NAME`).

## Arborescence utile (hors `third_party/`)

| Chemin | Rôle |
|--------|------|
| `src/Main.cpp` | Point d’entrée JUCE |
| `src/MainComponent.h/.cpp` | Fenêtre principale, callback audio `getNextAudioBlock`, liaison UI ↔ DSP |
| `src/dsp/` | Pipeline, effets, sampler, séquenceur, YIN, ONNX, limiteur |
| `src/ui/` | Thème neon, composants (rack d’effets, séquenceur, etc.) |
| `src/project/` | `ProjectData`, `ProjectLoader` — sérialisation `.saxfx` |
| `src/midi/` | MIDI |
| `tests/` | Tests Catch2 |
| `models/` | `.onnx` (classifieur, mix) |
| `web/` | Compagnon navigateur (Web Audio), optionnel par rapport au binaire principal |
| `cmake/FindOnnxRuntime.cmake` | Intégration ONNX |

## Fil audio (à ne pas casser)

Référence d’implémentation : `DspPipeline::process` (mono) et `DspPipeline::processStereo` (stéréo).

Ordre logique stéréo (résumé) :

1. Analyse **YIN** et **BPM** sur le canal **gauche** (sax) — ne modifient pas le buffer seuls.
2. **RMS** lissé sur la gauche (VU, expression, ducking éventuel).
3. **EffectChain** — traitement **in-place sur le canal gauche** ; le pitch passé aux effets peut être forcé (clavier) ou issu de YIN.
4. **ExpressionMapper** — peut pousser un paramètre d’effet selon le RMS.
5. Copie **gauche → droite** pour le sax traité (signal mono étendu en L/R identiques pour la partie live — voir code actuel).
6. **Sampler** en stéréo (pan / Haas par slot), mixé sur L+R ; **ducking** optionnel (souvent désactivé par défaut côté engine).
7. **MonoSubFilter** (1er ordre 6 dB/oct, fc=120 Hz) — force le contenu sub en mono (PA compat.). Membre `monoSubFilter_` dans `DspPipeline`.
8. **MasterLimiter** sur L et R.

**Synth** : effet **100 % wet** sur le sax ; le sax sec disparaît quand le synth est audible — voir `SynthEffect` et README section Synth.

**Slot sampler 8 (DRM)** : le modèle ONNX de mix est **8 slots** ; le slot 8 suit une **voie heuristique** (`ContentType::LOOP`), pas le réseau 8-slot.

## Threads et synchronisation

- Le callback **audio** doit rester **lock-free** autant que possible : files SPSC pour MIDI → sampler, atomiques pour flags (`std::atomic`, `memory_order` cohérent).
- **ONNX / inference** : thread dédié (`InferenceThread` etc.) — ne pas bloquer le callback audio sur l’inférence.
- Modifications de chaîne d’effets / gros états : typiquement **message thread** (GUI) + recréation `prepare()` si besoin.
- **`Sampler::loadSample()` / `reloadSlotData()`** : n’utilisent plus `loaded=false` comme garde pendant le swap — le double-buffer garantit qu’on écrit toujours dans le buffer de fond (non lu par les voix actives). `loaded` ne passe à `true` qu’au **premier** chargement d’un slot vide. Les voix en fadeOut (`retriggering=true`) se terminent proprement sans coupure.

## Fichiers souvent touchés par type de changement

| Besoin | Fichiers / zones |
|--------|-------------------|
| Nouvel effet | `IEffect.h`, `EffectFactory`, nouvelle paire `*Effect.cpp/h`, `EffectType`, UI rack / icônes si besoin |
| Pipeline / ordre traitement | `DspPipeline.*`, éventuellement `MainComponent` (routing) |
| Sampler / grille | `Sampler.*`, `StepSequencer.*`, `SmartSamplerEngine.*`, UI `StepSequencerPanel` |
| Clavier / synthé solo | `KeyboardSynth.*`, `DspPipeline.*` (intégration SPSC noteOn/noteOff), `PianoKeyboardPanel.h` |
| Sauvegarde projet | `ProjectData.h`, `ProjectLoader.cpp` (migrations **version** JSON), toute UI qui sérialise |
| Thème / boutons | `SaxOsLookAndFeel`, `NeonButton`, `Colours`, `SaxFXLayout` / `SaxFXFonts` |

## Format projet `.saxfx`

- Écriture actuelle : **`version: 8`** (entier JSON). Chargement : migrations depuis v1+ dans `ProjectLoader::load`.
- Détail des clés : [docs/project-format.md](docs/project-format.md). **Source de vérité** : `ProjectLoader.cpp` + `ProjectData.h`.

## Tests

```bash
cmake --build build --config Release --target SaxFXTests --parallel
```

Exécuter l’exe de tests généré sous `build/tests/Release/` (ou équivalent). 206 tests (Catch2) ; 203/206 passent (3 échecs pré-existants : encoding de noms de tests + check version).

## Conventions Git

Voir section **Git Conventions** dans [README.md](README.md) (`type(scope): description`).

## Licence

MIT — voir [LICENSE](LICENSE).
