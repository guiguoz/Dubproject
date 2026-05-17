# Format de fichier projet SaxFX Live / DubEngine (`.saxfx`)

Les projets sont des fichiers **JSON** UTF-8 avec l'extension `.saxfx`. La **source de vérité** du schéma est le code : `src/project/ProjectLoader.cpp` (sauvegarde / chargement / migrations) et `src/project/ProjectData.h` (modèle C++).

## Version actuelle (écriture)

À l'enregistrement, le champ racine **`version`** est toujours **`18`** (nombre entier JSON).

Les fichiers plus anciens (1 … 15) sont **chargés** et migrés en mémoire par `ProjectLoader::load` ; à la prochaine sauvegarde ils passent en v16.

### Table des versions (résumé)

| Version | Éléments notables |
|---------|-------------------|
| 1 | Ancien format plat (`effects`, `harmonizer`, …) — migré vers v2 en mémoire au load |
| 2 | Tableau `effectChain` |
| 3 | `aiManaged` par effet ; champs `muted`, `gridDiv` sur les samples ; `musicContext` |
| 4 | `bpm` racine ; `samples[].steps` (16 booléens) par slot |
| 5 | `masterKeyRoot`, `masterKeyMajor`, `slotMix`, `scenes` (8 scènes), `currentScene` |
| 6 | Par scène : `trackBarCounts[9]` (1–32 barres), grilles `steps` jusqu'à 512 booléens par piste |
| 7 | `trimStart` / `trimEnd` par slot dans chaque scène |
| 8 | `delaySends[9]` par scène (send bus delay par slot) |
| 9–11 | `dubDelayEnabled`, `dubDelaySend`, `dubDelayWet`, `dubDelayFeedback`, `dubDelayTone`, `dubDelayDrive`, `dubDelayDiv` à la racine |
| 12 | `midiLearn[]` à la racine — bindings MIDI CC → paramètre |
| 13 | `userGains[9]` par scène — gain fader utilisateur, indépendant des gains IA |
| 14 | `serumState` à la racine (état preset Serum, base64) ; `swing` racine |
| 15 | `serumGain` par scène — gain Serum persisté par scène |
| 16 | Migration : `userGains[i] < 0.50` réinitialisé à 1.0 au load si slot non vide |
| 17 | `serumPresetName` à la racine — nom preset Serum saisi manuellement (click-to-edit) |
| 18 | `serumState` + `serumPresetName` déplacés dans `scenes[]` — preset Serum **par scène** ; migration v<18 : copie du root vers toutes les scènes utilisées |

## Structure JSON v18 (vue d'ensemble)

Racine :

| Clé | Type | Description |
|-----|------|-------------|
| `version` | `number` | Toujours `18` à l'écriture |
| `projectName` | `string` | Nom du projet |
| `bpm` | `number` | Tempo **maître** global (float) |
| `masterKeyRoot` | `number` | 0=C … 11=B |
| `masterKeyMajor` | `bool` | Mode majeur / mineur |
| `currentScene` | `number` | Index de scène courante (0–7) |
| `swing` | `number` | Swing global 0.0–1.0 (v14) |
| `slotMix` | `array` | États **Magic mix** (un objet par slot concerné, seulement si `applied`) |
| `scenes` | `array` | Scènes utilisées (`used: true`) |
| `dubDelayEnabled` | `bool` | Bus dub delay activé (v11) |
| `dubDelaySend` | `number` | Send vers le bus delay (v11) |
| `dubDelayWet` | `number` | Wet du bus delay (v11) |
| `dubDelayFeedback` | `number` | Feedback (v11) |
| `dubDelayTone` | `number` | Tone EQ (v11) |
| `dubDelayDrive` | `number` | Drive (v11) |
| `dubDelayDiv` | `number` | Division temporelle (v11) |
| `midiLearn` | `array` | Bindings MIDI CC → paramètre (v12) |
| `serumState` | `string` | État preset Serum, base64 (v14) |
| `serumPresetName` | `string` | *Supprimé en v18 — déplacé dans `scenes[].serumPresetName`* |

### `slotMix[]` (slots où le magic mix a été appliqué)

| Clé | Type |
|-----|------|
| `slot` | `number` 0–8 |
| `gain`, `pan`, `width`, `depth` | `number` |
| `applied` | `bool` |

### `midiLearn[]` (v12)

| Clé | Type |
|-----|------|
| `target` | `number` — cast depuis `midi::MappingTarget` |
| `cc` | `number` 0–127 |
| `min`, `max` | `number` |

### `scenes[]`

Chaque élément représente une scène ; seules les scènes marquées `used: true` sont sérialisées.

| Clé | Type | Version |
|-----|------|---------|
| `index` | `number` 0–7 | v5 |
| `bpm` | `number` | v5 |
| `used` | `bool` | v5 |
| `trackBarCounts` | `array` de 9 `number` (1–32) | v6 |
| `filePaths` | `array` de 9 `string` | v5 |
| `mutes` | `array` de 9 `bool` | v5 |
| `gains` | `array` de 9 `number` — gains calibration IA (diagnostique) | v5 |
| `steps` | `array` de 9 tableaux de `bool` — longueur piste `t` = `trackBarCounts[t] * 16` | v6 |
| `trimStart`, `trimEnd` | `array` de 9 `number` (`trimEnd` = -1 = pas de trim) | v7 |
| `delaySends` | `array` de 9 `number` — send bus delay par slot (0=muet, 0.8=PAD) | v8 |
| `userGains` | `array` de 9 `number` — gain fader utilisateur (point de départ du slider) | v13 |
| `serumGain` | `number` — gain Serum pour cette scène | v15 |
| `serumState` | `string` — état preset Serum base64 pour cette scène (v18) | v18 |
| `serumPresetName` | `string` — nom du preset Serum pour cette scène (v18) | v18 |

## Exemple minimal (illustratif)

```json
{
  "version": 18,
  "projectName": "Exemple",
  "bpm": 120.0,
  "masterKeyRoot": 0,
  "masterKeyMajor": true,
  "currentScene": 0,
  "swing": 0.0,
  "slotMix": [],
  "scenes": [
    {
      "index": 0,
      "used": true,
      "serumState": "<base64>",
      "serumPresetName": "ARP_Power_Grid_1"
    }
  ],
  "dubDelayEnabled": false,
  "dubDelaySend": 0.2,
  "dubDelayWet": 0.28,
  "dubDelayFeedback": 0.48,
  "dubDelayTone": 0.55,
  "dubDelayDrive": 0.15,
  "dubDelayDiv": 1,
  "midiLearn": []
}
```

## Règles d'évolution du format

- Incrémenter `version` dans `ProjectLoader::save()`.
- Ajouter la migration dans `ProjectLoader::load()` (bloc `if (data.version >= N)`).
- Documenter ici en une ligne dans la table des versions.
- Ne jamais baisser la version.
- Slot guard : rejeter `slot >= 9` au chargement.
