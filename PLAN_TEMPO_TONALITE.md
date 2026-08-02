# DubEngine — Backlog : moteur tempo/tonalité et effets de performance

Prérequis : PLAN_FIX_SYNC.md (P0) implémenté et validé. PLAN_REFONTE_TRANSITIONS.md
recommandé mais pas bloquant (seul B4 en dépend directement).
Hors périmètre (décision produit) : Ableton Link / MIDI clock, enregistrement master.

Implémenter dans l'ordre A → D. Un commit par item, avec ses tests.
Contraintes RT identiques aux plans précédents : zéro allocation/lock/IO dans le
thread audio, atomics + double-buffers, chaque modif du Sampler appliquée aux DEUX
chemins (process ET processStereo).

---

# A — FONDATION : lecture à taux fractionnaire par slot

Tout le reste du backlog dépend de cet item. Le construire d'abord, seul, avec ses
tests, avant d'empiler quoi que ce soit dessus.

## A1. Position de lecture fractionnaire + interpolation Hermite

**Fichiers :** `src/dsp/Sampler.h/.cpp` (struct voice, boucles de lecture).

**État actuel :** `readPos` est un `int` incrémenté de 1 → vitesse de lecture figée,
samples inutilisables si le BPM projet change, aucune transposition possible.

**Cible :**
- `double readPos` + `double rate` par voix (1.0 = vitesse native).
- Lecture par interpolation Hermite 4 points (Catmull-Rom) : implémenter
  `inline float hermite4(const float* p, double frac)` dans l'anonymous namespace
  de Sampler.cpp. Pas de linéaire (aliasing audible sur les rates éloignés de 1).
- `rate` effectif = produit de plusieurs facteurs, calculé une fois par bloc :
  `rate = tempoRatio × transposeRatio × perfRatio`
  - `tempoRatio`  : suivi de tempo (A2)
  - `transposeRatio` : 2^(semitones/12) (section C)
  - `perfRatio`   : effets de perf — tape stop, glide, reverse (section D)
- Bords : l'interpolation lit p[-1]..p[+2] → clamp aux bornes du buffer ; pour les
  loops, les points au-delà de la fin lisent le début (cohérent avec le pré-blend
  de boucle du fix P0.1).
- Reverse-ready : autoriser rate < 0 (readPos décroît) dès maintenant, même si
  l'UI arrive en D3 — ça évite de retoucher la boucle de lecture deux fois.
- Le wrap de boucle devient : `while (readPos >= loopLen) readPos -= loopLen;`
  (et symétrique en reverse). loopLen en double, période musicale EXACTE (cf. P0.1).
- Compat : les triggers sample-accurate (P0.3) posent `readPos = 0.0` à l'offset ;
  les fades (fadeIn/fadeOut) restent comptés en samples de SORTIE, pas d'entrée.

**Tests :**
- T-A1a : rate = 1.0 → sortie identique bit-à-bit à l'ancienne lecture entière
  (sur un sample de test, hors interpolation aux bords : tolérance 1e-6).
- T-A1b : rate = 0.5 et 2.0 sur une sinusoïde → fréquence de sortie mesurée = f×rate
  ±1 %, pas d'aliasing au-dessus du niveau attendu de l'Hermite.
- T-A1c : loop + rate ≠ 1 → la période de boucle en samples de sortie =
  loopLen / rate, stable sur 100 cycles (dérive cumulée < 1 sample).

## A2. Suivi de BPM instantané : mode hybride repitch → re-stretch

**Fichiers :** `Sampler`, `MainComponent` (chemin setBpm), `ScenePreloader`/tâches
de fond existantes (`backgroundTasks_`).

**Comportement cible au changement de BPM projet :**
1. **Immédiat (thread audio)** : chaque slot chargé passe en repitch —
   `tempoRatio = bpmProjet / bpmDuStretchActuel` (le BPM auquel le PCM du slot a
   été stretché est stocké dans le slot : nouveau champ `atomic<float> stretchedBpm`).
   Les loops restent calées sur la grille instantanément ; la hauteur bouge
   (assumé, esprit vinyle).
2. **En arrière-plan (message thread + async)** : re-stretch WSOLA de chaque slot
   depuis son PCM ORIGINAL (à conserver — voir A3) vers le nouveau BPM, écrit dans
   le buffer inactif du slot.
3. **Swap quantisé** : au prochain step 0 de la piste, flip `activeDataIdx` +
   `tempoRatio` remis à 1.0 dans le MÊME bloc audio (sinon double correction
   audible). Utiliser le mécanisme de resync legato de P0.2 pour recaler readPos
   proportionnellement (`newPos = oldPos × ancienneLongueur⁻¹ × nouvelleLongueur`).
- Débounce : si l'utilisateur tourne le knob BPM, ne lancer le re-stretch qu'après
  400 ms de stabilité ; le repitch, lui, suit chaque valeur en continu.
- Interaction transitions : un changement de BPM pendant PREPARING invalide le
  preload (déjà spécifié dans le plan transitions).

**Tests :**
- T-A2a : BPM 120→140 pendant lecture d'une loop 2 mesures → aucun trou, la loop
  reste alignée au séquenceur à chaque step 0 (offset < 1 ms), hauteur revenue à
  la normale après le swap.
- T-A2b : 10 changements de BPM en 2 s (knob) → un seul re-stretch lancé, zéro
  glitch.

## A3. Conserver le PCM original par slot

Aujourd'hui le slot ne garde que le PCM stretché : chaque re-stretch depuis un
fichier = IO disque. Ajouter un stockage du PCM original décodé (post-trim,
pré-stretch) par slot, côté message thread uniquement (simple std::vector, pas
touché par l'audio). Budget mémoire : plafond global ~300 Mo, éviction LRU des
originaux des scènes non voisines (recharge disque si besoin). Toutes les
opérations offline (re-stretch A2, transpose C1, re-trim) partent de cet original —
jamais de traitement en cascade sur du PCM déjà traité (dégradation cumulative).

## A4. Qualité du stretch offline

**Fichier :** `src/dsp/WsolaShifter.h` (utilitaire offline).

L'utilitaire offline actuel fait une interpolation LINÉAIRE pour le time-scale :
perte d'aigus et aliasing sur les gros ratios. Le remplacer par :
- resampling Hermite 4 points minimum (réutiliser hermite4 de A1) ;
- pour la voie « stretch sans changement de pitch », garder WSOLA mais vérifier
  que les constantes (fenêtre 512 / hop 256 / delta 128) conviennent au matériel
  percussif : sur les breaks de batterie, réduire la fenêtre (256/128) limite le
  flou des transitoires — exposer ces constantes en paramètres du call offline et
  choisir le preset selon le rôle du slot (PERC/KICK → fenêtre courte,
  PAD/MELODIC → fenêtre longue).

**Test :** T-A4 : sweep 20 Hz–20 kHz stretché ×0.8 et ×1.25 → énergie au-dessus de
Nyquist/2 du contenu attendu < −60 dB (mesure FFT), contre l'implémentation
linéaire en référence.

---

# B — TEMPO : contrôles musicaux

## B1. Tap tempo

**Fichiers :** `MainComponent` (UI + MIDI), aucun changement DSP (A2 fait le travail).

Bouton UI + apprentissage MIDI (réutiliser l'infra MIDI Learn existante).
Moyenne glissante des 4 derniers taps, écart-type > 15 % → reset de la mesure.
Applique via le chemin setBpm normal (donc repitch immédiat + re-stretch différé).

## B2. Rampe de BPM quantisée

**Fichiers :** `StepSequencer` (ou TransportClock si la refonte est faite),
`MainComponent`.

`rampBpm(target, bars)` : interpolation du BPM sur N mesures, avancée par le thread
audio (valeur cible + durée posées en atomics ; le thread audio interpole par bloc).
Le repitch A2 suit naturellement puisque tempoRatio est recalculé par bloc ; le
re-stretch de fond n'est lancé qu'à l'arrivée de la rampe. UI : champ « → BPM sur
N mesures » à côté du BPM. Cas limite : nouvelle rampe pendant une rampe → repart
de la valeur courante.

**Test :** T-B2 : rampe 120→150 sur 4 mesures → BPM mesuré (intervalle entre steps)
monotone, atteint 150 ±0.1 exactement à la frontière de la 4e mesure.

## B3. Half-time / double-time par slot

**Fichiers :** `Sampler` (un facteur ×0.5/×1/×2 dans tempoRatio), UI step-seq panel.

Bouton ½× / 1× / 2× par piste, changement quantisé au prochain step 0 de la piste
(même mécanique que le mute quantisé existant). En half-time la hauteur descend
d'une octave (repitch assumé — c'est l'effet recherché en dub) ; documenter dans
l'UI (tooltip).

---

# C — TONALITÉ

## C1. Transpose par slot (±12 demi-tons)

**Fichiers :** `Sampler` (transposeRatio dans le rate), UI par slot, sauvegarde scène.

- One-shots (drums) : repitch pur via `transposeRatio = 2^(st/12)` — coût zéro, la
  durée change mais on s'en fiche pour un hit.
- Loops : le repitch changerait la durée → deux modes, sélection automatique selon
  loopEnabled :
  - loop OFF → repitch pur temps réel ;
  - loop ON → transpose OFFLINE (pitch-shift WSOLA sur l'original A3, durée
    préservée) en tâche de fond + swap quantisé au step 0, exactement le pipeline
    A2. Pendant le calcul, badge « … » sur le slot ; pas de repitch temporaire
    (une loop désaccordée ET désalignée serait pire que d'attendre 1–2 s).
- Le transpose est stocké par slot DANS la scène (SceneManager) et restauré par
  les transitions.
- Changement quantisé à la mesure quand le transport tourne.

**Tests :** T-C1a one-shot : sinusoïde +7 st → fréquence ×2^(7/12) ±1 %.
T-C1b loop : +3 st → durée inchangée (±1 sample), alignement step 0 conservé.

## C2. Détection de tonalité au chargement + key-match

**Fichiers :** `KeyDetector`/`KeyResult` existants, `ScenePreloader`,
`MusicContext.keyRoot`, UI slot.

- À l'import d'un sample MELODIC/BASS/PAD (rôle du classifieur — inutile sur un
  kick) : lancer KeyDetector sur le PCM original en tâche de fond ; stocker
  `detectedKey` + confiance dans les métadonnées du slot (et dans la scène).
- Badge UI sur le slot : « Am » (vert si compatible avec la tonalité projet,
  orange sinon, avec l'écart : « Am · −2 st »). Confiance faible → « ? », pas de
  suggestion.
- Clic sur le badge = appliquer la transposition suggérée via C1 (plus court
  chemin : min(|delta|, 12−|delta|), signe choisi pour rester dans ±7 st ;
  au-delà, proposer la transposition vers la relative min/maj).
- Option globale « Auto key-match on load » (off par défaut).

**Test :** T-C2 : fixtures de loops de tonalité connue (générer des progressions
synthétiques en C maj, A min, F# min) → détection correcte ≥ 90 %, suggestion de
transpose = plus court chemin.

## C3. Tonalité par scène

**Fichiers :** `SceneManager` (champ keyRoot/isMajor par scène),
`SceneTransitionEngine` (si refonte faite) ou applyScene.

Chaque scène stocke sa tonalité (héritée de MusicContext au moment de la capture,
éditable). À la transition, pour chaque slot commun (KEEP) dont la scène cible a
une tonalité différente : événement de transpose quantisé à launchStep (pipeline
C1). Les slots ENTER arrivent déjà à la bonne tonalité (transpose stocké par
scène). Résultat : enchaîner Am → Cm transpose automatiquement ce qui traverse.
Dépendance : PLAN_REFONTE_TRANSITIONS §3 (catégorie KEEP). Sans la refonte,
implémenter la version simple : transpose appliqué dans applyScene.

## C4. Verrouillage à la gamme (Serum + transpositions)

**Fichiers :** `ScaleHarmonizer` existant, `SerumHost` (filtrage des notes MIDI
entrantes), C1/C2.

- Les notes MIDI envoyées au Serum sont mappées au degré diatonique le plus proche
  de la gamme courante (KeyResult.scaleDegrees) quand le lock est actif.
- Les suggestions de transpose C2 sont contraintes aux intervalles restant dans la
  gamme projet.
- Toggle UI global « Scale lock » + affichage de la gamme courante.

---

# D — EFFETS DE PERFORMANCE (tous : simples modulations de perfRatio, thread audio)

Infrastructure commune : par slot, une petite machine à état de perf
(`enum PerfFx { None, TapeStop, TapeStart, Glide }` + rampe) avancée par bloc dans
le thread audio, pilotée par atomics posés depuis l'UI/MIDI. perfRatio multiplie le
rate (A1). Tous quantisables (déclenchement au prochain temps ou immédiat, réglage
global).

## D1. Tape stop / tape start

- Stop : perfRatio décroît de 1 → 0 sur une durée réglable (0.25–2 s), courbe
  quadratique (décélération de platine) ; à 0, la voix passe playing=false.
- Start : symétrique depuis 0 → 1 au (re)trigger.
- Déclenchement : bouton par slot + un « tape stop master » qui l'applique à tous
  les slots (fin de morceau) — mappable MIDI.

## D2. Pitch glide sur trigger (sirène/riser)

- Paramètres par slot : glideStart (demi-tons, ±24), glideTime (ms).
  Au trigger, perfRatio part de 2^(glideStart/12) et glisse vers 1.0
  (exponentiel sur glideTime). glideStart > 0 = sirène descendante, < 0 = riser.
- Presets rapides : Siren (+12, 800 ms), Riser (−12, 1500 ms), Dive (+24, 300 ms).

## D3. Reverse par slot

- Toggle par piste, quantisé au prochain step 0 : rate ← −rate, readPos inchangé
  (lecture repart en arrière depuis la position courante — effet « rewind »).
  Option « reverse from end » pour les one-shots (readPos = fin au trigger).
- La boucle de lecture A1 gère déjà rate < 0 ; ici seulement l'UI + le wrap
  symétrique + les tests.

**Tests section D :** T-D1 tape stop 500 ms → fréquence instantanée décroissante
monotone jusqu'au silence, aucune discontinuité > −60 dB. T-D2 glide +12/800 ms →
fréquence initiale = 2× la finale, convergence à ±1 % en 800 ms ±10 %.
T-D3 reverse → sortie = miroir temporel du segment, alignement step 0 conservé.

---

# E — LOOPER : suivi du tempo

**Fichier :** `src/dsp/LooperEngine.h/.cpp`.

Le buffer du looper est figé au tempo d'enregistrement : tout changement de BPM le
désynchronise. Appliquer la même solution que les slots :
- position de lecture fractionnaire + Hermite (partager hermite4) ;
- `rate = bpmProjet / bpmEnregistrement` (stocker le BPM au moment où Recording →
  Playing) ;
- l'overdub écrit à la position de LECTURE courante (donc dans le référentiel du
  buffer, pas du temps réel) — attention au mode Tape : le feedback 0.98 reste par
  passage de boucle ;
- resync au downbeat comme aujourd'hui (getCurrentPhase du séquenceur).
Repitch assumé (pas de stretch temps réel sur le looper).

**Test :** T-E : enregistrer 2 mesures à 120, passer à 150 → le loop reste calé au
downbeat (offset < 1 ms sur 20 mesures), pitch monté de ratio 150/120.

---

# F — OPTIONNEL (fin de backlog, ne pas commencer avant A–E validés)

## F1. Slice-to-steps
Découpage d'un break sur transitoires (réutiliser la détection du
FeatureExtractor), mapping des slices sur les steps d'une piste, UI d'édition.
Gros chantier UI pour un gain live modéré — à ne faire que si tout le reste est
stable et que l'envie produit est confirmée.

---

# Ordre d'implémentation et jalons

1. **A1 + A3** (fondation lecture fractionnaire + PCM originaux) — jalon : T-A1*.
2. **A2 + A4** (BPM hybride + qualité offline) — jalon : changer le BPM en live
   sans glitch, test manuel 10 min.
3. **C1** (transpose slot) puis **B3** (half/double) — mêmes mécanismes.
4. **C2** (key detect + badge) puis **C3/C4** selon avancement de la refonte
   transitions.
5. **B1/B2** (tap + rampe).
6. **D1–D3** (perf FX) — rapides une fois A1 en place.
7. **E** (looper).
8. F1 seulement si décidé.

# Budget CPU / garde-fous

- L'Hermite par sample × 9 slots × 2 voix est négligeable (< 2 % d'un cœur à
  44,1 k). Le WSOLA reste STRICTEMENT offline/background — jamais dans le callback.
- Ajouter un compteur de charge du callback audio (max block time / budget) loggé
  en debug, pour objectiver chaque étape.
- Chaque item : mise à jour du README (section sprint) + tests Catch2 listés.
