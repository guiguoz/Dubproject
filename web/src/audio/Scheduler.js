/**
 * Scheduler — timing audio précis par look-ahead.
 *
 * Principe : setTimeout toutes les 25 ms pour vérifier si des steps
 * doivent être planifiés dans la fenêtre de 100 ms à venir.
 * Chaque step est déclenché via audioContext.currentTime (précision sample).
 *
 * ⚠️  Ne jamais utiliser setInterval pour le timing audio.
 */
export class Scheduler {
  /**
   * @param {AudioContext}  audioContext
   * @param {SceneManager}  sceneManager
   */
  constructor(audioContext, sceneManager) {
    this.audioContext    = audioContext;
    this.sceneManager    = sceneManager;

    this.bpm              = 120;
    this.scheduleAheadTime = 0.1;   // secondes — fenêtre de look-ahead
    this.schedulerInterval = 25;    // ms — fréquence du timer

    this._nextStepTime = 0;
    this._timerId      = null;
  }

  /** Démarre le scheduling depuis audioContext.currentTime. */
  start() {
    this._nextStepTime = this.audioContext.currentTime;
    this._schedule();
  }

  /** Arrête le timer. */
  stop() {
    if (this._timerId !== null) {
      clearTimeout(this._timerId);
      this._timerId = null;
    }
  }

  /** @param {number} bpm */
  setBpm(bpm) {
    this.bpm = bpm;
    // Scheduler est l'unique propriétaire du BPM — plus de sync vers SceneManager
  }

  // ─── Privé ────────────────────────────────────────────────────────────────

  /**
   * Durée d'un 1/16e de note en secondes.
   * stepDuration = (60 / bpm) / 4
   */
  get _stepDuration() {
    return 60.0 / this.bpm / 4;
  }

  /**
   * Boucle de scheduling : planifie tous les steps dans la fenêtre de look-ahead,
   * puis se re-programme via setTimeout.
   */
  _schedule() {
    const horizon = this.audioContext.currentTime + this.scheduleAheadTime;

    while (this._nextStepTime < horizon) {
      const next = this._nextStepTime + this._stepDuration;
      this.sceneManager.onTick(this._nextStepTime, next);
      this._nextStepTime = next;
    }

    this._timerId = setTimeout(() => this._schedule(), this.schedulerInterval);
  }
}
