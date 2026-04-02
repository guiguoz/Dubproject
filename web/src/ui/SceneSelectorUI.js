/**
 * SceneSelectorUI — composant visuel pour la sélection et la transition de scènes.
 *
 * Génère et gère :
 *   - Boutons de scènes (actif, clignotant si en attente)
 *   - Barre de progression (bleu → dégradé orange si pending)
 *   - Compteur de steps restants
 */
export class SceneSelectorUI {
  /**
   * @param {HTMLElement}  container
   * @param {SceneManager} sceneManager
   */
  constructor(container, sceneManager) {
    this._container    = container;
    this._sceneManager = sceneManager;
    this._blinkTimer   = null;
    // État pour l'interpolation rAF
    this._stepTime     = 0;
    this._nextStepTime = 0;
    this._stepProgress = 0; // progression au dernier step reçu (0–1)
    this._stepSize     = 0; // taille d'un step en unités de progression
    this._rafId        = null;

    this._buildUI();
    this._bindCallbacks();
    this._startProgressAnimation();
  }

  // ─── Construction ─────────────────────────────────────────────────────────

  _buildUI() {
    this._container.innerHTML = `
      <div class="scene-selector">
        <div class="scene-progress-container">
          <div class="scene-progress-bar"></div>
        </div>
        <div class="scene-buttons-row"></div>
        <div class="pending-indicator hidden"></div>
      </div>`;

    this._progressBar      = this._container.querySelector('.scene-progress-bar');
    this._buttonsRow       = this._container.querySelector('.scene-buttons-row');
    this._pendingIndicator = this._container.querySelector('.pending-indicator');

    this._renderButtons();
  }

  _renderButtons() {
    this._buttonsRow.innerHTML = '';
    this._sceneManager.scenes.forEach((scene, i) => {
      const btn = document.createElement('button');
      btn.className        = 'scene-btn';
      btn.dataset.sceneIndex = i;
      btn.innerHTML = `
        <span class="scene-number">${i + 1}</span>
        <span class="scene-name">${scene.name}</span>`;
      btn.addEventListener('click', () => this._sceneManager.requestSceneChange(i));
      this._buttonsRow.appendChild(btn);
    });

    // Marquer la scène active initiale
    this._updateActiveButton(this._sceneManager.currentSceneIndex);
  }

  // ─── Callbacks SceneManager ───────────────────────────────────────────────

  _bindCallbacks() {
    this._sceneManager.onSceneChange = ({ newIndex }) => {
      this._updateActiveButton(newIndex);
      this._stopBlinking();
      this._progressBar.classList.remove('has-pending');
    };

    this._sceneManager.onPendingChange = ({ pendingIndex }) => {
      if (pendingIndex !== null) {
        this._startBlinking(pendingIndex);
        this._progressBar.classList.add('has-pending');
      } else {
        this._stopBlinking();
        this._progressBar.classList.remove('has-pending');
        this._pendingIndicator.classList.add('hidden');
      }
    };

    this._sceneManager.onStepUpdate = ({ currentStep, totalSteps, stepTime, nextStepTime }) => {
      this._stepProgress = currentStep / totalSteps;
      this._stepSize     = 1 / totalSteps;
      this._stepTime     = stepTime;
      this._nextStepTime = nextStepTime;

      const pending = this._sceneManager.pendingSceneIndex;
      if (pending !== null) {
        this._updateCountdown(totalSteps - currentStep, pending);
      }
    };
  }

  // ─── Méthodes d'état visuel ───────────────────────────────────────────────

  _updateActiveButton(activeIndex) {
    this._buttonsRow.querySelectorAll('.scene-btn').forEach((btn, i) => {
      btn.classList.toggle('active', i === activeIndex);
    });
  }

  _startBlinking(sceneIndex) {
    this._stopBlinking();
    const btn = this._buttonsRow.querySelector(`[data-scene-index="${sceneIndex}"]`);
    if (btn) btn.classList.add('pending-blink');
  }

  _stopBlinking() {
    this._buttonsRow.querySelectorAll('.pending-blink').forEach(btn => {
      btn.classList.remove('pending-blink');
    });
  }

  /** @param {number} progress — 0 à 1 */
  _updateProgress(progress) {
    this._progressBar.style.width = `${Math.min(progress * 100, 100)}%`;
  }

  /**
   * Boucle rAF : interpole la barre de progression entre deux steps
   * en utilisant audioContext.currentTime comme référence temporelle.
   */
  _startProgressAnimation() {
    const animate = () => {
      if (this._sceneManager.isPlaying && this._nextStepTime > this._stepTime) {
        const elapsed  = this._sceneManager.audioContext.currentTime - this._stepTime;
        const duration = this._nextStepTime - this._stepTime;
        const interp   = Math.min(elapsed / duration, 1);
        this._updateProgress(this._stepProgress + interp * this._stepSize);
      } else {
        // Transport arrêté : remettre la barre à zéro
        this._updateProgress(0);
      }
      this._rafId = requestAnimationFrame(animate);
    };
    this._rafId = requestAnimationFrame(animate);
  }

  /**
   * @param {number} remainingSteps
   * @param {number} pendingIndex
   */
  _updateCountdown(remainingSteps, pendingIndex) {
    const scene = this._sceneManager.scenes[pendingIndex];
    if (!scene) return;

    this._pendingIndicator.textContent =
      remainingSteps > 0
        ? `⏭ Scène ${pendingIndex + 1} "${scene.name}" dans ${remainingSteps} steps`
        : `⏭ Transition vers Scène ${pendingIndex + 1}…`;
    this._pendingIndicator.classList.toggle('imminent', remainingSteps <= 4);
    this._pendingIndicator.classList.remove('hidden');
  }
}
