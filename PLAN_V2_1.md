# DubEngine V2.1 — Plan pour Claude Code

**Prérequis absolu :** V2.0 recettée (jalon M9 de PLAN_V2_ENGINE.md validé par
l'utilisateur, null-test en place). Ne rien commencer avant.
**Hérité de PLAN_V2_ENGINE.md, applicable ici :** hiérarchie normative
(§11.1), contraintes RT (§1), null-test à chaque commit (§11.2), journal
d'événements, points d'arrêt QUESTIONS.md, instrumentation CPU (§11.4).
Ordre strict N1 → N6, un commit par item avec ses tests.

---

# N1 — Save/Load versionné (fondation de tout le reste)

## N1.1 Format projet
- Un projet = un DOSSIER : `MonProjet.dubproj/` contenant `project.json`
  (lisible, indenté) + `samples/` (chemins relatifs).
- `project.json` : champ `version` (entier, commence à 1) en tête ; puis BPM,
  tonalité projet, scènes (patterns, slots : chemin relatif, mode
  ONE-SHOT/FREE/LOOP SYNC, loopBeats, bpm confirmé, transpose, key-match
  on/off, trim, gains utilisateur), état des toggles globaux, mappings MIDI.
- **Tout état qui influence le rendu audio est dans le fichier** — critère
  vérifiable : charger un projet et le re-rendre offline (§11.2 V2.0) donne
  un rendu bit-identique à celui d'avant la sauvegarde (test T-N1a, le plus
  important de ce plan).
- Migrations : `loadProject` route par version vers des fonctions
  `migrate_vN_to_vN+1` chaînées ; un projet d'une version future → message
  clair, pas de crash. Toute évolution du format = version +1 + migration +
  fixture de test du vieux format committée.

## N1.2 Collect & save
« Enregistrer (tout collecter) » : copie chaque sample référencé dans
`samples/` (déduplication par hash de contenu), réécrit les chemins en
relatif. Fichier source manquant au chargement : slot marqué offline (badge
UI), le projet s'ouvre quand même, jamais de crash ni de dialogue bloquant en
série.

## N1.3 Autosave + récupération
- Snapshot automatique toutes les 2 min (et avant chaque transition de scène
  armée) vers `MonProjet.dubproj/autosave/` — 5 fichiers tournants,
  horodatés. Écriture sur un thread de fond, JAMAIS le thread audio ni un
  blocage du message thread > 50 ms (sérialiser vers un buffer mémoire sur le
  message thread, écrire le disque en fond).
- Au démarrage, si un autosave est plus récent que la dernière sauvegarde
  manuelle : proposer la restauration (choix : restaurer / ouvrir la
  sauvegarde / ignorer).

**Tests :** T-N1a (round-trip rendu bit-identique) ; T-N1b (migration v1→v2
sur fixture) ; T-N1c (sample manquant → projet ouvert, slot offline) ;
T-N1d (autosave n'ajoute aucun événement au journal du thread audio).

# N2 — Undo/Redo

- **Command pattern sur l'EngineFacade** : chaque mutation d'état passe par
  un objet commande {apply, revert, description, fusionnable}. AUCUNE
  mutation ne contourne la façade (vérifier : c'est déjà l'architecture V2).
- Périmètre undoable : édition de patterns, chargement/suppression de sample
  dans un slot, changements de mode/transpose/loopBeats/trim, édition de
  scènes, BPM/tonalité projet, mappings MIDI. NON-undoable (geste live,
  volontairement) : triggers de pads, mutes live, navigation de scène,
  contrôles de performance — sinon l'historique devient inutilisable.
- Fusion : les mouvements continus (drag d'un même paramètre) fusionnent en
  une seule entrée.
- Pile bornée (200 entrées), Ctrl+Z / Ctrl+Shift+Z, affichage de la
  description (« Annuler : pattern piste 3 »).
- Interaction moteur : apply/revert émettent les mêmes intentions quantisées
  que l'UI (§11.6) — l'undo d'un changement audio prend effet proprement,
  jamais de mutation directe d'état audio.

**Tests :** T-N2a : 50 mutations aléatoires, 50 undo → état sérialisé
identique à l'initial (comparaison JSON). T-N2b : redo complet → identique à
l'état final. T-N2c : drag fusionné = une entrée.

# N3 — Séquenceur expressif : probabilité, ratchet, vélocité

- Par step, trois attributs : `velocity` (0–127, défaut 100 → module le gain
  du trigger, courbe exponentielle douce), `probability` (0–100 %, défaut
  100), `ratchet` (1/2/3/4 sous-hits également répartis dans la durée du
  step, vélocité décroissante de 15 % par sous-hit).
- Implémentation : uniquement dans la GÉNÉRATION d'événements du Sequencer
  (le reste du moteur ne change pas — un ratchet = 4 événements Trigger
  sample-accurate).
- **Déterminisme préservé** (invariant §11.2) : la probabilité utilise un RNG
  seedé par (seed projet, index de step absolu, piste) — deux rendus offline
  du même projet restent bit-identiques ; le seed projet est dans
  project.json (N1) et re-tirable par l'utilisateur.
- UI : clic droit / long press sur un step → mini-popover (vélocité en drag
  vertical, prob, ratchet). Affichage : opacité du step = vélocité, coin
  marqué si prob < 100 ou ratchet > 1.
- Sauvegarde : version du format +1, migration (steps existants → défauts).

**Tests :** T-N3a : ratchet 4 → 4 triggers aux offsets exacts step/4.
T-N3b : prob 50 % sur 10 000 steps → 50 % ±2, et séquence identique entre
deux runs même seed. T-N3c : vélocité 64 → gain attendu ±0,1 dB.

# N4 — Spring reverb + intégration dub

- Nouveau `engine/fx/SpringReverb` : modèle algorithmique léger — réseau de
  délais dispersifs en série (chirp caractéristique du ressort : cascade
  d'allpass accordés) + boucle feedback filtrée + « boing » excitable.
  Paramètres : decay, tension (fréquence du chirp), mix, damping. Qualité
  cible : caractère ressort reconnaissable, pas une émulation physique
  parfaite. Mono-in/stéréo-out. Budget : < 3 % d'un cœur.
- Intégration : bus send parallèle au PingPongDelay ; l'AutoMix v1 pose des
  sends statiques par rôle (SNARE 0,3 ; PERC 0,2 ; MELODIC 0,15 ; autres 0)
  ; le **ducking des sends** (le send n'est audible que dans les creux du
  dry : sidechain du retour par le bus sec, attack 5 ms / release 200 ms,
  −6 dB max) s'applique aux DEUX sends (reverb + delay) — thread audio.
- Le TransitionEngine peut cibler la reverb dans les DUB throws (SendRamp
  vers reverb en plus du delay — variante du type DUB, choisie par rôle :
  percs → reverb, mélodique → delay).

**Tests :** T-N4a : impulsion → réponse > 1,5 s à decay max, énergie du chirp
mesurable. T-N4b : ducking suit l'enveloppe du dry à l'échantillon près.
T-N4c : null-test V2.0 inchangé quand la reverb est à mix 0.

# N5 — Filtre master performable

- Un paramètre bipolaire unique [−1, +1], défaut 0 = bypass strict
  (null-test inchangé à 0) : négatif → LP 24 dB/oct descend de 20 kHz vers
  200 Hz (log) ; positif → HP 24 dB/oct monte de 20 Hz vers 2 kHz ; légère
  résonance fixe (Q 1,2) pour le caractère.
- Position dans le graphe : après le bus mix + retours d'effets, AVANT la
  saturation… (pas de saturation en V2.1 — avant le MasterLimiter).
- Lissage du paramètre côté audio (rampe 20 ms) ; mappable MIDI CC (infra
  MIDI Learn existante) ; gros contrôle dédié dans l'UI (le mode performance
  complet reste hors périmètre V2.1).
- Non-undoable, non sauvegardé armé (toujours à 0 à l'ouverture d'un projet
  — c'est un geste, pas un réglage).

**Tests :** T-N5a : paramètre 0 → bit-transparent. T-N5b : balayage complet
en 100 ms → aucun zipper (dérivée de sortie bornée), fréquence de coupure
suit la loi log ±5 %.

# N6 — Reports de V2.0 (spécifiés dans les plans d'origine, adaptés au moteur V2)

Dans cet ordre, chacun optionnel et validé séparément avec l'utilisateur
avant de commencer (point d'arrêt QUESTIONS.md) :
1. **AutoMix v2** : anti-masquage dynamique (règle 2 de §7.2 V2.0, test
   T-MX1), panorama auto par rôle (règle 5), pré-amorçage des cibles aux
   transitions (§7.3).
2. **Looper** : port sur le moteur V2 — position dérivée du transport,
   lecture fractionnaire Hermite, BPM d'enregistrement stocké, overdub dans
   le référentiel du buffer, resync au downbeat. L'UI looper est réactivée.
3. **Effets de performance** : tape stop/start, pitch glide (presets Siren/
   Riser/Dive), reverse par slot, half/double-time — via le facteur de rate
   du SlotPlayer (mécanique déjà présente dans le moteur V2 pour le chemin
   bypass ; à travers le stretcher, le perf FX force temporairement le
   bypass avec crossfade — plus simple et l'artefact de pitch est l'effet
   recherché).

# Ordre, jalons, règles

N1 → N2 → N3 → N4 → N5 → N6 (chaque sous-item de N6 séparément).
Chaque item : tests verts + null-test global + point d'écoute utilisateur
avant le suivant. Le format projet ne bouge qu'avec version+1 et migration
testée. Aucune modification du cœur V2 (Transport, position dérivée,
transparence) n'est autorisée par ce plan ; si un item semble l'exiger,
STOP + question dans QUESTIONS.md.
