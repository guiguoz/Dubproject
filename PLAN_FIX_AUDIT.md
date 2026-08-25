# DubEngine V2 — Plan de correction de l'audit + tests

Destiné à l'IA qui code. Contexte : le moteur V2 tourne (transport, séquenceur,
transparence à l'import validés à l'oreille). Un audit a relevé ~30 bugs. Ce plan
les corrige en 4 vagues, CHAQUE correction accompagnée de son test. Règle absolue :
**aucun fix n'est considéré fait sans le test qui prouve qu'il ne peut pas revenir.**

## Règles de travail (à respecter pour tout ce document)
1. Un commit par bug (ou par groupe homogène explicitement indiqué), contenant le
   fix ET son test, verts.
2. Ne jamais passer à la vague suivante si les tests de la vague courante échouent.
3. Deux invariants à faire respecter partout, car la majorité des bugs en découlent :
   - **Tout état lu et écrit par deux threads (audio ↔ message/mix) est explicitement
     thread-safe** : `std::atomic` pour les scalaires, double-buffer à flip atomique
     pour les structures. Aucun accès partagé « nu ».
   - **Toute entrée externe (fichier projet, MIDI, taille de bloc) est bornée et
     validée AVANT usage.** Jamais d'indexation par une valeur venue de l'extérieur
     sans clamp/rejet préalable.
4. Après chaque vague, le null-test de régression (T0 ci-dessous) doit passer. S'il
   change, c'est soit une régression (à corriger), soit un changement de rendu
   INTENTIONNEL (régénérer la référence dans le même commit, avec justification
   écrite).

---

# VAGUE 0 — Mettre en place le filet de test AVANT de corriger quoi que ce soit

Sans ce filet, corriger 30 bugs va en réveiller d'autres en silence. On l'installe
en premier.

## T0 — Null-test de régression (le filet principal)
**But** : prouver qu'aucune correction ne change le rendu audio de façon inattendue.
**À faire** :
- Vérifier si `renderOffline(sessionState, numSamples)` existe déjà (prévu §11.2 du
  plan V2). S'il n'existe pas, l'implémenter : rendu déterministe complet (séquenceur,
  slots, mix) sans carte son, dans un buffer.
- Créer une fixture de session de référence committée dans le repo :
  `tests/fixtures/regression_session.json` + les samples de test nécessaires
  (`tests/fixtures/samples/`, des WAV courts générés ou libres de droits).
  La session doit exercer : 2-3 slots avec des rôles différents (un kick OneShot,
  une loop, un pad), un pattern non trivial, au moins un changement d'état en cours
  de route.
- Outil `tests/nulltest` : rend cette session sur N secondes, compare au rendu de
  référence stocké (`tests/fixtures/regression_reference.raw` ou hash + RMS par
  fenêtre). Tolérance : identité stricte (ou < -120 dBFS d'écart si le float
  introduit du bruit non déterministe — à documenter).
- Intégrer au CTest : `nulltest` doit tourner à chaque `ctest`.
**Critère** : le null-test passe sur le code actuel (établit la référence). À partir
de là, il DOIT passer après chaque vague.
**Prérequis de déterminisme** : aucune source de temps réel ni d'aléa non seedé dans
le chemin de rendu. Si un RNG existe (probabilité de step p.ex.), il est seedé par
le projet.

## T-TSAN — ThreadSanitizer pour toute la catégorie data race
**But** : détecter TOUTES les data races d'un coup, au lieu d'écrire un test illusoire
par race (une race ne se déclenche pas de façon déterministe, un test manuel ne prouve
rien).
**À faire** :
- Ajouter une configuration de build `-fsanitize=thread` (Clang ou GCC ; sous Windows,
  prévoir une cible WSL/Linux ou Clang, car MSVC ne supporte pas TSan). Documenter la
  commande dans le README.
- Créer un scénario de test multi-thread `tests/tsan_scenario` qui reproduit l'usage
  réel : thread audio qui tourne (`processBlock` en boucle), pendant que le message
  thread fait des `setBpm`, `setStep`, changements de scène, imports, et que le mix
  thread tourne. Laisser tourner quelques secondes sous TSan.
**Critère** : zéro rapport TSan. C'est ce test qui valide les corrections H1–H8 et M7
en bloc — pas des tests unitaires individuels.

---

# VAGUE 1 — Ce qui bloque ou fausse les tests immédiats

## PRÉALABLE — Lever l'incohérence sur C2
Avant de corriger C2, EXPLIQUER comment l'app a pu démarrer et jouer un kick alors
que l'audit décrit C2 comme un crash startup garanti (78 accès `stepSeqPanel_->` avant
`make_unique`). Deux possibilités : soit le chemin d'accès n'est pas toujours pris
(alors C2 est conditionnel, pas garanti), soit l'audit décrit un état du code différent
de celui qui tourne. Confirmer sur le code réel avant de coder le fix. Répondre dans
QUESTIONS.md.

## C2 — stepSeqPanel_ accès avant initialisation
**Fichier** : MainComponent.cpp:113-513 (accès), ~648 (make_unique).
**Fix** : garantir que `stepSeqPanel_` est construit AVANT tout accès. Soit déplacer
le `make_unique` avant le premier usage, soit garder tous les accès derrière une garde
`if (stepSeqPanel_)`. Préférer la première (ordre d'initialisation correct) à la
seconde (garde partout = fragile).
**Test T-C2 (fumée)** : instancier MainComponent (ou le sous-système concerné),
`prepare()`, quelques `processBlock()`, détruire — sans crash. Si MainComponent est
trop couplé à l'UI pour être testé seul, tester au moins le chemin d'init du moteur.

## C3 — Events traités sans dispatch temporel (fausse le timing)
**Fichiers** : EngineFacade.cpp:117-131, SlotPlayer.cpp:469-519, EventScheduler.
**Problème** : tous les events sont appliqués au sample 0 du bloc au lieu d'être
dispatchés à leur offset intra-bloc. Le `EventScheduler::processBlock()` template fait
le vrai dispatch mais n'est pas utilisé. Les GainRamp d'un Replace s'écrasent dans le
même bloc. Conséquence directe : jitter sur les triggers → le Test 2 de verrouillage
serait faussé.
**Fix** : router le traitement des events par `EventScheduler::processBlock()` avec
split-processing (découper le bloc aux timestamps des events, rendre les sous-segments
entre chaque event). Chaque event s'applique à son offset exact.
**Test T-C3 (unitaire, critique)** : poser 2 events sur le même slot à des offsets
différents dans un même bloc (ex. trigger à l'offset 0 et à l'offset 200 d'un bloc de
512). Vérifier que le rendu produit bien deux déclenchements aux positions 0 et 200,
pas deux au sample 0. Variante : un GainRamp à l'offset 100 ne doit pas être écrasé par
un autre au même bloc.
**Note** : ce test protège le Test 2 de verrouillage à venir.

**Après vague 1** : recompiler, vérifier que le kick joue toujours (test manuel), puis
le null-test T0. C'est le moment de faire enfin le Test 2 de verrouillage à l'oreille
sur un timing propre.

---

# VAGUE 2 — Data races (un seul principe, tous les sites)

Traiter H1–H8 + M7 comme UNE tâche guidée par l'invariant n°3.1. Ne pas écrire un test
unitaire par race : la validation de toute la vague est T-TSAN (vague 0).

## Scalaires → std::atomic
- **H2 kickSlot_** (AudioGraph) : `std::atomic<int>`.
- **H3 swingFactor_** (EngineFacade.h:253) : `std::atomic<float>`.
- **H4 currentScene_** (EngineFacade.h:254) : `std::atomic<int>`.
- **M7 slotRoleAnalyzed_[]** (EngineFacade.cpp:280,620) : `std::atomic` par entrée,
  ou double-buffer si lu en masse.

## Structures trop grosses pour un atomic → double-buffer à flip atomique
- **H1 Transport::state_** (Transport.h:93-101, struct 48+ bytes) : le thread audio
  écrit dans un buffer inactif, publie par un `std::atomic<index>` ; le message thread
  lit via l'index publié. Même pattern que le double-buffer de patterns déjà en place.
- **H7 AutoMixDub targets_** (AudioGraph.cpp:96-126) : le mix thread écrit les cibles
  dans un buffer inactif, flip atomique ; l'audio thread lit le buffer publié. Aucun
  accès simultané au même buffer.

## Cycle de vie des threads
- **H6 Import concurrent** (EngineFacade.cpp:234-328) : un mutex ou un flag atomique
  « import en cours » PAR SLOT ; un second import sur le même slot attend ou est
  rejeté. Le PCM n'est jamais écrit pendant qu'une voix le lit (respecter le
  double-buffer PCM prévu).
- **H8 use-after-free (lambdas callAsync)** (EngineFacade.cpp:328,485,516) : les
  lambdas qui capturent `this` doivent utiliser une garde de vie
  (`juce::WeakReference` sur le composant, ou `SafePointer`, ou vérifier l'existence
  avant usage). Une callAsync qui s'exécute après destruction ne doit pas déréférencer
  un objet mort.

**Validation vague 2** : T-TSAN vert (zéro race rapportée) + null-test T0 inchangé.

---

# VAGUE 3 — Comportement DSP + robustesse

## C1 — MonoSubFilter : HP au lieu de LP
**Fichier** : AudioGraph.h:20-44.
**Fix** : la formule actuelle `sL = L[i] + a1*sL` avec a1<0 est un passe-haut.
Implémenter un vrai passe-bas 1 pôle : `sL = (1-a)*L[i] + a*sL` avec `a = exp(-2π*fc/sr)`.
Vérifier le sens sur les deux canaux.
**Test T-C1 (unitaire)** : injecter une sinusoïde grave (ex. 80 Hz) et une aiguë
(ex. 8 kHz) à travers le filtre réglé en LP. Mesurer l'énergie de sortie : le grave
doit passer (atténuation faible), l'aigu doit être fortement atténué (> X dB).
Un HP ferait l'inverse — le test échouerait sur le code actuel, confirmant le bug.

## M4 — MonoSubFilter state non reset au changement SR
**Fichier** : AudioGraph.cpp:24-28.
**Fix** : remettre l'état interne (sL, sR) à 0 dans `prepare()` / au changement de SR,
et recalculer `a` avec le nouveau SR.
**Test** : couvert par T-C1 exécuté à deux SR différents (44100 et 48000) → réponse
cohérente aux deux.

## H5 / M5 — slotIndex MIDI et projet non bornés
**Fichiers** : ProjectLoader.cpp:83, MidiManager.cpp:126.
**Fix** : borner `slotIndex` à `[0, kNumSlots)` avant tout usage ; rejeter (log +
ignore) une valeur hors bornes. Invariant n°3.2.
**Test T-H5 (robustesse)** : charger un projet dont un slotIndex vaut 999 et un autre
-1 ; injecter un message MIDI avec slotIndex hors bornes → aucun crash, valeurs
rejetées proprement.

## M9 — SceneStore::setSceneEnergy sans bounds check
**Fichier** : SceneStore.h:174-179.
**Fix** : clamp/rejet si `idx` hors `[0, kNumScenes)`.
**Test** : appeler avec idx négatif et idx trop grand → pas de crash.

## M1 — memset OOB dans le path d'erreur numFrames > maxBlock_
**Fichier** : AudioGraph.cpp:62-65.
**Fix** : clamp `numFrames` à `maxBlock_` (ou early-return propre) avant le memset ;
ne jamais écrire au-delà du buffer alloué.
**Test T-M1** : appeler processBlock avec numFrames > maxBlock_ → pas d'OOB (valider
sous AddressSanitizer si dispo).

## M6 — evBuf[256] (6 Ko) sur la pile du callback audio
**Fichier** : EngineFacade.cpp:117.
**Fix** : sortir ce buffer de la pile du callback (membre pré-alloué au `prepare()`).
Cohérent avec « zéro allocation ET pas de gros objets pile dans le callback ».
**Test** : couvert indirectement ; vérifier qu'aucune allocation/gros objet pile ne
subsiste dans le callback (revue + éventuellement un check de taille de frame de pile).

---

# VAGUE 4 — Raffinements (après stabilité des vagues 1-3)

Chacun avec un test si applicable ; sinon revue + null-test.
- **M2** gain staging divise toujours par 9 au lieu du nombre de slots actifs
  (AudioGraph.cpp:114-119). Test : mix à 2 slots actifs → niveau attendu, pas divisé
  par 9.
- **M3** PingPongDelay freeze mute la sortie au lieu de soutenir (PingPongDelay.cpp:43-45).
  Test : en freeze, la sortie maintient la queue, ne tombe pas à 0.
- **M8** protection denormals (FTZ/DAZ) dans le callback audio. Test : injecter des
  valeurs qui décroissent vers le denormal → pas de chute de perf (mesure de charge).
- **MasterLimiter** = soft-clipper sans attack/release : décider si on garde tel quel
  (documenter la limitation) ou on implémente une vraie enveloppe. À trancher avec
  l'utilisateur (QUESTIONS.md).

## Liste « info » → pile M8c / nettoyage (pas des bugs)
Ne PAS traiter ici, noter dans la pile de nettoyage :
- SceneStore morphing dupliqué avec EngineFacade (code mort).
- Transition Dub = stub jamais déclenché (à câbler quand les transitions seront
  finalisées).
- swing non écrit quand == 0 (vérifier que la valeur 0 est bien persistée).
- Fichiers multi-canaux tronqués silencieusement à 2 canaux (au minimum : logger un
  avertissement à l'import).
- OfflineRender ne simule pas le morphing delay (à aligner pour que le null-test
  couvre aussi le delay).

---

# Récapitulatif de l'ordre d'exécution
1. **Vague 0** : T0 (null-test) + T-TSAN en place. Le filet AVANT tout.
2. **Vague 1** : C2 (après avoir levé l'incohérence) + C3. → recompiler, kick OK,
   null-test, puis Test 2 de verrouillage à l'oreille.
3. **Vague 2** : toutes les data races en une passe (invariant thread-safe). Validé
   par T-TSAN + null-test.
4. **Vague 3** : C1/M4, H5/M5, M9, M1, M6. Chacun son test.
5. **Vague 4** : raffinements + tri de la liste info.

# Principe à retenir (à graver dans les notes du projet)
- Tout état partagé entre threads = atomic ou double-buffer. Jamais d'accès nu.
- Toute entrée externe = bornée avant usage.
- Chaque bug corrigé arrive avec son test. Pas de fix sans test.
- Le null-test doit rester vert après chaque vague ; un changement de rendu est soit
  une régression, soit intentionnel et documenté.
