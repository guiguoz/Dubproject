# Tentatives échouées : Quantization Unmute

**Date** : 21 juin 2026  
**Objectif** : Quand un sample est démuté dans certaines scènes, il doit attendre le prochain step 0 avant de commencer la lecture, au lieu de démarrer immédiatement.

---

## Contexte

### Comportement actuel (avant modifications)
- Unmute d'un slot → le sample démarre **immédiatement** au prochain step actif
- Pas de synchronisation avec le début de la boucle (step 0)

### Comportement souhaité
- Unmute d'un slot → bloquer les triggers jusqu'au **prochain step 0**
- Le sample ne démarre qu'au début de la prochaine boucle du pattern

### Contraintes
- **Thread-safe** : `setSlotMuted()` appelé depuis l'UI (message thread), `trigger()` appelé depuis l'audio thread
- **Pas de régression** : l'éditeur de sample doit continuer de fonctionner
- **Architecture existante** : 
  - `StepSequencer::process()` appelle `sampler.trigger(track)` pour chaque step actif
  - Chaque track loop indépendamment avec son propre `trackStepCount`
  - `globalStep % trackSteps` détermine le step courant dans la boucle de la track

---

## Tentative 1 : Flag `blockTriggers` atomique simple

### Implémentation
```cpp
// Sampler.h - PlayState
std::atomic<bool> blockTriggers { false };

// Sampler.cpp - setSlotMuted()
if (wasMuted && !muted)
    ps.blockTriggers.store(true, std::memory_order_release);

// Sampler.cpp - trigger()
if (ps.blockTriggers.load(std::memory_order_acquire))
    return;

// Sampler.cpp - clearTriggerBlock()
void Sampler::clearTriggerBlock(int slot) {
    playStates_[slot].blockTriggers.store(false, std::memory_order_release);
}

// StepSequencer.h - process()
if (trackStep == 0)
    sampler.clearTriggerBlock(track);
if (active.steps[track][trackStep])
    sampler.trigger(track);
```

### Problème identifié
**Le flag est clearé à chaque step 0, même si aucun unmute n'a eu lieu.**

Exemple de scénario problématique :
1. Step 0 passe → `clearTriggerBlock()` met `blockTriggers = false`
2. Step 1, 2, 3... passent normalement
3. **Unmute au step 8** → `blockTriggers = true`
4. Step 9, 10... → triggers bloqués ✓
5. **Step 0 suivant** → `clearTriggerBlock()` devrait libérer
6. **MAIS** : si l'unmute arrive **après** que le step 0 ait déjà clearé le flag dans le cycle précédent, le sample démarre immédiatement

**Résultat** : ❌ Sample démarre immédiatement après unmute + éditeur cassé

---

## Tentative 2 : Flag `unmuteStep` avec valeur sentinelle

### Implémentation
```cpp
// Sampler.h - PlayState
std::atomic<int> unmuteStep { -1 };  // -1 = en attente, -2 = clearé

// Sampler.cpp - setSlotMuted()
if (wasMuted && !muted)
    ps.unmuteStep.store(-1, std::memory_order_release);

// Sampler.cpp - trigger()
if (ps.unmuteStep.load(std::memory_order_acquire) == -1)
    return;

// Sampler.cpp - clearTriggerBlock()
const int step = ps.unmuteStep.load(std::memory_order_acquire);
if (step == -1)
    ps.unmuteStep.store(-2, std::memory_order_release);
```

### Problème identifié
**Une fois clearé à `-2`, le flag reste à `-2` pour toujours.**

Scénario :
1. Unmute → `unmuteStep = -1` (bloque) ✓
2. Step 0 → `clearTriggerBlock()` met `-1 → -2` (libère) ✓
3. **Mute → Unmute suivant** → `unmuteStep = -1` mais...
4. Le check `unmuteStep == -1` bloque, mais `clearTriggerBlock()` ne voit que `-2`, donc ne fait rien
5. Ou pire : le flag reste à `-2` et ne bloque jamais

**Résultat** : ❌ Sample démarre immédiatement après unmute + éditeur cassé

---

## Tentative 3 : Ajout reset sur mute + valeur inactive `-99`

### Implémentation
```cpp
// Sampler.cpp - setSlotMuted()
if (wasMuted && !muted)
    ps.unmuteStep.store(-1, std::memory_order_release);
else if (!wasMuted && muted)
    ps.unmuteStep.store(-99, std::memory_order_release);  // reset

// Sampler.cpp - clearTriggerBlock()
if (step == -1)
    ps.unmuteStep.store(-99, std::memory_order_release);  // inactive
```

### Problème identifié
**Le flag ne gère toujours pas correctement les cycles multiples.**

Scénario :
1. Unmute au **step 8** (milieu de boucle) → `unmuteStep = -1`
2. Steps 9-15 → triggers bloqués ✓
3. **Step 0** → `clearTriggerBlock()` met `-1 → -99`
4. **Trigger step 0** appelé immédiatement après dans la même itération
5. Check : `unmuteStep == -1` ? Non (`-99`), donc le trigger **passe**

**Mais** : Si le séquenceur est au step 0 **au moment de l'unmute** :
- `unmuteStep = -1` (bloque)
- Le step 0 actuel a **déjà été traité** dans la boucle `while (phase_ > nextFirePhase_)`
- Les steps 1, 2, 3... se déclenchent **immédiatement** après l'unmute
- Le prochain step 0 n'arrivera que dans **une boucle complète**

**Résultat** : ❌ Sample démarre immédiatement après unmute + éditeur cassé

---

## Tentative 4 : Ordre inversé (clear **avant** check step)

### Modification dans StepSequencer.h
```cpp
// Clear trigger block at step 0 BEFORE checking the step state
if (trackStep == 0)
    sampler.clearTriggerBlock(track);

if (active.steps[track][trackStep])
    sampler.trigger(track);
```

**Note** : Ce code était déjà dans cet ordre dans la tentative 1, donc cette "tentative 4" était en fait identique.

### Problème identifié
**Même problème qu'avant** : le flag est clearé **avant** que le trigger soit vérifié, donc si l'unmute arrive entre deux steps, le blocage ne tient qu'un cycle.

**Résultat** : ❌ Sample démarre immédiatement après unmute + éditeur cassé

---

## Analyse des échecs

### Problème fondamental
Le système ne peut pas savoir si `blockTriggers` ou `unmuteStep` doit être clearé **maintenant** ou **pas**, car on ne sait pas si l'unmute a eu lieu **avant** ou **après** le dernier step 0.

### Race condition identifiée
```
Thread UI (unmute)         Thread Audio (step 0)
─────────────────────────────────────────────────
unmute clicked
  blockTriggers = true
                           process() loop
                           step 0 detected
                           clearTriggerBlock()
                             blockTriggers = false
                           trigger(0) → PASSES ✗
```

### Pourquoi l'éditeur casse ?
Hypothèse non confirmée : l'éditeur de sample (`SampleEditorComponent`) appelle probablement `setSlotMuted()` quelque part (pour isoler le slot en cours d'édition ?), ce qui déclenche le flag de manière inattendue.

Autre hypothèse : `reloadSlotData()` ou `loadSample()` pendant que le flag est armé → comportement indéterminé.

---

## Ce qui n'a PAS été tenté

### Option A : Tracker le step au moment de l'unmute
Stocker le `globalStep` **exact** au moment de l'unmute, et ne clear le flag que si on atteint **le prochain step 0 après ce globalStep**.

Problème : `globalStep` n'est pas accessible depuis `setSlotMuted()` (thread UI).

### Option B : Queue de commandes unmute
Au lieu de set un flag atomique, envoyer une commande "unmute at next step 0" dans une queue lock-free, consommée par le thread audio.

Complexité accrue, risque de bugs.

### Option C : Mode "quantize unmute" global
Ajouter un mode optionnel "quantize unmute" (bouton UI), qui change le comportement de `setSlotMuted()`.

Ne répond pas au besoin : l'utilisateur veut que **tous** les unmutes soient quantifiés.

### Option D : Modifier StepSequencer pour ne jamais appeler trigger() sur un slot muté
Au lieu de bloquer dans `Sampler::trigger()`, ne jamais appeler `trigger()` si le slot est muté.

Problème : le step 0 doit quand même être détecté pour libérer le blocage, donc on revient au même problème.

### Option E : Double-flag "unmute armed" + "unmute cleared"
```cpp
std::atomic<bool> unmuteArmed { false };
std::atomic<bool> unmuteClearPending { false };
```

Scénario :
1. Unmute → `unmuteArmed = true`
2. Step 0 → `unmuteClearPending = true`
3. Trigger check : si `unmuteArmed && !unmuteClearPending` → block
4. Après trigger : `unmuteArmed = false`, `unmuteClearPending = false`

**Jamais implémenté** car trop de states à gérer, risque de deadlock.

---

## Leçons apprises

1. **Les flags atomiques simples ne suffisent pas** quand les événements (unmute, step 0, trigger) arrivent de manière asynchrone depuis différents threads.

2. **Le timing de l'unmute est critique** : selon où on est dans la boucle au moment de l'unmute, le comportement diffère.

3. **Le step 0 est appelé à chaque cycle** → tout mécanisme de "clear" sera déclenché **même si aucun unmute n'a eu lieu**.

4. **L'éditeur de sample est fragile** → toute modification de l'état de lecture (`playing`, `muted`, `loaded`) peut casser son fonctionnement.

5. **Les tests unitaires auraient pu aider** → un test simulant unmute au step 8, puis vérifier que le premier trigger arrive au step 0 suivant.

---

## Conclusion

Après 4 tentatives et ~2h de debug, **aucune solution fonctionnelle n'a été trouvée**.

Le problème est **architectural** : la synchronisation entre l'UI thread (unmute) et l'audio thread (step sequencer) nécessiterait une refonte plus profonde du système de triggers.

**Recommandation** : Si cette fonctionnalité est critique, envisager :
- Un système de commandes différées (queue lock-free)
- Un refactor du StepSequencer pour gérer les "pending unmutes" en interne
- Un mode "live" vs "studio" où le comportement de mute/unmute change selon le contexte

**Status** : ❌ **ÉCHEC - Fonctionnalité abandonnée**

---

## Code original restauré

Tous les fichiers ont été revertés via :
```bash
git checkout HEAD -- src/dsp/Sampler.h src/dsp/Sampler.cpp src/dsp/StepSequencer.h
cmake --build build --config Release --parallel 4
```

Le comportement est revenu à l'état initial : unmute → trigger immédiat au prochain step actif.
