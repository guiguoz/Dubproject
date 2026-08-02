# QUESTIONS.md — Questions et déviations ouvertes

Toutes les questions M6 sont closes. Aucune question ouverte.

---

## [CLOSE] Q1 — DUB throw pré-frontière

**Décision** : stub M6 validé. En M7 : SendRamp AVANT le mute du slot sortant
(délai se remplit pendant que le son est encore là). Test T-TX DUB à écrire :
vérifier qu'il reste de l'énergie dans le delay après le mute.

## [CLOSE] Q2 — SlotRole::Unknown

**Décision** : `Unknown = 255` (uint8_t). AutoMix traite Unknown : gain 0 dB,
aucun send, pas de sidechain. Appliqué dans SlotPlayer.h.

## [CLOSE] Q3 — Durée de l'état Settling

**Décision** : IDLE à 1 bloc (permet d'enchaîner les transitions en live).
Libération par slot : conditionnée à "fade terminé ET plus aucune voix active",
vérifiée indépendamment par chaque slot — jamais par l'état de transition.
