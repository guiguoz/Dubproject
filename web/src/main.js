/**
 * main.js — point d'entrée de la couche web de DubEngine.
 *
 * Initialise SceneManager, Scheduler et SceneSelectorUI,
 * puis charge les scènes depuis le JSON du projet (.saxfx).
 */

import { SceneManager }    from './audio/SceneManager.js';
import { Scheduler }       from './audio/Scheduler.js';
import { SceneSelectorUI } from './ui/SceneSelectorUI.js';
import { AudioBufferPool } from './audio/AudioBufferPool.js';

// ─── Données de scènes (exemple aligné sur SceneSaveData du projet C++) ──────
//
// En production : charger depuis un fichier .saxfx (JSON) via fetch().
// Chaque track.buffer doit être un AudioBuffer décodé depuis un fichier WAV.

const SCENES_DATA = [
  { name: 'Intro',  lengthInBars: 2, stepsPerBar: 16, tracks: [], pattern: [] },
  { name: 'Build',  lengthInBars: 4, stepsPerBar: 16, tracks: [], pattern: [] },
  { name: 'Drop',   lengthInBars: 4, stepsPerBar: 16, tracks: [], pattern: [] },
  { name: 'Break',  lengthInBars: 2, stepsPerBar: 16, tracks: [], pattern: [] },
  { name: 'Outro',  lengthInBars: 2, stepsPerBar: 16, tracks: [], pattern: [] },
];

// ─── Initialisation ───────────────────────────────────────────────────────────

const audioContext  = new AudioContext();
const bufferPool    = new AudioBufferPool(audioContext);
const sceneManager  = new SceneManager(audioContext);
const scheduler     = new Scheduler(audioContext, sceneManager);

sceneManager.loadScenes(SCENES_DATA);

const selectorContainer = document.getElementById('scene-selector');
if (selectorContainer) {
  new SceneSelectorUI(selectorContainer, sceneManager);
}

// ─── Contrôles transport ──────────────────────────────────────────────────────

const playBtn = document.getElementById('play-btn');
const stopBtn = document.getElementById('stop-btn');
const bpmInput = document.getElementById('bpm-input');

playBtn?.addEventListener('click', async () => {
  try {
    await audioContext.resume();
  } catch (err) {
    console.error('[DubEngine] audioContext.resume() échoué :', err);
    return;
  }
  sceneManager.play(0);
  scheduler.start();
  playBtn.disabled = true;
  stopBtn.disabled = false;
});

stopBtn?.addEventListener('click', () => {
  sceneManager.stop();
  scheduler.stop();
  playBtn.disabled = false;
  stopBtn.disabled = true;
});

bpmInput?.addEventListener('change', () => {
  const bpm = parseFloat(bpmInput.value);
  if (bpm > 0) scheduler.setBpm(bpm);
});

// ─── Chargement de samples depuis un fichier .saxfx (optionnel) ───────────────

/**
 * Charge un projet .saxfx et met à jour les scènes.
 * @param {File} file
 */
async function loadProject(file) {
  const text    = await file.text();
  const project = JSON.parse(text);

  const scenesData = project.scenes
    .filter(s => s.used)
    .map(s => ({
      name:         s.name ?? 'Scène',
      lengthInBars: 1,
      stepsPerBar:  16,
      tracks: s.filePaths.map((sampleUrl, i) => ({
        sampleUrl,
        gain: s.gains[i] ?? 1,
        buffer: null, // rempli après préchargement
      })),
      pattern: s.steps.map(trackSteps => [...trackSteps].map(v => v ? 1 : 0)),
    }));

  // Précharger tous les samples AVANT de mettre à jour les scènes
  await bufferPool.preloadProject(scenesData);

  // Injecter les buffers décodés dans chaque track
  scenesData.forEach(scene => {
    scene.tracks.forEach(track => {
      if (track.sampleUrl) track.buffer = bufferPool.get(track.sampleUrl);
    });
  });

  sceneManager.loadScenes(scenesData);
}

const fileInput = document.getElementById('project-file');
fileInput?.addEventListener('change', e => {
  const file = e.target.files[0];
  if (!file) return;
  const nameEl = document.getElementById('file-name');
  if (nameEl) nameEl.textContent = file.name;
  loadProject(file);
});
