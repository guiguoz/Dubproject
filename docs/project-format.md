# Format de fichier projet SaxFX Live (.saxfx)

Les projets SaxFX Live sont sauvegardés en JSON avec l'extension `.saxfx`.

## Exemple complet

```json
{
  "version": "1.0",
  "name": "Mon set live — Jazz Club",
  "audio": {
    "sampleRate": 44100,
    "bufferSize": 128,
    "inputDevice": "Focusrite USB ASIO",
    "outputDevice": "Focusrite USB ASIO",
    "inputChannel": 0,
    "outputChannels": [0, 1]
  },
  "effects": {
    "reverb": {
      "enabled": true,
      "mix": 0.4,
      "roomSize": 0.6,
      "damping": 0.5,
      "width": 1.0
    },
    "flanger": {
      "enabled": false,
      "rate": 2.0,
      "depth": 0.5,
      "feedback": 0.3,
      "mix": 0.5
    },
    "envelope": {
      "enabled": false,
      "attack": 0.01,
      "decay": 0.1,
      "sustain": 0.8,
      "release": 0.3
    }
  },
  "harmonizer": {
    "enabled": false,
    "mix": 0.5,
    "voices": [
      { "interval": 3, "gain": 0.7 },
      { "interval": 7, "gain": 0.5 }
    ]
  },
  "sampler": {
    "slots": [
      {
        "id": 0,
        "midiNote": 60,
        "file": "samples/loop_A.wav",
        "loop": true,
        "gain": 1.0,
        "pitchShift": 0
      },
      {
        "id": 1,
        "midiNote": 61,
        "file": "samples/pad_B.wav",
        "loop": false,
        "gain": 0.8,
        "pitchShift": 0
      }
    ]
  },
  "midi": {
    "device": "FCB1010",
    "channel": 0,
    "mappings": [
      { "type": "cc",   "number": 1,  "action": "preset_next" },
      { "type": "cc",   "number": 2,  "action": "preset_prev" },
      { "type": "note", "number": 60, "action": "sample_trigger", "slot": 0 },
      { "type": "note", "number": 61, "action": "sample_trigger", "slot": 1 },
      { "type": "cc",   "number": 7,  "action": "reverb_mix" },
      { "type": "cc",   "number": 8,  "action": "harmonizer_mix" }
    ]
  }
}
```

## Champs

### `audio`
| Champ | Type | Valeurs typiques | Description |
|-------|------|-----------------|-------------|
| `sampleRate` | int | 44100, 48000 | Fréquence d'échantillonnage |
| `bufferSize` | int | 64, 128, 256 | Taille du buffer (⚠ < 256 pour ≤ 20 ms) |
| `inputDevice` | string | — | Nom exact du périphérique ASIO/WASAPI |
| `outputDevice` | string | — | Nom exact du périphérique de sortie |

### `effects.reverb`
| Champ | Type | Plage | Description |
|-------|------|-------|-------------|
| `mix` | float | 0.0–1.0 | Ratio wet/dry |
| `roomSize` | float | 0.0–1.0 | Taille de la salle simulée |
| `damping` | float | 0.0–1.0 | Absorption haute fréquence |

### `harmonizer.voices`
| Champ | Type | Valeurs | Description |
|-------|------|---------|-------------|
| `interval` | int | 1–12 | Intervalle en demi-tons (3 = tierce mineure) |
| `gain` | float | 0.0–1.0 | Volume de la voix harmonique |

### `midi.mappings.action`
| Action | Description |
|--------|-------------|
| `preset_next` | Charge le preset suivant |
| `preset_prev` | Charge le preset précédent |
| `sample_trigger` | Déclenche le sample du slot `slot` |
| `reverb_mix` | Contrôle le mix réverb (CC → 0–127 mappé 0.0–1.0) |
| `harmonizer_mix` | Contrôle le mix harmoniseur |

## Versioning

Le champ `version` permet la compatibilité ascendante lors des mises à jour du format.
