# DubEngine V2 — Réécriture complète du moteur audio
## Plan directeur pour Claude Code — remplace TOUS les plans précédents

**Décisions produit (validées par l'utilisateur, non négociables) :**
1. Le moteur audio est réécrit de zéro dans un nouveau module `engine/`.
   L'UI JUCE existante est CONSERVÉE et rebranchée à la fin via une façade.
2. Le BPM du projet est FIXE (réglé par l'utilisateur, il ne change pas en
   cours de session). Un sample importé dont le BPM diffère de celui du projet
   doit fonctionner sans aucune manipulation : ce n'est jamais un problème.
3. **Transparence absolue à l'import** : un sample importé doit sonner
   EXACTEMENT comme en dehors de l'app — même tonalité, même timbre, mêmes
   attaques. Aucun traitement destructif, jamais. La tonalité projet est
   définie MANUELLEMENT par l'utilisateur ; aucune transposition automatique
   sauf non-conformité à cette tonalité ET consentement (cf. §5).
4. Le mixage est 100 % automatique, style dub, sans intervention utilisateur,
   hyper réactif. Déterministe (règles DSP) ; l'inférence ONNX sort du chemin
   par défaut.
5. La désynchronisation rythmique doit être IMPOSSIBLE PAR CONSTRUCTION, pas
   corrigée après coup.

**Technologie :** JUCE/C++ conservé. Nouvelle dépendance :
**Signalsmith Stretch** (https://github.com/Signalsmith-Audio/signalsmith-stretch,
licence MIT, header-only) pour tout time-stretch/pitch-shift, temps réel comme
offline. Le WSOLA maison est abandonné.

---

# 1. Principes d'architecture (à respecter dans chaque fichier)

- **Une seule horloge.** `Transport` est l'unique source de vérité temporelle :
  une position en samples (int64, monotone), le BPM projet, la signature. Tout
  le reste — steps, phases de loop, événements, transitions — est CALCULÉ à
  partir de cette position. Aucun autre compteur temporel n'a le droit
  d'exister dans le moteur.
- **Position dérivée, jamais intégrée.** Une loop synchronisée ne fait jamais
  `pos += rate` : sa position source est recalculée à chaque bloc comme une
  fonction pure de la position transport (voir §4.3). La dérive est donc
  structurellement impossible — il n'y a rien à resynchroniser.
- **Tout est événement sample-accurate.** Triggers, mutes, changements de
  scène, fades : des événements horodatés en samples transport, exécutés à
  l'échantillon près par le scheduler dans le thread audio. Aucune décision de
  timing ne vient du message thread ou d'un timer GUI.
- **Le PCM original est immuable.** Chaque slot conserve le PCM décodé du
  fichier, jamais modifié. Toute conformation (tempo, pitch) est appliquée EN
  LECTURE, en temps réel, réversible instantanément. Zéro pipeline destructif.
- **RT strict** : dans le callback audio, zéro allocation, zéro lock, zéro IO,
  zéro exception. Échanges GUI↔audio par atomics, files lock-free (porter
  `LockFreeQueue.h`/`RingBuffer.h` existants, qui sont sains) et double-buffers
  à flip atomique.
- **Le moteur est une lib testable sans UI ni carte son** : `engine/` compile
  en cible statique + une cible de tests Catch2 qui pousse des blocs et vérifie
  des sorties. Chaque jalon (§8) a ses tests AVANT le branchement UI.

## 1.1 Arborescence

```
src/engine/
  Transport.h              // §2
  EventScheduler.h         // §3
  SlotPlayer.h/.cpp        // §4
  StretchConform.h/.cpp    // §4.4 (wrapper Signalsmith par slot)
  Sequencer.h              // §3.2
  SceneStore.h             // §6.1
  TransitionEngine.h/.cpp  // §6
  AutoMixDub.h/.cpp        // §7
  ImportPipeline.h/.cpp    // §5 (message thread)
  Analysis.h/.cpp          // §5.2 (BPM/key/rôle — porte l'existant)
  EngineFacade.h/.cpp      // §9 (pont vers l'UI actuelle)
  fx/                      // PingPongDelay, MasterLimiter portés tels quels
third_party/signalsmith-stretch/   // vendored, MIT
```

## 1.2 Repris de l'ancien code (à PORTER, pas à réécrire)
`PingPongDelay` (sain, garder le morphing), `MasterLimiter`,
`FeatureExtractor`/`AiContentClassifier` (classification des rôles),
`BpmDetector` (avec correction d'octave ajoutée, §5.2), `KeyDetector`,
`LockFreeQueue`, `RingBuffer`, `SerumHost`, toute l'UI (`src/ui/`, MIDI).

## 1.3 Supprimé (ne rien en reprendre)
`Sampler.*`, `StepSequencer.h`, `BeatClock.h`, `SceneManager.h`,
`SmartSamplerEngine.h`, `WsolaShifter.*`, `autoMatchSampleAsync` et tout le
pipeline d'import de MainComponent, `AiMixEngine` (le fichier peut rester
compilé derrière un flag OFF par défaut), `DspPipeline` (remplacé par
EngineFacade + graphe fixe §7.4).

---

# 2. Transport

```cpp
struct TransportState {            // POD, snapshot par bloc
    int64_t samplePos;             // position absolue en samples, monotone
    double  sampleRate;
    double  bpm;                   // fixe pendant la session
    double  samplesPerBeat;        // sampleRate * 60 / bpm (précalculé)
    double  samplesPerStep;        // samplesPerBeat / 4 (double-croches)
    bool    playing;
};
```

- `Transport::advance(numSamples)` appelé UNE fois par bloc, en tête de
  callback. Tout le moteur reçoit le même snapshot const.
- Helpers purs : `beatAt(samplePos)`, `stepIndexAt(samplePos)` (int64
  monotone, PAS de modulo global arbitraire), `samplesUntilStep(n)`,
  `nextBoundary(stepsPerCycle)`.
- Swing : décale les steps impairs à la GÉNÉRATION des événements (§3.2),
  jamais en mutant l'horloge.
- Play/stop : `samplePos` repart de 0 au play (départ propre) ; le stop coupe
  via fades courts planifiés par le scheduler.

**Tests T-TR :** monotonie ; `stepIndexAt` exact sur 10^9 samples (pas d'erreur
d'arrondi float — utiliser double + int64) ; frontières de step stables pour
bpm non ronds (ex. 133.7).

# 3. EventScheduler + Sequencer

## 3.1 Scheduler

File d'événements triée par temps, en samples transport absolus :

```cpp
struct EngineEvent {
    int64_t  time;      // sample transport absolu
    uint8_t  type;      // Trigger, Release, Mute, Unmute, GainRamp, SceneFlip,
                        // TransposeSet, SendRamp, PerfFx…
    uint8_t  slot;
    float    a, b;      // params (gain cible, durée rampe en samples, semitones…)
};
```

- Deux sources d'événements : le Sequencer (généré dans le thread audio,
  §3.2) et le message thread (file lock-free entrante ; les événements UI
  portent soit `time = ASAP` soit une frontière quantisée calculée par le
  moteur, jamais par l'UI).
- Exécution : à chaque bloc, le scheduler découpe le bloc aux timestamps des
  événements (split-processing) : les sous-segments sont rendus entre chaque
  événement → tout est à l'échantillon près, y compris les fades qui démarrent
  mi-bloc. Capacité fixe (256), overflow = drop + compteur debug (jamais
  d'allocation).

## 3.2 Sequencer (fonction pure du transport)

- Patterns : double-buffer à flip atomique (préparé par le message thread,
  flippé sur événement SceneFlip — reprendre ce principe, seul bon point de
  l'ancien design).
- À chaque bloc : pour chaque step dont l'heure (avec swing) tombe dans
  `[samplePos, samplePos + numSamples)`, émettre un événement Trigger à
  l'échantillon exact. AUCUN état interne de phase : l'index de step découle
  de `stepIndexAt()`. Longueurs de pattern par piste en steps ; l'index de
  piste = `stepIndexAt() % trackSteps` sur int64 (pas de wrap artificiel).
- Reprendre les comportements validés : mute/unmute quantisé au step 0 de la
  piste (via événements planifiés à `nextBoundary`), loop bleed « Option A ».

**Tests T-SEQ :** step tombant mi-bloc → trigger à l'offset exact ; swing 60 %
→ steps impairs décalés de la valeur théorique ±1 sample ; 15 min à 140 BPM →
l'écart entre le trigger n et n+1 est CONSTANT (±0) ; patterns de longueurs
mixtes (16/32/64) alignés sur 10 000 steps.

# 4. SlotPlayer — lecture transparente et conformation temps réel

9 slots. Par slot : PCM original immuable (float, SR projet — l'unique
conversion tolérée à l'import est le resampling de SR fichier→device, en sinc
fenêtré), 2 voix pour les chevauchements, paramètres atomiques.

## 4.1 Modes de lecture par slot

- **ONE-SHOT (défaut pour les rôles percussifs)** : lecture directe du PCM,
  SANS AUCUN traitement — le chemin est bit-transparent (pas de fade-in au
  trigger si le sample démarre < −60 dB ; sinon micro-fade 16 samples
  linéaire). Le stretch n'existe pas sur ce chemin.
- **LOOP SYNC (loops rythmiques dont le BPM est connu/confirmé)** : lecture à
  travers StretchConform (§4.4) avec `timeRatio = bpmSample / bpmProjet`,
  hauteur STRICTEMENT constante. Position dérivée (§4.3) → verrouillée à la
  grille par construction.
- **FREE (tout le reste : pads, FX, vocals, BPM inconnu)** : lecture directe
  transparente, loop optionnelle sur la longueur brute, non alignée. Jamais de
  stretch sans opt-in.
Le mode est proposé par l'analyse (§5.2) et TOUJOURS modifiable par
l'utilisateur (l'UI a déjà les toggles ; brancher via la façade).

## 4.2 Verrouillage à la grille — le cœur anti-désync

Pour un slot LOOP SYNC, la longueur musicale est
`loopBeats = round(durOriginale / samplesPerBeatAuSampleBpm)` (validée à
l'analyse, éditable). Sa durée en samples projet :
`loopLenProject = loopBeats × samplesPerBeat` (exacte, en double).

## 4.3 Position dérivée

À chaque sous-segment rendu, la position SOURCE demandée au conformeur est :

```
phase        = ((t − anchor) mod loopLenProject) / loopLenProject   // t = sample transport
srcPos       = phase × durOriginaleSamples
```

`anchor` = le sample transport du trigger (toujours une frontière de step →
les loops d'un même pattern partagent la même phase). Le moteur ne stocke PAS
de readPos qui s'incrémente : il DEMANDE la position exacte. Conséquences :
- dérive impossible (pas d'intégration d'erreur) ;
- un re-trigger sur la même loop est un no-op exact (même anchor modulo la
  longueur) → le « legato » est gratuit et toujours juste ;
- le wrap de boucle est un simple modulo — pas de crossfade qui raccourcit la
  période (le bug historique) ; la continuité au point de bouclage est
  assurée par un pré-blend de 512 samples calculé UNE fois à l'import dans
  une copie de travail (l'original reste intact), période inchangée.

## 4.4 StretchConform (wrapper Signalsmith, un par slot)

- `signalsmith::stretch::SignalsmithStretch<float>` en mode temps réel :
  `presetDefault(channels, sampleRate)`, `setTransposeSemitones(st)` pour la
  transposition (§5.3), lecture par `process()` alimenté depuis srcPos.
- **Bypass total** quand `timeRatio == 1.0 && semitones == 0` : le wrapper
  court-circuite Signalsmith et copie le PCM (chemin bit-transparent). Le
  passage bypass↔actif se fait par crossfade 256 samples planifié par le
  scheduler.
- **Latence** : Signalsmith reporte `inputLatency()/outputLatency()`. Le
  wrapper compense en lisant la source en avance de `inputLatency` et le
  scheduler retarde le début audible de `outputLatency`… NON — plus simple et
  robuste : pré-rouler le stretcher au moment du PREMIER trigger prévu (le
  Sequencer connaît les triggers une frontière à l'avance pour les slots
  SYNC : pré-nourrir `inputLatency` samples juste avant l'échéance, sur le
  thread audio, coût négligeable). L'attaque sort à l'échantillon prévu.
  Écrire le test T-SP4 AVANT d'implémenter, il pilote le design.
- CPU : ~1–2 % par instance active à 44,1 k ; 9 slots dont typiquement 2–4 en
  stretch actif = budget OK. Compteur de charge du callback (max block time)
  loggé en debug, assert < 50 % du budget.

**Tests T-SP :**
1. T-SP1 (transparence) : slot ONE-SHOT/FREE → sortie bit-identique au PCM
   original (tolérance 0), pour kick, pad, vocal, loop non-sync.
2. T-SP2 (verrouillage) : loop 2 mesures à 126 BPM, projet 120, LOOP SYNC →
   le downbeat de la loop coïncide avec le step 0 à ±1 sample, pendant
   1 000 cycles (aucune dérive mesurable).
3. T-SP3 (hauteur constante) : sinusoïde 220 Hz dans une loop stretchée
   126→120 → fréquence de sortie 220 Hz ±5 cents.
4. T-SP4 (latence) : impulsion au beat 1 de la loop → sort à l'échantillon
   du beat 1 du projet ±32 samples.
5. T-SP5 (bypass) : timeRatio 1.0 → chemin identique à T-SP1 (aucun passage
   par Signalsmith, vérifié par instrumentation).

# 5. ImportPipeline + Analysis (message thread)

## 5.1 Import
1. Décodage → conversion SR device (sinc) si nécessaire → **stockage PCM
   original immuable** (budget global 300 Mo, LRU sur les scènes non
   voisines).
2. Chargement IMMÉDIAT dans le slot en mode FREE (transparent) : l'utilisateur
   entend son sample tel quel en < 200 ms, TOUJOURS. C'est l'exigence n°1.
3. Analyse en tâche de fond (§5.2) → badges UI ; passage éventuel en LOOP
   SYNC selon §5.2. Jamais de popup bloquant.

## 5.2 Analysis
- Rôle (classifieur existant) ; percussif → ONE-SHOT, terminé.
- BPM (BpmDetector porté) avec **correction d'octave** : retenir parmi
  {bpm, ×2, ÷2} le plus proche du BPM projet ; et `loopBeats` doit tomber à
  < 2 % d'un entier de temps, sinon le sample n'est PAS une loop calable →
  reste FREE avec badge « BPM ? ».
- Passage auto en LOOP SYNC seulement si : rôle rythmique (LOOP/BREAK/BASS),
  confiance ≥ 0,75, ratio dans [0,8 ; 1,25]. Sinon : FREE + badge cliquable
  « 126 BPM → caler à 120 ? » (l'action utilisateur force le mode). Le BPM et
  loopBeats restent éditables (champ UI existant).
  NB : même le passage auto en LOOP SYNC préserve exactement la tonalité
  (stretch à hauteur constante) — il ne modifie que la vitesse, de ≤ 25 %,
  et il est réversible d'un clic (retour FREE = retour au son strictement
  original). C'est ce qui le rend acceptable par défaut.
- Tonalité (KeyDetector) : purement informative → badge. Voir §5.3.

## 5.3 Tonalité — invariant (repris tel quel, il fait foi)
La tonalité projet est saisie MANUELLEMENT (vide par défaut = aucun key-match
possible nulle part). Une transposition automatique n'existe que si TOUTES ces
conditions sont vraies : tonalité projet définie ∧ détection confiante
(≥ 0,7) ∧ NON-conformité réelle (tonique identique ou relative maj/min =
conforme = zéro traitement) ∧ toggle key-match du slot ON (off par défaut) ∧
rôle MELODIC/BASS/PAD (jamais KICK/PERC/FX). Sinon : badge orange cliquable =
suggestion (plus court chemin, ±7 st max). Toute transposition passe par
`setTransposeSemitones` du conformeur : TEMPS RÉEL, visible dans le champ
transpose du slot, annulable instantanément (retour à 0 = retour au son
original exact, puisque rien n'a été écrit).

**Tests T-IM :** T-IM1 : import → audible en FREE transparent avant la fin de
l'analyse. T-IM2 : loop 63 BPM détectée, projet 120 → octave corrigée à 126,
ratio 0,95, LOOP SYNC. T-IM3 : sample conforme à la tonalité projet, tous
toggles ON → semitones == 0, bypass intact. T-IM4 : tonalité projet non
définie → aucun transpose nulle part. T-IM5 : pad 30 s → jamais LOOP SYNC
auto.

# 6. Scènes et TransitionEngine

Reprendre INTÉGRALEMENT la conception de PLAN_REFONTE_TRANSITIONS.md
(TransitionPlan POD, machine IDLE→PREPARING→ARMED→EXECUTING→SETTLING, diff
KEEP/EXIT/ENTER/REPLACE, types SMOOTH/BUILD/BREAKDOWN/DUB/CUT selon la
densité, ordonnancement par rôles, préparation 100 % terminée avant armement),
avec ces adaptations à la V2 :

- Les événements du plan sont des `EngineEvent` ordinaires poussés au
  scheduler (§3.1) — le TransitionEngine ne fait plus son propre timing, il
  COMPILE le plan en événements à l'armement. Exécution sample-accurate
  gratuite.
- KEEP est encore plus fort en V2 : slot identique (fichier+trim+mode+
  transpose) → on ne touche RIEN, pas même l'anchor — la position dérivée
  garantit la continuité parfaite à travers la frontière.
- La « préparation » se réduit à : décoder les PCM manquants (originaux
  immuables), pré-rouler les stretchers des slots ENTER, préparer le
  double-buffer de patterns. Plus aucun stretch offline à faire → PREPARING
  dure < 500 ms dans le pire cas disque froid.
- DUB throw : événements SendRamp vers le PingPongDelay porté + mute — comme
  spécifié.
- Tonalité par scène : conserver §C3 (conditionnel, uniquement slots
  non-conformes, via TransposeSet temps réel).

**Tests T-TX :** reprendre T-TR1…T-TR8 du plan transitions tels quels, plus
T-TX9 : slot KEEP en LOOP SYNC → sortie strictement continue à travers la
frontière (null-test contre lecture sans transition).

# 7. AutoMixDub — ingénieur son automatique (déterministe, réactif)

Remplace SmartSamplerEngine/AiMixEngine. Zéro intervention utilisateur, zéro
ONNX dans le chemin par défaut (`SAXFX_HAS_ONNX` reste compilable, OFF).

## 7.1 Boucle de réaction
- Thread audio : FeatureExtractor léger par slot sur la sortie POST-player
  (RMS, centroïde, crête, fractions basse/médium/aigu) accumulé dans des
  atomics par fenêtre de 2048 samples. Coût quasi nul.
- Thread de mix dédié (pas le GUI) : réveil toutes les **50 ms**, lit les
  features, calcule les décisions (règles ci-dessous), pousse des CIBLES
  (gains, EQ, sends) en atomics.
- Thread audio : rampes exponentielles vers les cibles, constantes de temps
  30 ms (gains) / 120 ms (EQ, sends). Jamais de saut → réactif ET sans
  artefact. Hystérésis ±1 dB sur chaque décision pour éviter le pompage des
  règles.

## 7.2 Règles (dans cet ordre)
1. **Gain staging par rôle** : loudness cible par slot (RMS pondéré A,
   fenêtre 400 ms) relative au kick = 0 dB : BASS −2, SNARE/PERC −4,
   MELODIC −8, PAD −12, FX −10. Correction bornée ±9 dB, appliquée en rampes.
2. **Anti-masquage complémentaire** : si deux slots ont un chevauchement
   spectral fort (corrélation des fractions de bandes > 0,8 et tous deux
   > −25 dB), creuser la bande de conflit de −2,5 dB sur le slot au rôle le
   moins prioritaire (priorité : KICK > BASS > SNARE > MELODIC > PERC > PAD >
   FX). Cas câblé en dur en plus de la règle générale : kick vs bass →
   shelf bas −3 dB sur la bass à 80 Hz.
3. **Sidechain dub** : enveloppe du kick (attack 5 ms, release 120 ms) module
   −4 dB max le gain de BASS et PAD. C'est LE glue du mix dub.
4. **Sends automatiques** : par rôle, sends vers le PingPongDelay (SNARE/PERC
   0,25 ; MELODIC 0,15 ; les autres 0) et, si présent, la reverb. Les
   transitions (DUB throw) peuvent surcharger temporairement ces sends via
   événements — la règle reprend la main en SETTLING.
5. **Espace stéréo** : pan léger par rôle (KICK/BASS centre, PERC ±20 %,
   MELODIC ±12 % alternés par slot) — statique par scène, recalculé au flip.
6. **Master** : MasterLimiter porté ; si gain reduction > 3 dB soutenue sur
   2 s, retirer 1 dB au gain staging global (protection anti-écrasement).

## 7.3 Réactivité aux changements
Nouveau sample importé, mute/unmute, transition de scène → le thread de mix
converge en 3–6 cycles (150–300 ms) grâce aux rampes. Pré-amorçage : à
l'armement d'une transition, les features des slots ENTER sont estimées depuis
leur analyse d'import (RMS/spectre du fichier) → les cibles de la scène B sont
posées AVANT la frontière, le mix est déjà bon au premier temps.

## 7.4 Graphe audio fixe (remplace DspPipeline)
`9 × SlotPlayer → [gain slot ← AutoMix] → [EQ 3 bandes ← AutoMix] →
[sidechain] → bus mix + bus sends (delay, reverb) → SerumHost (inséré tel
qu'aujourd'hui) → MasterLimiter → out`. Ordre figé, pas de graphe dynamique.

**Tests T-MX :** T-MX1 : deux sinusoïdes même bande, rôles BASS vs PAD → le
PAD est atténué dans la bande, la BASS intacte. T-MX2 : kick périodique →
gain de la bass module à la période du kick, profondeur ≤ 4 dB. T-MX3 :
aucune décision ne saute (dérivée du gain bornée). T-MX4 : mix silencieux →
aucune décision NaN/dérive, retour aux défauts. T-MX5 (déterminisme) : deux
runs sur le même matériel → décisions identiques.

# 8. Ordre d'implémentation (jalons stricts, un commit par item, tests inclus)

M1. Squelette `engine/` + cible test + vendor Signalsmith. Transport +
    tests T-TR.
M2. EventScheduler + split-processing + tests. Sequencer + tests T-SEQ.
M3. SlotPlayer modes ONE-SHOT/FREE (bypass uniquement) + T-SP1. ⟵ à partir
    d'ici la transparence est verrouillée par test pour toujours.
M4. StretchConform + position dérivée + T-SP2…T-SP5.
M5. ImportPipeline + Analysis (ports BpmDetector/KeyDetector/classifieur,
    correction d'octave) + T-IM. Invariant tonalité §5.3.
M6. SceneStore + TransitionEngine + T-TX (y compris types BUILD/BREAKDOWN/DUB).
M7. AutoMixDub + graphe fixe + ports PingPongDelay/MasterLimiter/SerumHost +
    T-MX.
M8. EngineFacade : exposer à l'UI existante la surface qu'elle consomme déjà
    (triggers, mutes, patterns, scènes, BPM/tonalité projet, badges, waveform,
    Serum, MIDI). Adapter MainComponent : SUPPRIMER toute logique métier qui
    y vivait (import, transitions, crossfades, timers de mix) — il ne reste
    que du binding UI ↔ façade. Suppression de `src/dsp/` (hors fichiers
    portés dans engine/fx et engine/Analysis).
M9. Recette live : session 30 min, 8 scènes, imports à chaud, transitions
    dans les deux sens de densité, enregistrement de la sortie pour
    inspection. Compteur CPU < 50 % budget.

Règles pour Claude Code : ne jamais commencer un jalon si les tests du
précédent ne passent pas ; ne rien copier depuis l'ancien Sampler/StepSequencer
(interdit — c'est la source des bugs) ; chaque commit compile et teste vert ;
en cas d'ambiguïté sur un comportement UI existant, reproduire l'existant et
noter la question dans un fichier QUESTIONS.md plutôt que d'inventer.

# 9. Ce que l'utilisateur doit constater à la fin (critères de recette)

1. J'importe n'importe quel sample : je l'entends immédiatement, EXACTEMENT
   comme en dehors de l'app.
2. Une loop à 126 BPM dans mon projet à 120 tombe juste sur la grille, sans
   que je fasse quoi que ce soit, et sa tonalité n'a pas bougé.
3. Rien ne dérive : après 30 minutes, kick et loops sont alignés comme à la
   première mesure.
4. Les transitions de scène (2 slots ↔ 6 slots) sont musicales, sans glitch,
   sans freeze, et ce qui est commun aux deux scènes ne s'interrompt jamais.
5. Le mix se fait tout seul, sonne dub (sidechain, delays), réagit en une
   fraction de seconde à ce que je fais, et je n'ai touché aucun réglage.
6. La seule transposition qui existe est liée à la tonalité que J'AI définie,
   ne s'applique qu'aux samples non conformes, et je peux l'annuler d'un clic.

# 10. AMENDEMENTS POST-REVUE — priment sur les sections qu'ils corrigent

## 10.1 Tranche verticale anticipée (corrige §8)
Nouveau jalon **M4bis** (obligatoire, juste après M4) : EngineFacade MINIMALE
branchée dans l'app réelle — import d'un sample, trigger par pad, lecture des
3 modes (ONE-SHOT/FREE/LOOP SYNC) avec le séquenceur, sortie audio. Objectif :
l'utilisateur écoute et valide À L'OREILLE la transparence et le verrouillage
avant que M5–M7 ne soient construits. Les fonctionnalités absentes affichent
un état désactivé dans l'UI, elles ne crashent pas. M8 devient l'extension de
cette façade, plus sa création.

## 10.2 Position dérivée : périmètre exact (corrige §4.3–4.4 et T-SP2)
- Chemin BYPASS (ONE-SHOT, FREE, LOOP SYNC à ratio 1) : position dérivée
  stricte, garantie ±0 sample. Inchangé.
- Chemin STRETCH (LOOP SYNC ratio ≠ 1) : Signalsmith est streaming — la
  position dérivée s'applique à l'ENTRÉE (on nourrit la source cadencée par
  le transport), et la sortie est RECALÉE à chaque frontière de boucle :
  comparer la phase de sortie théorique à la phase transport ; écart > 1 ms →
  correction par micro-crossfade (256 samples) vers la position exacte.
  Garantie honnête : dérive NON CUMULATIVE, bornée à < 1 ms, recalée à chaque
  cycle.
- **T-SP2 corrigé** : downbeat de la loop aligné au step 0 à ±2 ms à CHAQUE
  cycle sur 1000 cycles, et l'écart du cycle N ne dépend pas de N
  (non-cumulatif, pente de régression < 0,01 ms/cycle). Le critère ±1 sample
  ne s'applique qu'au chemin bypass (T-SP1/T-SP5).

## 10.3 Pré-roll du stretcher (corrige §4.4)
Le pré-roll (inputLatency samples) ne se fait JAMAIS en un seul callback.
Il est AMORTI : dès qu'un trigger LOOP SYNC est planifié (connu au moins un
step à l'avance), le player nourrit le stretcher par tranches ≤ 256 samples
par bloc sur les blocs précédant l'échéance. Budget vérifié par le compteur
de charge. Si l'échéance arrive avant la fin du pré-roll (trigger immédiat
non planifié) : démarrage en bypass + bascule crossfadée vers le chemin
stretch dès qu'il est prêt (< 100 ms) — le son part à l'heure dans tous les
cas.

## 10.4 Hors périmètre V2.0 — explicitement reporté en V2.1 (corrige §1.3/§8)
NE PAS implémenter, NE PAS improviser : LooperEngine (l'UI du looper est
masquée/désactivée en V2.0 ; le port — position dérivée + rate — est spécifié
en V2.1), effets de performance (tape stop, glide, reverse), half/double-time,
tap tempo, rampe de BPM. Toute trace de ces features dans l'ancien code est
ignorée. Les items correspondants de l'ancien backlog seront re-spécifiés
sur la base du moteur V2 une fois M9 recetté.

## 10.5 AutoMix : répartition thread audio / thread de mix (corrige §7)
- THREAD AUDIO (par échantillon ou par bloc) : sidechain kick→BASS/PAD
  (détecteur d'enveloppe attack 5 ms / release 120 ms, gain appliqué par
  échantillon) ; filterbank 3 bandes bon marché (biquads Linkwitz-Riley
  100 Hz / 2,5 kHz) par slot pour les énergies de bandes ; RMS et crête.
  PAS de FFT ni de centroïde sur le thread audio.
- THREAD DE MIX (50 ms) : centroïde et features spectrales calculés depuis
  un ring buffer de la sortie des slots ; toutes les règles 1, 2, 4, 5, 6 ;
  publication des cibles.
- Test T-MX2 précisé : la modulation sidechain suit l'enveloppe du kick à
  l'échantillon près (pas de marches de 50 ms).

## 10.6 Précisions manquantes (complète §4/§5/§8)
- **Canaux** : PCM originaux conservés dans leur format (mono ou stéréo) ;
  le player sort toujours stéréo (mono dupliqué) ; un seul chemin de rendu
  stéréo — pas de duo process/processStereo, c'était une source de bugs
  dupliqués.
- **Device** : changement de sample rate ou de buffer size → le moteur se
  re-prépare (re-resample des originaux depuis le fichier en tâche de fond,
  stretchers re-préparés) ; le transport repart de 0 ; jamais de crash ni
  d'état hybride. Test T-TR4 : prepare(44100) → prepare(48000) → lecture
  correcte.
- **QUESTIONS.md** : chaque fin de jalon = point d'arrêt obligatoire ; si le
  fichier contient des questions ouvertes, Claude Code s'arrête et les
  présente à l'utilisateur au lieu d'enchaîner le jalon suivant.
- **M8 découpé** en 3 sous-jalons : M8a inventaire de la surface UI réellement
  consommée (liste exhaustive des appels de MainComponent vers l'ancien
  moteur, générée par grep, validée avant de coder) ; M8b façade complète +
  bascule ; M8c purge de l'ancienne logique de MainComponent et suppression
  de src/dsp/. Chaque sous-jalon compile et l'app démarre.

# 11. AMENDEMENTS SECONDE REVUE — priment sur les sections qu'ils corrigent

## 11.1 Hiérarchie normative de ce document (s'applique à TOUT le plan)
- **INVARIANTS — non négociables** : horloge unique (§2), position dérivée /
  non-dérive (§4.3, 10.2), PCM original immuable, transparence à l'import
  (§5, critères de recette §9), invariant tonalité (§5.3), contraintes RT
  (§1), mix automatique sans intervention (§7), transitions content-aware
  jamais interrompues sur les slots KEEP (§6), points d'arrêt QUESTIONS.md.
- **TESTS — obligatoires** ; un critère chiffré peut être ajusté uniquement
  avec justification écrite dans QUESTIONS.md, jamais supprimé.
- **DÉTAILS D'IMPLÉMENTATION — points de départ** : structures exactes,
  constantes, seuils DSP, découpages de classes. Claude Code PEUT en dévier
  si une solution plus simple ou plus robuste préserve invariants et tests ;
  chaque déviation est documentée en une ligne dans le message de commit.

## 11.2 Rendu offline + null-tests de régression (complète §1/§8)
- Le moteur expose `renderOffline(sessionState, numSamples) → buffer` :
  rendu déterministe complet (séquenceur, transitions, automix) sans device.
- Outil `tools/nulltest` : rend une session de référence (fixture committée :
  4 scènes, 6 samples de test, 2 transitions) et compare bit à bit au rendu
  de référence stocké (hash + diff RMS par fenêtre). Intégré à la cible de
  test : TOUT commit doit passer le null-test ; un changement de rendu
  INTENTIONNEL régénère la référence dans le même commit, avec justification.
- Déterminisme requis : aucune source de temps réel ni d'aléa non seedé dans
  le moteur (le thread de mix est simulé en offline : décisions recalculées
  toutes les 50 ms de temps rendu).

## 11.3 Journal d'événements (complète §3)
En build debug : ring buffer de 4096 entrées `{samplePos, type, slot, a, b}`
rempli par le Scheduler à l'exécution de chaque événement, dumpable en texte
(`engine.dumpEventLog()`) et automatiquement joint aux échecs de tests.
Coût release : compilé hors du binaire (constexpr).

## 11.4 Instrumentation CPU (corrige §4.4/§8)
Le compteur de charge du callback publie : max, moyenne, p95, p99 (fenêtre
glissante 10 s), consultables par la façade et affichés en debug dans l'UI.
Critère M9 : p99 < 50 % du budget de bloc, max < 80 %.

## 11.5 AutoMix découpé (corrige §7 — le mix automatique reste en V2.0)
- **V2.0 (AutoMix v1)** : règle 1 (gain staging par rôle), règle 3
  (sidechain kick→BASS/PAD, thread audio), règle 4 simplifiée (sends delay
  STATIQUES par rôle), règle 6 (protection limiteur), rampes et hystérésis
  (§7.1/10.5). C'est le cœur du son dub automatique.
- **V2.1** : règle 2 (anti-masquage dynamique), règle 5 (panorama auto),
  sends adaptatifs, pré-amorçage des cibles aux transitions (§7.3 — en V2.0,
  la convergence post-transition en 150–300 ms suffit).
- Les tests T-MX1 (anti-masquage) passent en V2.1 ; T-MX2, T-MX3, T-MX4,
  T-MX5 restent en V2.0.

## 11.6 Sémantique des horodatages d'événements (précise §3.1)
Deux valeurs possibles, aucune autre : `kNextBlock` (exécution au premier
sample du bloc suivant — pads live, actions immédiates) ou un sample transport
absolu calculé PAR LE MOTEUR à partir d'une intention quantisée envoyée par
l'UI (`{action, quantize: Step0Track|Bar|SceneBoundary}`). L'UI n'envoie
jamais de timestamp brut.

# 12. SORT DE L'ANCIENNE ARCHITECTURE — procédure obligatoire

## 12.1 Archivage AVANT toute écriture de code (première action de M1)
1. Committer l'état local complet tel quel : `git add -A && git commit -m
   "archive: état pré-V2 (V1 + implémentations partielles des anciens plans)"`.
2. Créer la branche d'archive : `git branch archive-v1`. Elle ne sera plus
   jamais modifiée ; elle sert uniquement de référence historique.
3. Créer la branche de travail : `git checkout -b v2-engine`.
4. Sur v2-engine : supprimer les anciens documents de plan (PLAN_FIX_SYNC,
   PLAN_REFONTE_TRANSITIONS, PLAN_TEMPO_TONALITE, PLAN_FIX_IMPORT et tout
   autre .md de spécification antérieur). PLAN_V2_ENGINE.md est l'unique
   source de vérité.

## 12.2 Coexistence pendant M1 → M8a
- L'ancien code (`src/dsp/`, la logique de MainComponent) RESTE EN PLACE et
  l'app existante doit continuer à compiler pendant toute la construction du
  moteur : `engine/` est une cible séparée, aucun fichier ancien n'est
  modifié ni supprimé avant M8 (exception : M4bis branche la façade minimale
  dans MainComponent derrière un flag de compilation `DUB_ENGINE_V2`,
  modifications limitées au strict branchement).
- **Interdiction de lecture-copie** : il est interdit de copier du code, des
  algorithmes ou des constantes depuis `Sampler.*`, `StepSequencer.h`,
  `BeatClock.h`, `SceneManager.h`, `SmartSamplerEngine.h`, `WsolaShifter.*`,
  `AiMixEngine.*`, `autoMatchSampleAsync` et le pipeline d'import de
  MainComponent, ainsi que depuis toute implémentation partielle des anciens
  plans (commits récents type « moteur tempo/tonalité », quantized unmute,
  etc.). Ces fichiers sont la SOURCE des bugs ; le moteur V2 est écrit
  uniquement depuis ce document. Consulter l'ancien code est autorisé pour
  UNE seule chose : inventorier des comportements produit à reproduire
  (valeurs par défaut UI, mappings MIDI) — jamais des mécanismes internes.

## 12.3 Procédure de portage (fichiers listés en §1.2 uniquement)
Pour chaque fichier autorisé (PingPongDelay, MasterLimiter, FeatureExtractor,
AiContentClassifier, BpmDetector, KeyDetector, LockFreeQueue, RingBuffer,
SerumHost) : COPIER le fichier vers `engine/fx/` ou `engine/Analysis` (jamais
d'include croisé vers src/dsp/), adapter les includes/namespace au module
engine, ajouter au minimum un test de fumée (compile, instancie, traite un
bloc sans NaN), puis ne plus jamais le resynchroniser avec l'original. Toute
correction (ex. octave du BpmDetector) se fait sur la COPIE portée.

## 12.4 Suppression (M8c) — checklist de purge
Une fois M8b recetté (l'app tourne intégralement sur la façade) :
1. Supprimer `src/dsp/` en entier (les originaux des fichiers portés
   compris — les copies vivent dans engine/).
2. Supprimer de MainComponent.* toute la logique métier morte : import/
   autoMatch, transitions/crossfades, timers de mix, gestion des scènes hors
   binding UI. Cible indicative : MainComponent ≤ ~800 lignes de binding pur.
3. ✔ FAIT (commit `ca7c577`) — flag `DUB_ENGINE_V2` supprimé (le moteur V2
   devient le seul chemin de compilation).
4. Vérifier par grep qu'aucune référence aux classes supprimées ne subsiste
   (Sampler, StepSequencer, SceneManager, DspPipeline, WsolaShifter,
   SmartSamplerEngine, AiMixEngine, BeatClock).
5. Le null-test (§11.2) et la recette M9 passent après purge — la
   suppression ne doit rien changer au rendu.
6. Commit dédié « purge V1 » ; l'ancienne architecture reste consultable
   uniquement via la branche archive-v1.

État M8c (session v2-engine) — purges déjà effectuées : diagnostics temporaires
(`194a9c1`), preload V1 async (`b2c2ba5`), lectures `isLoaded` → facade
(`54662b3`), rôle V2 fiable prioritaire (`8b7d6ee`), écritures delay-send /
sidechain V1 mortes (`aa451df`), flag supprimé (`ca7c577`).
⛔ BLOQUANT restant : le magic mix V1 (`SmartSamplerEngine`) lit toujours le
PCM du sampler V1 (`getSlotPcmView`) pour analyser les 8 scènes pendant
`applyMagicMix()` ; `MainComponent` consomme ses résultats (tags UI, spatial
viz, `onDone` → `sc.userGains`, bouton ⚡, `aiCloud_`). Tant qu'il n'est pas
porté dans le moteur V2 (recette M9), `src/dsp/` ne peut pas être supprimé.

### État M9 (portage magic mix → engine/mix/)
Le portage suit un découpage en étapes (voir rapport M9) ; chaque étape garde le
nulltest bit-exact (`0xcaf8b973768f00cc` / `0xfb07aa3caefb2d3c`) :

- **Étape 1 ✔ `34265e4`** — `src/engine/mix/MixAlgorithms.h` (header-only, sans
  JUCE) : helpers DSP purs portés (classification, biquads, EQ par rôle,
  unmasking, sub ownership, kick/bass, dub echo, gain/spatial, true-peak).
  Tests MIX-1..7.
- **Étape 10 ✔ `266ec31`** — purge du sous-système on-load mort de
  `SmartSamplerEngine` (`processSlotOnLoad`, `OnLoadWorkerThread`,
  `processSlotData`, BPM/stretch/pitch/cache, `onSlotProgress`,
  `setSampleRate`, setters AI, sidechain V1). −378 lignes ; `WsolaShifter`
  découplé des cibles SaxFXLive/SaxFXTests.
- **Étape 2 ✔ `5935b9e`** — `src/engine/mix/MixDecisions.h` : décisions purs
  (densité de scène → scale, présence bass, type effectif, gain calibré,
  duck Serum). Tests MIX-8/9.
- **Étape 3 ✔ `585da34`** — `src/engine/mix/MixEngine.h` :
  `processHeuristic()` = branche heuristique complète de `applyNeutronMix`
  (phases 1/2/4) sans side-effects sur un player. Tests MIX-10/11.
- **Étape 6 ✔ `aaa9c56`** — spatialisation runtime V2 : pan + Haas dans
  `SlotPlayer` (params atomiques pan/width, loi égal-power V1 + Haas width×25 ms
  sur le canal faible, identité quand pan==width==0 pour préserver la
  transparence T-SP1), dérivée du rôle dans `AudioGraph::setSlotRole`
  (mapping `SlotRole`→`MixContentType` + `spatialForType`, centroid neutre).
  Tests T-SP6a/b/c. NULL1 régénéré (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`)
  — changement de rendu INTENTIONNEL, seul point audible du portage
  (Debug == Release bit-exact vérifié).
- **Étape 7 ✔** — état mix persistant : `engine/mix/MixState.h` (header-only,
  sans JUCE) — `SlotMixState {gain, pan, width, depth, applied}` aligné sur le
  schéma projet v5 (`SlotMixData`), `mixStateFromOutputs()` capture l'état
  depuis `MixOutputs` (slots non traités → défauts, rien à écraser à la
  restauration), `setSlotMixState()`/`slotMixState()` bornés. `EngineFacade`
  expose `setSlotMixState()` (restaure + applique gain/spatial au SlotPlayer)
  et `getSlotMixState()` (sauvegarde projet). Tests MIX-12/13/14. NON audible :
  NULL1 bit-exact inchangé (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).
- **Étape 8 ✔** — worker magic mix asynchrone (façade) : `engine/mix/MixWorker.h`
  (header-only, sans JUCE) — `MixWorkerInputs` = snapshot runtime (PCM mono par
  slot, types, loaded/muted, scène courante, bpm, SR), `toMixInputs()` mappe le
  snapshot vers `MixInputs` (active = loaded && !muted), `runHeuristicMix()`
  = `processHeuristic()` → `mixStateFromOutputs()`. `EngineFacade` expose
  `triggerMagicMix()` (capture le snapshot sur le message thread, exécute le
  mix sur un thread dédié, applique l'état persistant au runtime via
  `setSlotMixState()` sur le message thread, callback `setMagicMixDoneCallback`),
  `isMagicBusy()`/`isMagicActive()`. Mapping `SlotRole`→`MixContentType`
  centralisé dans `AudioGraph::roleToMixType()` (header) + getter
  `AudioGraph::slotRole()`. Tests MIX-15/16. NON audible (déclenché
  explicitement par l'UI, jamais dans renderOffline) : NULL1 bit-exact inchangé
  (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).
- **Étape 4 ✔** — chemin IA ONNX du magic mix : `engine/mix/MixAi.h` (header-only,
  sans JUCE) — `MixAiDecision {volume, lowGain, midGain, highGain}` (sortie
  modèle), `serumCompensateDecision()` (port V1 L930-968 : duck de volume par
  proximité spectrale log2 max 25 % + carving EQ mid/high des slots SYNTH/PAD
  quand Serum est synthé/pad), `processAiMix()` (port V1 L904-1021 : EQ 3 bandes
  DC-block 20 → shelf 100 → peak 2500 → shelf 8000 (+1 dB air bias) → LP 18 kHz,
  gains clampés ±6 dB, kick transient / bass harmonics, gain =
  clamp(0,1.5, volume × saxClearance / truePeak) ; slot 8 DRM = heuristique LOOP ;
  spatialisation + balance L/R identiques au chemin heuristique).
  `MixWorker::runAiMix()` + `EngineFacade::triggerAiMagicMix()` (inférence ONNX
  fournie par l'appelant — src/dsp/ AiMixEngine, SAXFX_HAS_ONNX ; le module ne
  porte que le post-traitement pur). `MixInputs` étendu (contexte Serum complet :
  serumContentType/MidFrac/HighFrac, defaults neutres). Tests MIX-17/18/19.
  NON audible (chemin non déclenché par défaut) : NULL1 bit-exact inchangé
  (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).
- **Étape 5 ✔** — revert du magic mix : `MixState::resetMixState()` (remet tout
  l'état persistant aux défauts : gain 1, spatial neutre, applied=false) +
  `EngineFacade::revertMagicMix()` (reset état + application runtime gain 1 /
  spatial neutre, synchrone message thread — le PCM n'est jamais modifié en V2,
  invariant transparence, donc aucun rechargement fichier comme en V1) +
  `EngineFacade::toggleMagicMix()` (apply si inactif, revert sinon — sémantique
  V1 `toggleMagicMix`). Test MIX-20. NON audible : NULL1 bit-exact inchangé
  (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).

À venir : étape 9 (ré-câblage MainComponent),
11 (réduction surface V1), 12 (purge finale + recette M9).

### Étape 9 ✔ — ré-câblage MainComponent sur la façade (M9)
MainComponent ne consomme plus la surface du magic mix V1 :
- `triggerAI()` → `facade_.setSerumContext(...)` (rms/centroid/midFrac/highFrac/
  type Serum) + `facade_.triggerMagicMix()`. Les rôles fixes par piste et
  `setTypeOverride`/`setArrangement`/`buildSceneSnapshot` sont supprimés (le
  worker V2 capture le snapshot runtime : PCM + rôles + overrides manuels).
- Save projet → `facade_.getSlotMixState` (champ `applied`, pas `active`).
- Load projet → `facade_.setSlotMixState` (restaure + applique runtime).
- Timer `aiCloud_` → `facade_.isMagicBusy/isMagicActive/didLastMixUseFallback`.
- Spatial viz + `activeRoleForSlot`/`v2RoleForSlot` → `facade_.getDetectedType`
  (mapping `MixContentType`→`SlotRole` pour AutoMix/delay sends).
- Helper `serumContentTypeForMix()` : `engine::analysis::ContentCategory`→`MixContentType`.
- Suppression de `buildSceneSnapshot` (mort) et des appels V1 morts
  (`setSlotFilePath`/`setMusicContext`/`clearSlot`, `restoreSlotMixState`).
- Membre `samplerEngine_` retiré de `MainComponent.h`.
NON audible (re-câblage UI, aucun chemin audio changé) : NULL1 bit-exact inchangé
(`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).

### Étape 11 ✔ — réduction surface V1 (M9)
Fichiers `src/dsp/` totalement orphelins supprimés (plus aucune inclusion depuis
`src/`, `tests/` ni CMake) :
- `SmartSamplerEngine.h` (header-only, magic mix V1 — toute la logique a été
  portée dans `engine/mix/` aux étapes 1-8 et le câblage UI en étape 9).
- `WsolaShifter.h/.cpp` (le WSOLA maison est abandonné — Signalsmith Stretch
  vendored, §4.4 ; déjà découplé des cibles à l'étape 10).
NON audible (fichiers jamais compilés/référencés au runtime) : NULL1 bit-exact
inchangé (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).

À venir : étape 12 (purge finale + recette M9).

### Étape 12 — purge finale (M9) — purge complète ✔

Port et purge des orphelins du magic mix restants dans `src/dsp/`,
indépendants du pipeline audio V1 (toujours en service, câblage M8b en attente) :

- **Port FeatureExtractor** → `engine/Analysis/FeatureExtractor.h/.cpp`
  (V2, namespace `engine::analysis`, zéro dépendance JUCE/dsp). MainComponent
  (`serumMixFeatures_`, régie Serum) consomme désormais le port V2 ; l'include
  V1 retiré. `tests/test_feature_extractor.cpp` migré vers le V2 (tests déjà
  à l'identique, namespace seul).
- **Purge ciblée** des fichiers `src/dsp/` devenus orphelins (plus aucune
  inclusion depuis `src/`, `tests/` ni CMake) :
  - `FeatureExtractor.*` (V1) — remplacé par le port V2.
  - `AiMixEngine.*` (V1) — le chemin IA ONNX du magic mix ne vit plus que dans
    `engine/mix/MixAi.h` (post-traitement pur, tests MIX-17/18/19) ;
    `EngineFacade::triggerAiMagicMix()` (injecter des décisions IA) est
    conservé sans wrapper V1, l'inférence ONNX étant fournie par l'appelant.
  - `AiContentClassifier.*` (V1) — l'app n'utilise que
    `engine/Analysis/AiContentClassifier.*` (V2). `tests/test_ai_classifier.cpp`
    migré vers le V2 (API identique, namespace seul).
- **Découplage** `SlotDynamics.h` : enum `ContentCategory` désormais locale
  (n'importait `FeatureExtractor.h` que pour celle-ci) — seul garde-fou qui
  empêchait la suppression du V1.
- CMake : sources V1 retirées de `SaxFXLive` et `SaxFXTests` ;
  `MIX_MODEL_PATH/NORM` supprimés (test V1 mort) ; port V2 ajouté à `SaxFXLive`.
- `tests/test_ai_mix_engine.cpp` supprimé (moteur V1 mort, couverture portée
  dans `engine/mix/`).

### Étape 13 — M8c Vague 1-2 : purge complète src/dsp/ ✔

Après M8b (EngineFacade complet), purgé tous les fichiers V1 restants :

- **Fichiers supprimés** : Sampler, DspPipeline, StepSequencer, LooperEngine,
  SceneManager, PingPongDelay (dsp/), BpmDetector, BeatClock, LockFreeQueue,
  RingBuffer, InferenceThread, KeyDetector, KeyResult, MusicContext, DspCommon,
  MasterLimiter (dsp/), OnnxInference (dsp/), SlotDynamics, SamplerPanel,
  SamplerChannel. Total : 27 fichiers supprimés.
- **Fichiers créés** : `engine/Types.h` (StepBuf, StopMode, GridDiv),
  `engine/Analysis/OnnxInference.h` (porté depuis dsp/).
- **`engine::SceneStore`** absorbé `dsp::SceneManager` : SceneData unifié
  (SlotConfig + steps + trackBarCounts + serum + dubDelay), crossfade adaptatif,
  morphing PingPongDelay, pending scene, energy tracking — tout dans `SceneStore`.
- **MainComponent** migré : `sceneStore_` remplace `sceneManager_`, tous les accès
  aux champs plats (`filePaths[i]`, `gains[i]`, etc.) migrés vers `slots[i].field`.
- **Tests** : 7 fichiers morts supprimés (sampler, pingpong, inference_thread,
  slot_dynamics, ring_buffer, scene_manager, p0_load) ; `test_midi_mapper` nettoyé.
- **`src/dsp/`** réduit à `SerumHost.cpp/.h` (hôte VST3, dépend JUCE).

NON audible (purge + re-câblage UI, aucun chemin audio changé) : NULL1 bit-exact
inchangé (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).
Vérifications : SaxFXLive compile, **35/35** SaxFXTests, **86/86** EngineTests.

NON audible (purge de fichiers jamais référencés au runtime) : NULL1 bit-exact
inchangé (`0xa00f0dd7fb70bbd0` / `0xec5593f087458af5`).
Vérifications : SaxFXLive compile, **35/35** SaxFXTests, **86/86** EngineTests.

⛔ La purge complète de `src/dsp/` est terminée — seul `SerumHost` reste (hôte VST3, dépend JUCE).
Recette M9 (validation manuelle app + nulltest final) passée.
