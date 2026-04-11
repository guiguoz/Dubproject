# Format de fichier projet SaxFX Live / DubEngine (`.saxfx`)

Les projets sont des fichiers **JSON** UTF-8 avec l’extension `.saxfx`. La **source de vérité** du schéma est le code : `src/project/ProjectLoader.cpp` (sauvegarde / chargement / migrations) et `src/project/ProjectData.h` (modèle C++).

## Version actuelle (écriture)

À l’enregistrement, le champ racine **`version`** est toujours **`8`** (nombre entier JSON).

Les fichiers plus anciens (1 … 7) sont **chargés** et migrés en mémoire par `ProjectLoader::load` ; à la prochaine sauvegarde ils passent en v8.

### Table des versions (résumé)

| Version | Éléments notables |
|---------|-------------------|
| 1 | Ancien format plat (`effects`, `harmonizer`, …) — migré vers v2 en mémoire au load |
| 2 | Tableau `effectChain` |
| 3 | `aiManaged` par effet ; champs `muted`, `gridDiv` sur les samples ; `musicContext` |
| 4 | `bpm` racine ; `samples[].steps` (16 booléens) par slot |
| 5 | `masterKeyRoot`, `masterKeyMajor`, `slotMix`, `scenes` (8 scènes), `currentScene` |
| 6 | Par scène : `trackBarCounts[9]` (1–32 barres), grilles `steps` jusqu’à 512 booléens par piste |
| 7 | `trimStart` / `trimEnd` par slot dans chaque scène |
| 8 | Même schéma que v7 côté champs ; **garde chargement** `slot >= 9` rejeté sur les samples ; numéro de format incrémenté |

## Structure JSON v8 (vue d’ensemble)

Racine :

| Clé | Type | Description |
|-----|------|-------------|
| `version` | `number` | Toujours `8` à l’écriture |
| `projectName` | `string` | Nom du projet |
| `bpm` | `number` | Tempo **maître** global (float) |
| `effectChain` | `array` | Effets dans l’ordre de la chaîne |
| `samples` | `array` | Slots sampler actifs (seulement les entrées avec fichier) |
| `midiMappings` | `array` | Liaisons note MIDI → slot |
| `musicContext` | `object` | Contexte musical détecté / style (voir ci-dessous) |
| `masterKeyRoot` | `number` | 0=C … 11=B |
| `masterKeyMajor` | `bool` | Mode majeur / mineur |
| `currentScene` | `number` | Index de scène courante (0–7) |
| `slotMix` | `array` | États **Magic mix** (un objet par slot concerné, seulement si `applied`) |
| `scenes` | `array` | Scènes utilisées (`used: true`) |

### `effectChain[]`

| Clé | Type |
|-----|------|
| `type` | `string` — nom aligné sur `dsp::effectTypeName()` |
| `enabled` | `bool` |
| `aiManaged` | `bool` |
| `params` | `array` de `number` — un float par index de paramètre |

### `samples[]` (entrées avec `path` non vide)

| Clé | Type |
|-----|------|
| `slot` | `number` — **0 … 8** (9 pistes) |
| `path` | `string` — chemin fichier WAV |
| `gain`, `loop`, `oneShot`, `muted`, `gridDiv` | types évidents |
| `steps` | `array` de 16 `bool` — motif “legacy” par slot (voir scènes pour longueur complète) |

### `midiMappings[]`

| Clé | Type |
|-----|------|
| `note` | `number` |
| `slot` | `number` |

### `musicContext`

| Clé | Type |
|-----|------|
| `bpm` | `number` (0 = non défini) |
| `keyRoot` | `number` (-1 = inconnu) |
| `isMajor` | `bool` |
| `style` | `number` — cast depuis `SmartMixEngine::Style` |

### `slotMix[]` (slots où le magic mix a été appliqué)

| Clé | Type |
|-----|------|
| `slot` | `number` 0–8 |
| `gain`, `pan`, `width`, `depth` | `number` |
| `applied` | `bool` |

### `scenes[]`

Chaque élément représente une scène ; seules les scènes marquées `used: true` sont sérialisées.

| Clé | Type |
|-----|------|
| `index` | `number` 0–7 |
| `bpm` | `number` (par scène, peut différer du maître selon usage UI) |
| `used` | `bool` |
| `trackBarCounts` | `array` de 9 `number` (1–32) |
| `filePaths` | `array` de 9 `string` |
| `mutes` | `array` de 9 `bool` |
| `gains` | `array` de 9 `number` |
| `steps` | `array` de 9 tableaux de `bool` — longueur piste `t` = `trackBarCounts[t] * 16` |
| `trimStart`, `trimEnd` | `array` de 9 `number` (`trimEnd` = -1 signifie pas de trim) |

## Exemple minimal (illustratif)

```json
{
  "version": 8,
  "projectName": "Exemple",
  "bpm": 120.0,
  "effectChain": [],
  "samples": [],
  "midiMappings": [],
  "musicContext": { "bpm": 0.0, "keyRoot": -1, "isMajor": true, "style": 0 },
  "masterKeyRoot": 0,
  "masterKeyMajor": true,
  "currentScene": 0,
  "slotMix": [],
  "scenes": []
}
```

## Ancien format v1 (référence historique)

Les premiers projets utilisaient des clés plates du type `effects.reverb`, `harmonizer`, etc. Ce layout n’est plus écrit ; le chargeur **convertit** vers la structure `effectChain` en mémoire. Ne pas s’en servir comme modèle pour de nouveaux fichiers.

Pour toute évolution du format : incrémenter la version, ajouter la migration dans `ProjectLoader::load`, et documenter ici en une phrase.
