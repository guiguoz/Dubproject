#include "engine/SlotPlayer.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace engine {

// ─── prepareStretchers ───────────────────────────────────────────────────────

void SlotPlayer::prepareStretchers(int channels, float sampleRate) noexcept {
    sampleRate_ = sampleRate;
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

    // Reset complet : aucun état résiduel (fadingOut, fadeLeft, readFrac…) ne survit.
    // pcm_[slot] et params_[slot] sont dans des tableaux séparés — non affectés.
    Voice& voice = voices_[slot][vIdx];
    voice = Voice{};
    voice.active = true;

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
                                const TransportState& ts, float fadeScale) noexcept {
    // Vérifier qu'il y a une voix active en mode LoopSync
    bool anyActive = false;
    for (int v = 0; v < 2; ++v)
        if (voices_[slot][v].active) { anyActive = true; break; }
    if (!anyActive) return;

    const SlotPcm& pcm = pcm_[slot];
    if (pcm.numFrames <= 0 || pcm.data.empty()) return;

    const float gain      = params_[slot].gain.load(std::memory_order_relaxed) * fadeScale;
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
            const int64_t tPos = ts.blockStart + static_cast<int64_t>(f);
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
        const double elapsed0 = static_cast<double>(ts.blockStart - anchor);
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

    const float gain    = params_[slot].gain.load(std::memory_order_relaxed);
    const PlayMode mode = params_[slot].mode.load(std::memory_order_relaxed);
    const bool loop     = (mode == PlayMode::Free);

    // Correction sample rate : si device SR ≠ sample SR, interpolation linéaire.
    // Exemple : sample 44100 Hz, device 48000 Hz → srRatio = 44100/48000 = 0.91875
    // → readPos avance de 0.91875 par frame output → pitch correct.
    const float srRatio = (sampleRate_ > 0.f) ? pcm.sampleRate / sampleRate_ : 1.f;
    const bool  needsSR = (std::abs(srRatio - 1.f) > 0.0005f);

    for (int f = 0; f < numFrames; ++f) {
        if (voice.readPos >= static_cast<int64_t>(pcm.numFrames)) {
            if (loop) { voice.readPos = 0; voice.readFrac = 0.f; }
            else       { voice.active = false; return; }
        }

        // Fade-out (stop transport) ou fade-in (attaque initiale)
        float vGain = voice.fadeGain;
        bool  deactivateAfter = false;

        if (voice.fadingOut) {
            if (voice.fadeLeft > 0) {
                // Rampe linéaire vers 0 : décrement proportionnel à la durée restante
                voice.fadeGain -= voice.fadeGain / static_cast<float>(voice.fadeLeft);
                --voice.fadeLeft;
                if (voice.fadeLeft == 0)
                    deactivateAfter = true;  // sortir après ce frame
            } else {
                voice.active = false;
                return;
            }
        } else if (voice.fadeLeft > 0) {
            // Fade-in initial (0 → 1 sur kFadeLen frames)
            voice.fadeGain += 1.0f / static_cast<float>(kFadeLen);
            if (voice.fadeGain > 1.0f) voice.fadeGain = 1.0f;
            --voice.fadeLeft;
        }

        float left, right;

        if (needsSR) {
            // Interpolation linéaire entre frame p0 et p1
            const int64_t p0   = voice.readPos;
            const int64_t p1   = p0 + 1;
            const float   frac = voice.readFrac;

            if (pcm.numChannels == 1) {
                const float s0 = pcm.data[static_cast<size_t>(p0)];
                const float s1 = (p1 < static_cast<int64_t>(pcm.numFrames))
                                  ? pcm.data[static_cast<size_t>(p1)] : s0;
                left = right = s0 + frac * (s1 - s0);
            } else {
                const size_t i0 = static_cast<size_t>(p0) * 2u;
                const float  l0 = pcm.data[i0],         r0 = pcm.data[i0 + 1u];
                const float  l1 = (p1 < static_cast<int64_t>(pcm.numFrames))
                                   ? pcm.data[static_cast<size_t>(p1) * 2u]       : l0;
                const float  r1 = (p1 < static_cast<int64_t>(pcm.numFrames))
                                   ? pcm.data[static_cast<size_t>(p1) * 2u + 1u]  : r0;
                left  = l0 + frac * (l1 - l0);
                right = r0 + frac * (r1 - r0);
            }

            voice.readFrac += srRatio;
            while (voice.readFrac >= 1.f) {
                voice.readFrac -= 1.f;
                ++voice.readPos;
                if (voice.readPos >= static_cast<int64_t>(pcm.numFrames)) {
                    if (loop) { voice.readPos = 0; voice.readFrac = 0.f; }
                    else       { voice.active = false; return; }
                    break;
                }
            }
        } else {
            if (pcm.numChannels == 1) {
                const float s = pcm.data[static_cast<size_t>(voice.readPos)];
                left  = s;
                right = s;
            } else {
                const size_t idx = static_cast<size_t>(voice.readPos) * 2u;
                left  = pcm.data[idx];
                right = pcm.data[idx + 1u];
            }
            ++voice.readPos;
        }

        const float g = vGain * gain;
        out[static_cast<size_t>(f) * 2u]      += left  * g;
        out[static_cast<size_t>(f) * 2u + 1u] += right * g;

        if (deactivateAfter) { voice.active = false; return; }
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
                    handleTrigger(slot, ts.blockStart);
                break;
            case EventType::Release: {
                // Fade-out court (~10 ms) pour éviter les clics au stop transport.
                // Ne ré-arme pas si déjà en cours de fade-out.
                const int fadeLen = std::max(1, static_cast<int>(sampleRate_ * 0.01f));
                for (int vi = 0; vi < 2; ++vi) {
                    Voice& vc = voices_[slot][vi];
                    if (!vc.active || vc.fadingOut) continue;
                    vc.fadingOut = true;
                    vc.fadeLeft  = fadeLen;
                    // fadeGain conserve sa valeur courante (1.0 en régime, <1 si attaque)
                }
                break;
            }
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
            if (!anyActive) continue;

            // Fade-out bloc par bloc pour LoopSync
            float fadeScale = 1.0f;
            Voice& vc0 = voices_[slot][0];
            if (vc0.fadingOut) {
                if (vc0.fadeLeft <= 0) {
                    // Fade terminé — désactiver sans rien rendre
                    voices_[slot][0] = Voice{};
                    voices_[slot][1] = Voice{};
                    continue;
                }
                fadeScale = vc0.fadeGain;
                const int consumed = std::min(vc0.fadeLeft, static_cast<int>(numFrames));
                // Avance le fade proportionnellement aux frames consommées
                vc0.fadeGain -= vc0.fadeGain * static_cast<float>(consumed)
                                             / static_cast<float>(vc0.fadeLeft);
                vc0.fadeLeft -= consumed;
                if (vc0.fadeLeft <= 0) {
                    vc0.active       = false;
                    voices_[slot][1] = Voice{};
                }
            }
            renderLoopSync(slot, output, numFrames, ts, fadeScale);
        } else {
            for (int v = 0; v < 2; ++v) {
                if (voices_[slot][v].active)
                    renderVoice(slot, v, output, numFrames, ts);
            }
        }
    }
}

// ─── Diagnostic voix (message thread) ────────────────────────────────────────

SlotPlayer::VoiceDiagInfo SlotPlayer::getVoiceDiagInfo(int slot) const noexcept {
    VoiceDiagInfo d{};
    if (slot < 0 || slot >= kSlots) return d;
    for (int v = 0; v < 2; ++v) {
        d.active[v]    = voices_[slot][v].active;
        d.fadingOut[v] = voices_[slot][v].fadingOut;
        d.readPos[v]   = voices_[slot][v].readPos;
        d.fadeGain[v]  = voices_[slot][v].fadeGain;
    }
    d.numFrames = pcm_[slot].numFrames;
    return d;
}

float SlotPlayer::diagSrRatio(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots || sampleRate_ <= 0.f) return 1.f;
    const float sr = pcm_[slot].sampleRate;
    return (sr > 0.f) ? sr / sampleRate_ : 1.f;
}

} // namespace engine
