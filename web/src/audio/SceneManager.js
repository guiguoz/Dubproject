/**
 * SceneManager — gestion des scènes et transitions automatiques.
 *
 * Modèle de données aligné sur ProjectData.h (C++/JUCE) :
 *   - 8 scènes max, 8 tracks × 16 steps, BPM par scène.
 *
 * Callbacks UI (assignables depuis l'extérieur) :
 *   onSceneChange({ newIndex, oldIndex })
 *   onPendingChange({ pendingIndex })          // null = annulé
 *   onStepUpdate({ currentStep, totalSteps })
 */
export class SceneManager {
  constructor(audioContext) {
    this.audioContext = audioContext;

    /** @type {Scene[]} */
    this.scenes = [];
    this.currentSceneIndex = 0;
    this.pendingSceneIndex = null;
    this.isPlaying = false;
    this.currentStep = 0;

    // Callbacks UI — remplacer depuis l'extérieur
    this.onSceneChange  = null;
    this.onPendingChange = null;
    this.onStepUpdate   = null;

    // Nœuds audio actifs : Map<AudioBufferSourceNode, true>
    this._activeSources = new Map();
  }

  /**
   * Charge les scènes depuis un tableau de données brutes.
   * Compatible avec le format SceneSaveData du projet C++.
   *
   * @param {SceneData[]} scenesData
   */
  loadScenes(scenesData) {
    this.scenes = scenesData.map((d, i) => ({
      id:          i,
      name:        d.name        ?? `Scène ${i + 1}`,
      tracks:      d.tracks      ?? [],
      pattern:     d.pattern     ?? Array.from({ length: 8 }, () => new Array(16).fill(0)),
      lengthInBars: d.lengthInBars ?? 1,
      stepsPerBar:  d.stepsPerBar  ?? 16,
      get totalSteps() { return this.lengthInBars * this.stepsPerBar; },
    }));
  }

  /**
   * Demande un changement de scène.
   *   - Transport arrêté  → immédiat
   *   - Scène déjà active → annule le pending
   *   - Sinon            → met en attente jusqu'à la fin de la scène courante
   *
   * @param {number} sceneIndex
   */
  requestSceneChange(sceneIndex) {
    if (sceneIndex === this.currentSceneIndex) {
      // Re-clic sur la scène active : annule l'attente
      this.pendingSceneIndex = null;
      this.onPendingChange?.({ pendingIndex: null });
      return;
    }

    if (!this.isPlaying) {
      this._switchToScene(sceneIndex);
      return;
    }

    this.pendingSceneIndex = sceneIndex;
    this.onPendingChange?.({ pendingIndex: sceneIndex });
  }

  /**
   * Appelé à chaque step par le Scheduler (audio thread simulé via setTimeout).
   * Déclenche les samples, incrémente le step, gère la transition.
   *
   * @param {number} stepTime  — audioContext.currentTime du step à déclencher
   */
  /**
   * @param {number} stepTime      — audioContext.currentTime du step courant
   * @param {number} nextStepTime  — calculé par le Scheduler, source unique du BPM
   */
  onTick(stepTime, nextStepTime) {
    if (!this.isPlaying) return;

    const scene = this.scenes[this.currentSceneIndex];
    if (!scene) return;

    this._triggerStep(scene, this.currentStep, stepTime);

    this.currentStep++;
    this.onStepUpdate?.({
      currentStep: this.currentStep,
      totalSteps:  scene.totalSteps,
      stepTime,
      nextStepTime,
    });

    if (this.currentStep >= scene.totalSteps) {
      if (this.pendingSceneIndex !== null) {
        this._switchToScene(this.pendingSceneIndex);
      } else {
        this.currentStep = 0; // loop
      }
    }
  }

  /**
   * Démarre la lecture.
   * @param {number} [startSceneIndex=0]
   */
  play(startSceneIndex = 0) {
    if (this.audioContext.state === 'suspended') {
      this.audioContext.resume();
    }
    this.currentStep = 0;
    this.isPlaying   = true;
    this._switchToScene(startSceneIndex, /* silent */ true);
  }

  /** Arrête la lecture et coupe tous les samples en cours. */
  stop() {
    this.isPlaying         = false;
    this.currentStep       = 0;
    this.pendingSceneIndex = null;
    this._stopAllSources();
    this.onPendingChange?.({ pendingIndex: null });
  }

  // ─── Privé ────────────────────────────────────────────────────────────────

  /**
   * Effectue la transition vers une scène.
   * @param {number} index
   * @param {boolean} [silent=false]  — true lors du play() initial (pas de callback onSceneChange)
   */
  _switchToScene(index, silent = false) {
    const oldIndex = this.currentSceneIndex;
    this.currentSceneIndex = index;
    this.currentStep       = 0;
    this.pendingSceneIndex = null;

    if (!silent) {
      this.onSceneChange?.({ newIndex: index, oldIndex });
    }
    this.onPendingChange?.({ pendingIndex: null });
  }

  /**
   * Déclenche tous les samples actifs au step courant.
   * Chaque track est un AudioBuffer préchargé dans scene.tracks[i].buffer.
   *
   * @param {Scene}  scene
   * @param {number} step
   * @param {number} time  — audioContext.currentTime précis
   */
  _triggerStep(scene, step, time) {
    scene.tracks.forEach((track, trackIndex) => {
      if (!track?.buffer) return;
      if (!scene.pattern[trackIndex]?.[step]) return;

      const src = this.audioContext.createBufferSource();
      src.buffer = track.buffer;

      // Gain par track (optionnel, défaut 1)
      const gain = this.audioContext.createGain();
      gain.gain.value = track.gain ?? 1;

      src.connect(gain).connect(this.audioContext.destination);
      src.start(time);

      // Déconnexion + nettoyage à la fin du sample (évite les fuites mémoire)
      src.onended = () => {
        src.disconnect();
        gain.disconnect();
        this._activeSources.delete(src);
      };
      this._activeSources.set(src, true);
    });
  }

  /** Coupe immédiatement tous les AudioBufferSourceNode actifs. */
  _stopAllSources() {
    this._activeSources.forEach((_, src) => {
      try { src.stop(); } catch (_) { /* déjà arrêté */ }
    });
    this._activeSources.clear();
  }
}
