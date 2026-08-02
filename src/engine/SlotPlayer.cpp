#include "engine/SlotPlayer.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace engine {

// ─── prepareStretchers ───────────────────────────────────────────────────────

void SlotPlayer::prepareStretchers(int channels, float sampleRate) noexcept {
    for (int s = 0; s < kSlots; ++s)
        stretchers_[s].prepare(channels, sampleRate);
}

// ─── armLoopSync ─────────────────────────────────────────────────────────────

void SlotPlayer::armLoopSync(int slot, int loopBeats, float timeRatio,
                              float semitones, int64_t anchorSample) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    params_[slot].loopBeats.store(loopBeats,    std::memory_order_relaxed);
    params_[slot].timeRatio.store(timeRatio,    std::memory_order_relaxed);
    params_[slot].semitones.store(semitones,    std::memory_order_relaxed);
    params_[slot].anchor.store(anchorSample,    std::memory_order_relaxed);
    stretchers_[slot].setParams(timeRatio, semitones);
    stretchers_[slot].reset();
}

// ─── loadSlot ────────────────────────────────────────────────────────────────

void SlotPlayer::loadSlot(int slot, SlotPcm pcm, PlayMode mode) noexcept {
    if (slot < 0 || slot >= kSlots) return;

    // Arrêter les voix actives immédiatement
    for (int v = 0; v < 2; ++v)
        voices_[slot][v] = Voice{};

    // Marquer comme non chargé avant le swap pour que le thread audio
    // ne lise pas un état intermédiaire.
    loaded_[slot].store(false, std::memory_order_release);

    pcm_[slot] = std::move(pcm);
    params_[slot].mode.store(mode, std::memory_order_relaxed);

    // Rendre visible au thread audio
    loaded_[slot].store(true, std::memory_order_release);
}

// ─── clearSlot ───────────────────────────────────────────────────────────────

void SlotPlayer::clearSlot(int slot) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    loaded_[slot].store(false, std::memory_order_release);
    for (int v = 0; v < 2; ++v)
        voices_[slot][v] = Voice{};
}

// ─── handleTrigger ───────────────────────────────────────────────────────────

void SlotPlayer::handleTrigger(int slot, int64_t transportAnchor) noexcept {
    // Choisir la voix inactive. Si les deux sont actives, prendre voice[1]
    // (la plus ancienne est voice[0]).
    int vIdx = 0;
    if (voices_[slot][0].active)
        vIdx = 1;

    Voice& voice = voices_[slot][vIdx];
    voice.active  = true;
    voice.readPos = 0;

    // Pour LOOP SYNC : mémoriser l'anchor dans les params.
    const PlayMode mode = params_[slot].mode.load(std::memory_order_relaxed);
    if (mode == PlayMode::LoopSync) {
        params_[slot].anchor.store(transportAnchor, std::memory_order_relaxed);
        stretchers_[slot].setParams(
            params_[slot].timeRatio.load(std::memory_order_relaxed),
            params_[slot].semitones.load(std::memory_order_relaxed));
        stretchers_[slot].reset();
    }

    // Micro-fade : seulement si le premier sample dépasse le seuil
    const SlotPcm& pcm = pcm_[slot];
    float firstSample = 0.f;
    if (pcm.numFrames > 0 && !pcm.data.empty())
        firstSample = pcm.data[0]; // canal 0, frame 0

    if (std::abs(firstSample) > kFadeThreshold) {
        voice.fadeGain = 0.0f;
        voice.fadeLeft = kFadeLen;
    } else {
        voice.fadeGain = 1.0f;
        voice.fadeLeft = 0;
    }
}

// ─── renderLoopSync ──────────────────────────────────────────────────────────
//
// Mode LOOP SYNC — position dérivée du transport (§4.3 du plan).
// La position source est RECALCULÉE à chaque bloc depuis le transport.
// Cela rend la dérive impossible par construction.

void SlotPlayer::renderLoopSync(int slot, float* out, int numFrames,
                                const TransportState& ts) noexcept {
    // Vérifier qu'il y a une voix active en mode LoopSync
    bool anyActive = false;
    for (int v = 0; v < 2; ++v)
        if (voices_[slot][v].active) { anyActive = true; break; }
    if (!anyActive) return;

    const SlotPcm& pcm = pcm_[slot];
    if (pcm.numFrames <= 0 || pcm.data.empty()) return;

    const float gain      = params_[slot].gain.load(std::memory_order_relaxed);
    const int32_t loopBts = params_[slot].loopBeats.load(std::memory_order_relaxed);
    const int64_t anchor  = params_[slot].anchor.load(std::memory_order_relaxed);

    if (loopBts <= 0) return;

    // durée de la loop en samples du projet
    const double loopLenProject = static_cast<double>(loopBts) * ts.samplesPerBeat;
    const double durOrig        = static_cast<double>(pcm.numFrames);

    const bool bypass = stretchers_[slot].isBypass();

    // Buffers temporaires planaires pour le stretch (max kCrossfadeLen + numFrames).
    // On utilise deux petits tableaux statiques locaux : pas d'allocation.
    // Taille max = kCrossfadeLen (256) << raisonnable pour la pile.
    static thread_local std::vector<float> tmpL;
    static thread_local std::vector<float> tmpR;
    const int N = numFrames;
    if (static_cast<int>(tmpL.size()) < N) tmpL.resize(static_cast<size_t>(N), 0.f);
    if (static_cast<int>(tmpR.size()) < N) tmpR.resize(static_cast<size_t>(N), 0.f);

    // Calculer la position source frame-par-frame depuis le transport.
    // Pour le chemin bypass, on lit directement depuis le PCM.
    // Pour le chemin stretch, on reconstruit le buffer source et on appelle process().

    if (bypass) {
        // ── Chemin bypass : lecture directe depuis srcPos ─────────────────
        for (int f = 0; f < N; ++f) {
            const int64_t tPos = ts.samplePos + static_cast<int64_t>(f);
            const double  elapsed = static_cast<double>(tPos - anchor);

            // modulo positif
            double phaseRaw = elapsed / loopLenProject;
            phaseRaw -= std::floor(phaseRaw);

            const int64_t srcPos = static_cast<int64_t>(phaseRaw * durOrig);
            const int64_t clampedPos = srcPos % static_cast<int64_t>(pcm.numFrames);

            float left, right;
            if (pcm.numChannels == 1) {
                const float s = pcm.data[static_cast<size_t>(clampedPos)];
                left  = s;
                right = s;
            } else {
                const size_t idx = static_cast<size_t>(clampedPos) * 2u;
                left  = pcm.data[idx];
                right = pcm.data[idx + 1u];
            }

            out[static_cast<size_t>(f) * 2u]      += left  * gain;
            out[static_cast<size_t>(f) * 2u + 1u] += right * gain;
        }
    } else {
        // ── Chemin stretch ────────────────────────────────────────────────
        // On reconstruit le bloc source PCM continu depuis la position dérivée
        // frame 0, puis on appelle StretchConform::process() avec ratio entrée/sortie.
        //
        // inputFrames = round(outputFrames * timeRatio)
        // Le timeRatio donne combien de frames source il faut consommer pour N frames sorties.
        const float timeRatio = params_[slot].timeRatio.load(std::memory_order_relaxed);
        const int inputFrames = static_cast<int>(std::round(static_cast<float>(N) * timeRatio));

        // Calculer la position source au début du bloc.
        const double elapsed0 = static_cast<double>(ts.samplePos - anchor);
        double phaseRaw0 = elapsed0 / loopLenProject;
        phaseRaw0 -= std::floor(phaseRaw0);
        int64_t srcPos0 = static_cast<int64_t>(phaseRaw0 * durOrig);

        // Construire le buffer source planaire (mono ou stéréo).
        static thread_local std::vector<float> srcL;
        static thread_local std::vector<float> srcR;
        if (static_cast<int>(srcL.size()) < inputFrames) srcL.resize(static_cast<size_t>(inputFrames), 0.f);
        if (static_cast<int>(srcR.size()) < inputFrames) srcR.resize(static_cast<size_t>(inputFrames), 0.f);

        for (int f = 0; f < inputFrames; ++f) {
            const int64_t sp = (srcPos0 + static_cast<int64_t>(f))
                               % static_cast<int64_t>(pcm.numFrames);
            if (pcm.numChannels == 1) {
                const float s  = pcm.data[static_cast<size_t>(sp)];
                srcL[static_cast<size_t>(f)] = s;
                srcR[static_cast<size_t>(f)] = s;
            } else {
                srcL[static_cast<size_t>(f)] = pcm.data[static_cast<size_t>(sp) * 2u];
                srcR[static_cast<size_t>(f)] = pcm.data[static_cast<size_t>(sp) * 2u + 1u];
            }
        }

        float* inPtrs[2]  = { srcL.data(), srcR.data() };
        float* outPtrs[2] = { tmpL.data(), tmpR.data() };

        stretchers_[slot].process(
            const_cast<const float* const*>(inPtrs), inputFrames,
            outPtrs, N);

        // Mixer dans la sortie stéréo entrelacée.
        for (int f = 0; f < N; ++f) {
            out[static_cast<size_t>(f) * 2u]      += tmpL[static_cast<size_t>(f)] * gain;
            out[static_cast<size_t>(f) * 2u + 1u] += tmpR[static_cast<size_t>(f)] * gain;
        }
    }
}

// ─── renderVoice ─────────────────────────────────────────────────────────────

void SlotPlayer::renderVoice(int slot, int v, float* out, int numFrames,
                             const TransportState& /*ts*/) noexcept {
    Voice& voice       = voices_[slot][v];
    const SlotPcm& pcm = pcm_[slot];

    if (!voice.active || pcm.numFrames <= 0 || pcm.data.empty())
        return;

    const float gain     = params_[slot].gain.load(std::memory_order_relaxed);
    const PlayMode mode  = params_[slot].mode.load(std::memory_order_relaxed);
    const bool loop      = (mode == PlayMode::Free);

    for (int f = 0; f < numFrames; ++f) {
        // Boucle (FREE) : reprendre au début si fin de PCM
        if (voice.readPos >= static_cast<int64_t>(pcm.numFrames)) {
            if (loop)
                voice.readPos = 0;
            else {
                voice.active = false;
                return;
            }
        }

        // Calcul du gain courant (micro-fade)
        float vGain = voice.fadeGain;
        if (voice.fadeLeft > 0) {
            // Fade linéaire de 0 → 1 sur kFadeLen frames
            voice.fadeGain += 1.0f / static_cast<float>(kFadeLen);
            if (voice.fadeGain > 1.0f) voice.fadeGain = 1.0f;
            --voice.fadeLeft;
        }

        // Lecture du PCM
        float left, right;
        if (pcm.numChannels == 1) {
            const float s = pcm.data[static_cast<size_t>(voice.readPos)];
            left  = s;
            right = s;
        } else {
            // stéréo interleaved : frame i → indices [2i, 2i+1]
            const size_t idx = static_cast<size_t>(voice.readPos) * 2u;
            left  = pcm.data[idx];
            right = pcm.data[idx + 1u];
        }

        const float g = vGain * gain;
        out[static_cast<size_t>(f) * 2u]      += left  * g;
        out[static_cast<size_t>(f) * 2u + 1u] += right * g;

        ++voice.readPos;
    }
}

// ─── processBlock ─────────────────────────────────────────────────────────────

void SlotPlayer::processBlock(const TransportState& ts,
                              float* output, int32_t numFrames,
                              const EngineEvent* events, int numEvents) noexcept {
    // Traiter les événements pour ce bloc (triés chronologiquement)
    for (int i = 0; i < numEvents; ++i) {
        const EngineEvent& ev = events[i];
        const int slot = static_cast<int>(ev.slot);
        if (slot < 0 || slot >= kSlots) continue;

        switch (ev.type) {
            case EventType::Trigger:
                if (loaded_[slot].load(std::memory_order_acquire))
                    handleTrigger(slot, ts.samplePos);
                break;
            case EventType::Mute:
                params_[slot].muted.store(true, std::memory_order_relaxed);
                break;
            case EventType::Unmute:
                params_[slot].muted.store(false, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }

    // Rendre tous les slots actifs
    for (int slot = 0; slot < kSlots; ++slot) {
        if (!loaded_[slot].load(std::memory_order_acquire)) continue;
        if (params_[slot].muted.load(std::memory_order_relaxed)) continue;

        const PlayMode mode = params_[slot].mode.load(std::memory_order_relaxed);

        if (mode == PlayMode::LoopSync) {
            // LOOP SYNC : rendu centralisé (position dérivée du transport)
            bool anyActive = false;
            for (int v = 0; v < 2; ++v)
                if (voices_[slot][v].active) { anyActive = true; break; }
            if (anyActive)
                renderLoopSync(slot, output, numFrames, ts);
        } else {
            for (int v = 0; v < 2; ++v) {
                if (voices_[slot][v].active)
                    renderVoice(slot, v, output, numFrames, ts);
            }
        }
    }
}

} // namespace engine
