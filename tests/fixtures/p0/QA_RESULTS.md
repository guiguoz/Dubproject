# P0 QA — résultats

Date : 2026-05-25  
Commit de référence : `9aa40d7` (downmix + `loadSampleIntoSlot` restauré)

## Automatisé (`SaxFXTests` tag `[p0]`)

Commande :

```bash
cmake --build build --config Release --target SaxFXTests
bin/SaxFXTests.exe "[p0]"
```

| Check-list | Test Catch2 | Statut |
|------------|-------------|--------|
| **A1** mono drag-drop | `P0 downmix — mono fixture unchanged` | PASS |
| **A2** stéréo L seul, R silence | `P0 downmix — stereo_lr is (L+R)*0.5 not left-only` | PASS |
| **A3** stéréo plein | `P0 downmix — stereo_full in-phase matches mono amplitude` | PASS |
| **B3** chargement stéréo (logique) | `P0 load — trim + loadSample produces audio` | PASS |
| **C1** trim + reload | `P0 trim — slice length` + load test | PASS |
| Régression downmix | `P0 regression — planar downmix matches interleaved` | PASS |

Suite complète : **97 test cases, all passed** (après ajout P0).

## Manuel GUI (à faire sur bench)

Prérequis : **Direct Monitor OFF** (Scarlett), fixtures dans ce dossier.

| Check-list | Statut | Notes |
|------------|--------|-------|
| A1–A3 drag-drop | À valider | Fichiers `mono.wav`, `stereo_lr.wav`, `stereo_full.wav` |
| B1–B4 transitions scène | À valider | B2 skip reload même path — écouter coupure |
| C2 save/load projet trim | À valider | |
| C3 même path trim différent par scène | À valider | |
| D1 clipboard piste | À valider | |
| E1–E2 micro-coupure crossfade | À valider | |

## Critère de clôture P0

| Critère | Automatisé |
|---------|------------|
| A2 | Oui |
| B3 | Oui (logique PCM + `loadSample`) |
| C1 | Oui (trim + load) |
| B2 | **Non** — validation oreille requise |

**Verdict technique** : logique P0 validée en tests unitaires. Clôture produit complète après passage manuel B2 + bench A/E dans l’app.
