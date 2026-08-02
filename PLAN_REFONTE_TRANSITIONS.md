# DubEngine — Refonte complète du système de transitions de scène
## SceneTransitionEngine : transitions content-aware, from scratch

Contexte : le système actuel (sceneEndFlag au dernier step → timer GUI 30 fps →
applyScene monolithique sur le message thread) est remplacé intégralement. Ce document
est le cahier des charges. L'implémentation doit supprimer l'ancien chemin
(consumeSceneEnd / pendingScene_ dans timerCallback / crossfade GUI) une fois le
nouveau moteur validé.

Prérequis : les correctifs P0 de PLAN_FIX_SYNC.md (triggers sample-accurate, période
de boucle exacte, horloge unique). Sans eux, aucune transition ne peut être propre.

---

# 1. Principes de conception

1. **Le thread audio est le seul maître du temps.** Le message thread PRÉPARE
   (chargement, stretch, calcul du plan) ; le thread audio EXÉCUTE (fades, triggers,
   flips) aux frontières musicales exactes. Aucune décision de timing ne dépend d'un
   timer GUI.
2. **Rien ne s'arme tant que tout n'est pas prêt.** Une transition ne peut être armée
   que lorsque 100 % des samples de la scène cible sont décodés, stretchés au BPM
   projet, et copiés dans les buffers inactifs des slots. État "preparing" visible
   dans l'UI en attendant.
3. **Ce qui ne change pas ne bouge pas.** Un slot identique entre les deux scènes
   (même fichier, même trim) traverse la transition sans coupure, sans rechargement,
   sans fade. C'est la règle n°1 d'une transition propre.
4. **La densité est une dimension de la transition.** Passer de 2 slots actifs à 6
   (ou l'inverse) ne se fait jamais d'un coup : entrées et sorties sont ordonnées et
   échelonnées musicalement.
5. **Zéro allocation / lock / IO dans le thread audio.** Le plan est un POD de taille
   fixe, transmis par double-buffer + flip atomique.

---

# 2. Architecture

## 2.1 Nouveaux composants

```
src/dsp/TransitionPlan.h        — POD : le plan calculé (taille fixe, RT-safe)
src/dsp/SceneTransitionEngine.h — machine à états côté audio, exécute le plan
src/SceneDiff.h/.cpp            — message thread : diff A→B + calcul du plan
src/ScenePreloader.h/.cpp       — message thread + workers : préparation cible
```

## 2.2 Machine à états (SceneTransitionEngine, thread audio)

```
IDLE → PREPARING → ARMED → EXECUTING → SETTLING → IDLE
```

- **IDLE** : rien en cours. `requestTransition(target)` (message thread) passe en
  PREPARING.
- **PREPARING** (message thread actif) : ScenePreloader charge/stretch tous les
  samples cibles vers les buffers inactifs ; SceneDiff calcule le TransitionPlan ;
  le plan est publié via double-buffer. Durée illimitée — la scène courante joue
  normalement. Annulable (retour IDLE). L'utilisateur peut changer de cible : on
  recalcule.
- **ARMED** : le thread audio a flippé le plan. Il attend la frontière de lancement
  (voir 3.4). L'UI affiche "Scene A → B (armed)".
- **EXECUTING** : le plan se déroule, événement par événement, avec des timestamps
  en steps absolus (compteur monotone du séquenceur). Chaque événement =
  {step, offsetIntraBloc, action, slot, param}. Le moteur consomme les événements
  dont l'heure est atteinte, avec l'offset intra-bloc sample-accurate.
- **SETTLING** : tous les événements consommés ; on attend la fin des fades/queues de
  delay (durée max du plan), puis notification au message thread (atomic) pour la
  mise à jour UI différée (waveforms, labels) et le passage IDLE.

## 2.3 TransitionPlan (POD, RT-safe)

```cpp
struct TransitionEvent {
    int32_t  step;        // step absolu (compteur monotone) où l'event se déclenche
    int16_t  offset;      // offset intra-step en samples (normalement 0)
    uint8_t  slot;        // 0..8
    uint8_t  action;      // enum : FadeOut, FadeIn, Trigger, Mute, Unmute,
                          //        SwapBuffer, GainRamp, DelayThrow, PatternFlip
    float    param;       // durée de fade en steps, gain cible, send delay…
};

struct TransitionPlan {
    static constexpr int kMaxEvents = 64;
    TransitionEvent events[kMaxEvents];
    int   numEvents;            // triés par (step, offset)
    int32_t launchStep;         // step absolu du début (frontière choisie)
    int32_t settleSteps;        // durée après le dernier event avant SETTLING→IDLE
    uint8_t slotRoles[9];       // rôle détecté par slot (voir 3.1)
    uint8_t transitionType;     // CUT / SMOOTH / BUILD / BREAKDOWN / DUB (voir 3.3)
};
```

Publication : `plans_[2]` + `preparedPlan_` atomique, même pattern que
`prepareStepBuffer()/flipIfPrepared()` existant.

---

# 3. Le calcul du plan (SceneDiff, message thread)

## 3.1 Classification des slots

Pour chaque slot des deux scènes, déterminer un rôle. Réutiliser l'infra existante
(`FeatureExtractor` / `AiContentClassifier` déjà dans src/dsp/) :

```
KICK / BASS / PERC / MELODIC / PAD / FX / UNKNOWN
```

Heuristiques de secours si le classifieur est indisponible : centroïde spectral bas +
transitoire fort = KICK ; bas + soutenu = BASS ; court + brillant = PERC ; long +
soutenu = PAD. Le rôle détermine l'ORDRE des entrées/sorties (3.4) et les durées de
fade par défaut.

## 3.2 Diff A→B, slot par slot

Pour chaque slot i, comparer (filePath, trimStart, trimEnd, pattern, gain) :

| Cas | Situation | Catégorie |
|---|---|---|
| KEEP | même fichier+trim, pattern identique | rien à faire, continue |
| KEEP_PATTERN | même fichier, pattern différent | continue, seul le pattern flippe |
| KEEP_GAIN | même fichier, gain différent | GainRamp (rampe audio-thread) |
| EXIT | présent dans A, absent/vide dans B | fade-out planifié |
| ENTER | absent dans A, présent dans B | swap buffer + entrée planifiée |
| REPLACE | fichiers différents dans le même slot | EXIT puis ENTER (le double-buffer du slot permet le chevauchement : l'ancienne voix fade sur l'ancien buffer pendant que la nouvelle démarre sur le nouveau — mécanisme voices[2] + activeDataIdx existant) |

Comptes de densité : `densityA = slots actifs dans A`, `densityB = idem B`.

## 3.3 Choix du type de transition

Automatique par défaut (surchargeable par l'utilisateur via un sélecteur UI) :

- **SMOOTH** (défaut, |densityB − densityA| ≤ 1) : sorties et entrées croisées sur
  1 mesure autour de la frontière.
- **BUILD** (densityB > densityA + 1, ex. 2 → 6) : les slots communs continuent ;
  les entrées sont ÉCHELONNÉES sur 1 à 2 mesures APRÈS la frontière, dans l'ordre
  des rôles : KICK au step 0, BASS au step 4 (temps 2), PERC aux steps 8/12,
  MELODIC/PAD sur la mesure suivante avec fade-in 1 temps. Jamais plus de 2 entrées
  sur le même step.
- **BREAKDOWN** (densityB < densityA − 1, ex. 6 → 2) : les sorties sont échelonnées
  sur la DERNIÈRE mesure de la scène A, ordre inverse : FX/PAD d'abord (fade 1
  mesure), MELODIC, puis PERC ; KICK et BASS tiennent jusqu'à la frontière et
  coupent/fadent au step 0 de B. Les slots de B démarrent proprement au step 0.
- **DUB** (variante activable, très dans l'esprit du projet) : comme BREAKDOWN mais
  chaque slot sortant reçoit un événement DelayThrow (delaySend → 1.0 sur son
  dernier hit, puis mute) : la queue du PingPongDelay remplit le vide pendant que la
  nouvelle scène s'installe. Le feedback du delay est légèrement monté pendant
  SETTLING puis ramené (réutiliser le morphing delay existant).
- **CUT** : tout au step 0, fades 20 ms (StopMode::SceneSwap). Pour le live nerveux.

## 3.4 Frontière de lancement et durées

- `launchStep` = prochain multiple de `sceneLen` (longueur max des patterns de A),
  MAIS avec une contrainte de préparation : si le preload n'est pas prêt à
  `launchStep − sceneLen/4`, on vise la frontière suivante. Plus de course contre la
  montre.
- Les événements pré-frontière (BREAKDOWN/DUB) sont placés dans la dernière mesure
  AVANT launchStep — possible car le plan est armé au moins une frontière à l'avance.
- PatternFlip est TOUJOURS un événement à `launchStep`, offset 0, exécuté avant les
  triggers de ce step (conserver l'ordre actuel flip → trigger).
- Durées de fade par rôle (défauts, en steps) : KICK sort en 1 (net), BASS 4,
  PERC 2, MELODIC 8, PAD 16, FX 16. Entrées : KICK/PERC attaque directe (fade-in
  minimal), BASS 2, PAD/MELODIC 4–8.

## 3.5 Cas limites (à couvrir par des tests)

- Scène cible vide → BREAKDOWN vers silence, patterns vidés au launchStep.
- Scène source vide → toutes les entrées en BUILD depuis silence.
- Re-clic pendant PREPARING → recalcul vers la nouvelle cible.
- Re-clic pendant ARMED/EXECUTING → refusé (ou file d'attente d'une cible, au choix,
  mais une seule transition en vol à la fois).
- Stop du transport pendant EXECUTING → abort : appliquer immédiatement l'état final
  de B (gains cibles, patterns), StopMode::Instant sur ce qui devait sortir.
- Changement de BPM pendant PREPARING → invalider le preload (les stretchs sont au
  mauvais tempo), relancer.
- Slot en cours de lecture manuelle (trigger utilisateur hors séquenceur) → traité
  comme les autres : catégorie du diff s'applique.

---

# 4. ScenePreloader (message thread + std::async, réutiliser backgroundTasks_)

Pour chaque slot ENTER/REPLACE de la cible :
1. décodage fichier (ou cache PCM déjà en mémoire — garder un LRU des scènes
   visitées, taille bornée ~200 Mo),
2. trim,
3. stretch auto-match au BPM projet (WSOLA/Hermite existants),
4. copie dans le buffer INACTIF du slot (`data[1−activeDataIdx]`) — le swap effectif
   (SwapBuffer event) est fait par le thread audio au moment planifié,
5. pré-calcul du blend de boucle (cf. fix P0.1) et de l'enveloppe waveform UI.

`allReady()` = tous les slots prêts → publie le TransitionPlan et signale ARMED.
Échec de chargement d'un slot → le slot est exclu du plan (log + badge UI), la
transition reste possible.

---

# 5. Intégration et suppression de l'ancien code

- `navigateScene(delta)` → `transitionEngine.requestTransition(target)` ; le chemin
  "séquenceur arrêté = application immédiate" est conservé mais passe aussi par le
  plan (type CUT, launchStep immédiat) pour un seul chemin de code.
- Supprimer : `sceneEndFlag_`/`consumeSceneEnd`, `pendingTransLen_` (remplacé par
  launchStep dans le plan), le bloc transition de `timerCallback`,
  `SceneManager::updateCrossfade` côté GUI (les rampes deviennent des GainRamp
  audio-thread), `applyScene` est découpé : la partie données → ScenePreloader +
  events ; la partie UI → callback différé post-transition.
- `captureCurrentScene()` reste sur le message thread, appelé à la demande de
  transition (état de A figé au moment du request).
- Le morphing du PingPongDelay (4 s) est conservé, déclenché à launchStep.

---

# 6. Tests d'acceptation

1. **T-TR1 KEEP** : slot identique A/B, loop en lecture → aucune discontinuité dans
   la sortie du slot pendant toute la transition (diff échantillon à échantillon
   avec une lecture sans transition : identique).
2. **T-TR2 BUILD 2→6** : vérifier l'ordre et les steps d'entrée (KICK@0, BASS@4,
   PERC@8/12, PAD mesure suivante), aucune entrée avant launchStep.
3. **T-TR3 BREAKDOWN 6→2** : sorties échelonnées dans la dernière mesure de A,
   KICK/BASS tiennent jusqu'à launchStep, scène B démarre au step 0 avec les seuls
   slots de B.
4. **T-TR4 DUB** : delaySend du slot sortant monte avant son mute ; le delay
   contient de l'énergie du slot après le mute.
5. **T-TR5 preload lent** : simuler un stretch de 3 s → la transition vise la
   frontière suivante, la scène A joue sans glitch, jamais de chargement sur le
   message thread après ARMED.
6. **T-TR6 abort** : stop transport en pleine EXECUTING → état final de B appliqué,
   pas de voix orpheline, pas de fade fantôme au restart.
7. **T-TR7 REPLACE** : slot avec fichier différent → l'ancien fade pendant que le
   nouveau attaque au step planifié, chevauchement sans clic.
8. **T-TR8 événements sample-accurate** : launchStep tombant en milieu de bloc →
   PatternFlip et triggers du step 0 exécutés à l'offset intra-bloc exact.
9. **Test manuel live** : enchaîner 10 transitions A↔B (2 slots ↔ 6 slots) à
   140 BPM pendant 5 min : zéro clic, zéro trou, UI fluide (pas de frame > 50 ms).

# 7. Ordre d'implémentation suggéré (un commit par étape)

1. TransitionPlan.h + SceneTransitionEngine (exécution d'un plan fabriqué à la main
   dans un test — sans SceneDiff).
2. ScenePreloader (extraction depuis preloadSceneAsync/loadSampleIntoSlot).
3. SceneDiff : catégories KEEP/EXIT/ENTER/REPLACE + type SMOOTH uniquement.
4. Branchement navigateScene → engine, suppression ancien chemin, tests T-TR1/5/6/7/8.
5. Classification des rôles + BUILD/BREAKDOWN (T-TR2/3).
6. DUB + morphing delay (T-TR4).
7. Sélecteur UI du type de transition + badge d'état (preparing/armed).

# 8. Contraintes RT (rappel, non négociables)

- Thread audio : zéro allocation, zéro lock, zéro IO, zéro appel virtuel inutile.
- Tous les POD à taille fixe ; communication par atomics et double-buffers.
- Chaque fix appliqué aux DEUX chemins du Sampler (process ET processStereo).
- Ne pas régresser : quantized unmute, loop bleed Option A, StopModes existants.
