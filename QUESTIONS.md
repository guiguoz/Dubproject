# QUESTIONS.md — Questions et déviations ouvertes

## Q1 — DUB throw pré-frontière (fin M6, complété en M7)

**État** : stub (boucle vide).

**Raison** : Le `SendRamp` vers `PingPongDelay` avant la frontière de transition de type
`Dub` nécessite que le `PingPongDelay` soit porté dans `engine/fx/`. Ce portage est
planifié en M7. Le `TransitionEngine` émet déjà le `SendRamp` de release à la frontière ;
le pré-throw sera complété quand le delay sera disponible.

**Action** : en M7, après le portage de `PingPongDelay`, compléter
`TransitionEngine::compilePlan()` pour le type `Dub`.

---

## Q2 — SlotRole::Unknown non défini dans SlotPlayer.h (fin M6)

**État** : `SlotConfig.role` initialisé à `SlotRole::Loop` faute de valeur `Unknown`.

**Question** : faut-il ajouter `Unknown = 9` à l'enum `SlotRole` dans `SlotPlayer.h` ?

**Suggestion** : oui, ajouter `Unknown = 255` (uint8_t) comme valeur sentinelle.
L'utilisateur valide avant que M7 ne commence.

**Decision attendue** : Guillaume

---

## Q3 — Durée de l'état Settling (fin M6)

**État** : Settling dure 1 bloc (transitoire immédiat).

**Raison** : la spec §6 mentionne SETTLING sans définir sa durée. En V2.0, la
convergence du mix auto en 150–300 ms (§11.5) remplace un vrai état SETTLING
dans le TransitionEngine. Implémenté comme 1 bloc pour satisfaire les tests.

**Decision** : si une durée spécifique est souhaitée (ex. 300 ms = durée du send DUB),
indiquer la valeur. Sinon, la version à 1 bloc est suffisante.

**Decision attendue** : Guillaume
