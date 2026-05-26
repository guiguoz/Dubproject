# P0 QA — fixtures chargement samples

Fichiers générés par `generate_fixtures.py` (44100 Hz, 16-bit PCM, ~93 ms).

| Fichier | Contenu | Usage check-list |
|---------|---------|------------------|
| `mono.wav` | sinus 440 Hz mono | A1 |
| `stereo_lr.wav` | L = sinus 0.8, R = silence | A2 — détecte lecture canal gauche seul |
| `stereo_full.wav` | L/R = sinus 0.6 en phase | A3, B3 |

## Régénérer

```bash
python tests/fixtures/p0/generate_fixtures.py
```

## Bench manuel (GUI)

1. Build : `cmake --build build --config Release --target SaxFXLive`
2. **Direct Monitor OFF** sur la Scarlett.
3. Suivre la check-list P0 (sections A–E) avec ces fichiers en drag-and-drop / scènes.

Les tests automatisés `SaxFXTests` `[p0]` valident le downmix et le trim sans ouvrir l’UI.
