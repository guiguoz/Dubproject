# DubEngine — Plan de correctifs : désynchronisation samples + transitions de scène

Contexte : projet JUCE/C++ (repo Dubproject). Symptômes : les loops dérivent par rapport au
séquenceur au fil des mesures, jitter/flam entre pistes, transitions de scène glitchées avec
samples en retard. Analyse effectuée sur `master`. Traiter dans l'ordre P0 → P2.
Chaque correctif doit être accompagné de tests Catch2 (voir section Tests).

---

## P0.1 — Période de boucle fausse (dérive cumulative garantie)

**Fichier :** `src/dsp/Sampler.cpp`, wrap de loop dans `process()` (~l.519-535) ET le chemin
dupliqué dans `processStereo()` (~l.841).

**Bug :** au rebouclage, `vState.readPos = nxf` où `nxf = loopXfadeSamples(totalSamp)`
(jusqu'à 1024 samples). La période effective de la boucle devient `totalSamp − nxf`, soit
~23 ms plus courte que la longueur musicale à 44,1 kHz. La dérive est cumulative :
`nxf` samples d'avance par cycle de boucle.

**Fix attendu :** la période de boucle doit rester EXACTEMENT `totalSamp`.
Approche recommandée : pré-calculer le crossfade tail→head DANS le buffer au chargement
(les derniers `nxf` samples du buffer contiennent déjà le blend equal-power avec le head),
puis au wrap faire simplement `readPos -= totalSamp` (donc 0) sans re-blend à la lecture,
et supprimer le `readPos = nxf`. `sampleLoopWithXfade()` devient une lecture directe pour
les loops (le blend est déjà dans les données). Attention : le pré-blend doit être fait sur
le buffer inactif du double-buffer, puis swap — jamais sur le buffer actif.

Alternative acceptable si plus simple : garder le blend à la lecture mais wrapper à 0 et
faire lire la branche head du xfade « en avance » via un second pointeur, tant que la
période reste `totalSamp`.

## P0.2 — Le legato des loops ne resynchronise jamais

**Fichier :** `src/dsp/Sampler.cpp`, consommation de `triggerPending` (~l.401-418).

**Bug :** `isSameSample && isLoop && playing` → « keep playing seamlessly » : quand le
séquenceur retrigge une loop au step 0 de son pattern, la position de lecture n'est jamais
recalée. Toute dérive (P0.1, arrondis de stretch, changement de buffer) s'accumule.

**Fix attendu :** transformer ce chemin en resync doux :
- Calculer la position théorique attendue : le trigger vient du séquenceur au step 0 du
  pattern → position attendue = 0 (ou offset intra-bloc, voir P0.3).
- Si `|readPos − attendu| > seuil` (ex. 128 samples), lancer un micro-crossfade
  (~256 samples) de la voix courante vers une nouvelle voix démarrant à la position
  attendue (réutiliser le mécanisme double-voice existant). Sinon ne rien faire.
- Résultat : la dérive est bornée à un pattern et corrigée sans clic.

## P0.3 — Triggers quantifiés au bloc audio (jitter = taille du buffer)

**Fichiers :** `src/dsp/StepSequencer.h` (`process()`, boucle `while (phase_ > nextFirePhase_)`),
`src/dsp/Sampler.h/.cpp` (`trigger()`, consommation dans `process()`/`processStereo()`).

**Bug :** le séquenceur connaît la phase exacte de chaque step, mais `trigger(slot)` pose
juste un atomic consommé une fois par bloc ; toutes les voix démarrent à `i == 0` du bloc.
Jitter jusqu'à `blockSize` samples (11,6 ms @512/44,1 k), différent à chaque hit.

**Fix attendu :** triggers sample-accurate.
- `StepSequencer::process()` : calculer l'offset intra-bloc du step :
  `samplesPerBeat = sampleRate * 60 / bpm`,
  `offset = clamp(int((nextFirePhase_ − phaseAtBlockStart) * samplesPerBeat), 0, numSamples−1)`
  (capturer `phaseAtBlockStart` avant l'incrément de `phase_`).
- API : `sampler.trigger(track, offsetInBlock)` — encoder l'offset dans un
  `std::atomic<int> triggerOffset` par slot (ou packer pending+offset dans un seul atomic).
  Garder `trigger(slot)` = offset 0 pour la compat (MIDI, UI).
- Dans le sampler : la nouvelle voix stocke `startOffset` ; dans la boucle d'échantillons,
  la voix ne produit du son (et n'incrémente `readPos`) qu'à partir de `i >= startOffset`.
- Appliquer la même logique à `onTrackStep0()` (unmute quantisé) et aux triggers quantisés.

## P0.4 — Retrigger « choke » en retard de 256 samples

**Fichier :** `src/dsp/Sampler.cpp` (~l.421-427).

**Bug :** retrigger du même sample non-loop : la voix passe en `retriggering` avec fadeOut
256 samples et la lecture ne repart à 0 qu'après le fade → chaque hit répété est en retard
de ~6 ms (en plus du jitter P0.3).

**Fix attendu :** aligner sur le chemin polyphonique : démarrer immédiatement la nouvelle
lecture sur l'AUTRE voix (avec `startOffset` de P0.3) pendant que l'ancienne fade en 256
samples. Le choke devient un overlap court au lieu d'un délai.

---

## P1.1 — Transition de scène : tout se joue dans une double-croche sur le message thread

**Fichiers :** `src/MainComponent.cpp` (`navigateScene`, `timerCallback`, `applyScene`,
`preloadSceneAsync`), `src/dsp/StepSequencer.h` (`sceneEndFlag_`).

**Bugs :**
1. `sceneEndFlag_` est levé au step `transLen − 1` (dernier step) → il reste une
   double-croche moins la latence du timer 30 fps (jusqu'à 33 ms) pour exécuter
   `applyScene()` en entier.
2. Cache miss du preload → `loadSampleIntoSlot()` synchrone sur le message thread
   (décodage disque + auto-match WSOLA/Hermite) → samples appliqués plusieurs steps
   après le flip du pattern, + freeze UI.
3. `getSlotPcmSnapshot()` + `computeEnvelope()` (waveform UI) exécutés pendant la
   transition sur le message thread.

**Fix attendu :**
- **Armer la transition seulement quand le preload est prêt** : `navigateScene()` lance
  `preloadSceneAsync(target)` mais N'appelle `setPendingTransitionLen()` que lorsque tous
  les slots du cache sont `ready` (poll dans `timerCallback`, état « preparing » affiché
  dans le label). Tant que le preload n'est pas fini, la scène courante continue — jamais
  de chargement synchrone à la frontière.
- **Lever le flag plus tôt** : déclencher `sceneEndFlag_` à `sceneLen − 16` (une mesure
  avant la fin) au lieu de `sceneLen − 1`, et faire faire à `applyScene()` uniquement des
  swaps (loadSample double-buffer = memcpy vers buffer inactif + flip atomique — déjà
  conçu pour ça). Le flip du step buffer reste au step 0 (mécanisme existant OK).
- **Différer l'UI** : waveform/envelope calculés en tâche de fond après la transition,
  ou pré-calculés dans le preload.
- Le preload doit inclure le stretch auto-match complet (PCM final prêt à copier),
  pas seulement le décodage.

## P1.2 — Deux horloges de transport non synchronisées

**Fichiers :** `src/dsp/StepSequencer.h` (`phase_`), `src/dsp/Sampler.cpp` (`beatClock_`),
`src/dsp/BeatClock.h`.

**Bug :** `StepSequencer::phase_` et `Sampler::beatClock_` avancent indépendamment.
`setPlaying(true)` / `resetPhase()` remettent la phase du séquenceur à 0 mais jamais le
BeatClock → `triggerQuantized()` et les opérations quantisées côté sampler s'alignent sur
une grille décalée de celle du séquenceur.

**Fix attendu :** une seule source de vérité. Option minimale : ajouter
`BeatClock::resetPhase()` et l'appeler partout où le séquenceur reset sa phase
(`setPlaying(true)`, `resetPhase()`, `prepare()`), via un flag atomique consommé par le
thread audio. Option propre : un `TransportClock` unique avancé une fois par bloc dans
`getNextAudioBlock()`, dont séquenceur et sampler dérivent leur phase.

## P1.3 — Fade-in de 88 samples sur chaque déclenchement

**Fichier :** `src/dsp/Sampler.cpp` (`kFadeInLen = 88`, courbe exponentielle).

**Bug :** 2 ms de fade-in exponentiel sur CHAQUE trigger ramollit les transitoires (kick,
perc) et déplace perceptuellement l'attaque → contribue à l'impression de désync.

**Fix attendu :** fade-in court (16–32 samples, linéaire) sur trigger initial d'un sample
qui démarre à `readPos = 0` sur un buffer commençant proche de zéro ; garder 88 samples
uniquement pour les cas où c'est nécessaire (restart de loop en milieu de forme d'onde,
reprise après resync P0.2).

---

## P2.1 — Crossfade de scène piloté par le timer GUI

**Fichiers :** `src/MainComponent.cpp` (`updateCrossfade(33, ...)`), `src/dsp/SceneManager.h`.

**Bug :** rampes de gain avancées par pas de 33 ms depuis le timer 30 fps → escaliers
audibles, timing dépendant du framerate/charge UI.

**Fix attendu :** ne poser que des gains CIBLES atomiques depuis le message thread ; le
lissage existe déjà côté audio (`gainSmoothed_` avec `gainRampCoeff_`) — rendre la
constante de temps paramétrable par slot pour le crossfade (~150 ms) et supprimer
l'interpolation côté timer.

## P2.2 — Changement de BPM après chargement : loops fausses

**Contexte :** les samples sont time-stretchés au BPM projet AU CHARGEMENT ; la lecture est
à vitesse fixe (`readPos++` entier, pas de playback rate). Changer le BPM global ensuite
désynchronise toutes les loops.

**Fix attendu (court terme) :** au changement de BPM, relancer le re-stretch async de tous
les slots chargés (depuis le PCM original conservé), swap via double-buffer, resync P0.2 au
prochain step 0. Long terme : lecture à taux fractionnaire (Hermite runtime,
`rate = bpmProjet / bpmSample`) pour un suivi de tempo instantané.

## P2.3 — Garde-fou wrap du compteur global

**Fichier :** `src/dsp/StepSequencer.h` (`globalStep = nextFireStepIdx_ % kMaxSteps`).

Le wrap à 512 n'est correct que si `trackStepCount` divise 512. Les valeurs UI actuelles
(1/2/4/8/16/32 mesures) sont OK, mais `setTrackStepCount()` accepte n'importe quoi →
clamp/assert que `512 % count == 0`, ou faire des compteurs par piste.

---

## Tests d'acceptation (Catch2, à ajouter dans tests/)

1. **T-SYNC1 (P0.1)** : loop de longueur exacte N samples, loopEnabled, process de
   10 × N samples → vérifier que le sample de tête réapparaît exactement tous les N
   samples (tolérance 0). Tester N < 2048 et N > 100000.
2. **T-SYNC2 (P0.3)** : bpm tel qu'un step tombe au milieu d'un bloc de 512 ; vérifier que
   la première valeur non nulle de la voix apparaît à l'index d'offset attendu ±1.
3. **T-SYNC3 (P0.2)** : forcer une dérive artificielle de readPos (+500), retrigger legato
   au step 0 → readPos recalé, sortie sans discontinuité > seuil (pas de clic).
4. **T-SYNC4 (P0.4)** : retrigger même sample one-shot → la nouvelle attaque démarre dans
   le même bloc (overlap), pas 256 samples plus tard.
5. **T-TRANS1 (P1.1)** : simuler cache non prêt → `navigateScene` ne doit PAS armer
   `setPendingTransitionLen` ; une fois ready, transition armée et flip au step 0.
6. **T-CLK1 (P1.2)** : après `setPlaying(true)`, phase séquenceur et phase BeatClock
   égales ; un `triggerQuantized(Bar)` tombe sur le même sample que le step 0 du séquenceur.
7. **Test manuel** : kick 4/4 slot 2 + loop 2 mesures slot 0, 120 BPM, 15 minutes →
   enregistrer la sortie, vérifier offset kick/loop constant < 1 ms du début à la fin.

## Contraintes RT (non négociables)

- Zéro allocation, zéro lock, zéro IO dans `process()`/`processStereo()`/le callback audio.
- Tous les échanges GUI↔audio via atomics ou double-buffer + flip atomique (patterns
  existants dans le code : `activeDataIdx`, `prepareStepBuffer`/`flipIfPrepared`).
- Ne pas casser : quantized unmute (onTrackStep0), loop bleed Option A, fade-out 350 ms
  des pads, le fix « loaded=false pendant swap » du Sprint 23.
- Mettre à jour README (sprint « Correctifs sync ») et les deux chemins process()
  ET processStereo() pour chaque fix du Sampler.
