/**
 * AudioBufferPool — cache de buffers audio décodés.
 *
 * - Déduplication par URL : un seul fetch/decode par sample
 * - Préchargement parallèle de toutes les scènes avant le play
 * - decodeAudioData hors du thread audio (asynchrone)
 */
export class AudioBufferPool {
  /** @param {AudioContext} audioContext */
  constructor(audioContext) {
    this.audioContext = audioContext;
    /** @type {Map<string, AudioBuffer>} */
    this._buffers = new Map();
    /** @type {Map<string, Promise<AudioBuffer>>} */
    this._pending = new Map();
  }

  /**
   * Charge et décode un sample. Retourne le buffer mis en cache si déjà chargé.
   * @param {string} url
   * @returns {Promise<AudioBuffer>}
   */
  async loadBuffer(url) {
    if (this._buffers.has(url)) return this._buffers.get(url);
    if (this._pending.has(url)) return this._pending.get(url);

    const promise = fetch(url)
      .then(r => r.arrayBuffer())
      .then(ab => this.audioContext.decodeAudioData(ab))
      .then(buf => {
        this._buffers.set(url, buf);
        this._pending.delete(url);
        return buf;
      });

    this._pending.set(url, promise);
    return promise;
  }

  /**
   * Précharge tous les samples référencés dans les scènes (en parallèle).
   * @param {SceneData[]} scenesData
   */
  async preloadProject(scenesData) {
    const urls = new Set(
      scenesData.flatMap(s => s.tracks.map(t => t.sampleUrl).filter(Boolean))
    );
    const results = await Promise.allSettled([...urls].map(url => this.loadBuffer(url)));
    const failures = results.filter(r => r.status === 'rejected');
    if (failures.length > 0) {
      console.warn(`[BufferPool] ⚠ ${failures.length} sample(s) échoué(s) :`,
        failures.map(f => f.reason));
    }
  }

  /**
   * Retourne le buffer mis en cache, ou null si non chargé.
   * @param {string} url
   * @returns {AudioBuffer|null}
   */
  get(url) {
    return this._buffers.get(url) ?? null;
  }
}
