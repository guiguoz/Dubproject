#include "engine/SlotPlayer.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace engine {

// ─── getPcmSnapshot ──────────────────────────────────────────────────────────

std::vector<float> SlotPlayer::getPcmSnapshot(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return {};
    const auto& pcm = activePcm(slot);
    if (pcm.data.empty()) return {};

    if (pcm.numChannels == 1)
        return pcm.data;   // déjà mono — copie

    // Downmix stéréo entrelacé → mono
    std::vector<float> mono(static_cast<std::size_t>(pcm.numFrames));
    for (int i = 0; i < pcm.numFrames; ++i) {
        mono[static_cast<std::size_t>(i)] =
            (pcm.data[static_cast<std::size_t>(i * 2)] +
             pcm.data[static_cast<std::size_t>(i * 2 + 1)]) * 0.5f;
    }
    return mono;
}

float SlotPlayer::getPcmSampleRate(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return 0.f;
    return activePcm(slot).sampleRate;
}

// ─── prepareStretchers ───────────────────────────────────────────────────────

void SlotPlayer::prepareStretchers(int channels, float sampleRate, int maxBlockSize) noexcept {
    sampleRate_ = sampleRate;
    for (int s = 0; s < kSlots; ++s)
        stretchers_[s].prepare(channels, sampleRate);
    // Pré-allouer les buffers temporaires (zéro allocation en audio thread).
    tmpL_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    tmpR_.assign(static_cast<size_t>(maxBlockSize), 0.f);
    srcL_.assign(static_cast<size_t>(maxBlockSize) * 2, 0.f);   // x2 pour timeRatio > 1
    srcR_.assign(static_cast<size_t>(maxBlockSize) * 2, 0.f);
    slotRenderScratch_.assign(static_cast<size_t>(maxBlockSize) * 2, 0.f);
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

// ─── Staging pour ENTER (PREPARE → COMMIT) ────────────────────────────────────
void SlotPlayer::stagePcm(int slot, SlotPcm pcm, PlayMode mode) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    const int inactive = 1 - activePcmIdx_[slot].load(std::memory_order_relaxed);
    pcmBuffers_[inactive][slot] = std::move(pcm);
    stagedMode_[slot] = mode;
    stagedIdx_[slot].store(inactive, std::memory_order_release);
    stagedReady_[slot].store(true, std::memory_order_release);
}
bool SlotPlayer::hasStagedPcm(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return false;
    return stagedReady_[slot].load(std::memory_order_acquire);
}
void SlotPlayer::commitStagedPcm(int slot) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    if (!stagedReady_[slot].load(std::memory_order_acquire)) return;
    const int idx = stagedIdx_[slot].load(std::memory_order_acquire);
    // Flip atomique — le thread audio voit le nouveau PCM dès le sample suivant
    activePcmIdx_[slot].store(idx, std::memory_order_release);
    params_[slot].mode.store(stagedMode_[slot], std::memory_order_relaxed);
    resetSpatialSlot(slot);
    // Pas de kill de voix : KEEP/MORPH ne retrigg pas, ENTER sera triggé par Sequencer
    loaded_[slot].store(true, std::memory_order_release);
    stagedReady_[slot].store(false, std::memory_order_release);
}
void SlotPlayer::clearStagedPcm(int slot) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    stagedReady_[slot].store(false, std::memory_order_release);
}

// ─── loadSlot ────────────────────────────────────────────────────────────────

void SlotPlayer::loadSlot(int slot, SlotPcm pcm, PlayMode mode) noexcept {
    if (slot < 0 || slot >= kSlots) return;

    // Arrêter les voix actives immédiatement (atomique active = false)
    for (int v = 0; v < 2; ++v) {
        voices_[slot][v].active.store(false, std::memory_order_relaxed);
        voiceActive_[slot][v].store(false, std::memory_order_relaxed);
    }

    // Rampe de transition neutre (pas de fade résiduel sur un nouveau PCM)
    rampValue_ [slot] = 1.0f;
    rampTarget_[slot] = 1.0f;
    rampLeft_  [slot].store(0, std::memory_order_relaxed);

    // Marquer comme non chargé avant le swap pour que le thread audio
    // ne lise pas un état intermédiaire.
    loaded_[slot].store(false, std::memory_order_release);

    // Annuler tout staging en cours (load immédiat prioritaire)
    stagedReady_[slot].store(false, std::memory_order_release);

    // Double-buffer : écrire dans le buffer inactif, puis flip atomique
    const int inactive = 1 - activePcmIdx_[slot].load(std::memory_order_relaxed);
    pcmBuffers_[inactive][slot] = std::move(pcm);
    params_[slot].mode.store(mode, std::memory_order_relaxed);
    resetSpatialSlot(slot);

    // Flip — le thread audio voit maintenant le nouveau PCM
    activePcmIdx_[slot].store(inactive, std::memory_order_release);
    loaded_[slot].store(true, std::memory_order_release);
}

// ─── clearSlot ───────────────────────────────────────────────────────────────

void SlotPlayer::clearSlot(int slot) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    stagedReady_[slot].store(false, std::memory_order_release);
    loaded_[slot].store(false, std::memory_order_release);
    for (int v = 0; v < 2; ++v) {
        voices_[slot][v].active.store(false, std::memory_order_relaxed);
        voiceActive_[slot][v].store(false, std::memory_order_relaxed);
    }
    rampValue_ [slot] = 1.0f;
    rampTarget_[slot] = 1.0f;
    rampLeft_  [slot].store(0, std::memory_order_relaxed);
    resetSpatialSlot(slot);
}

// ─── advanceRamps ────────────────────────────────────────────────────────────

void SlotPlayer::advanceRamps(int numFrames) noexcept {
    for (int s = 0; s < kSlots; ++s) {
        int left = rampLeft_[s].load(std::memory_order_relaxed);
        if (left <= 0) continue;
        const int consumed = std::min(left, numFrames);
        const float frac = static_cast<float>(consumed) / static_cast<float>(left);
        rampValue_[s] += (rampTarget_[s] - rampValue_[s]) * frac;
        left -= consumed;
        rampLeft_[s].store(left, std::memory_order_relaxed);
        if (left <= 0)
            rampValue_[s] = rampTarget_[s];
    }
}

// ─── resetSpatialSlot ─────────────────────────────────────────────────────────

void SlotPlayer::resetSpatialSlot(int slot) noexcept {
    if (slot < 0 || slot >= kSlots) return;
    haasWritePos_[slot] = 0;
    std::fill(haasDelay_[slot], haasDelay_[slot] + kHaasDelayMax, 0.f);
}

// ─── spatialGains ─────────────────────────────────────────────────────────────
//
// Loi égal-power : angle = (pan+1)·π/4 →
// gL = cos(angle), gR = sin(angle). Le canal faible reçoit le signal Haas.
// Si pan == 0 ET width == 0 → identité (gL = gR = 1) : préserve la transparence
// bit-exact T-SP1 (appliquer le 0.7071 de centre
// casserait l'invariant de transparence).

void SlotPlayer::spatialGains(int slot, float& gL, float& gR,
                              bool& haasOnLeft) noexcept {
    const float pan   = params_[slot].pan.load(std::memory_order_relaxed);
    const float width = params_[slot].width.load(std::memory_order_relaxed);

    if (pan == 0.f && width == 0.f) {
        gL = 1.f; gR = 1.f; haasOnLeft = false;
        return;
    }

    constexpr float kPi = 3.14159265358979f;
    const float angle = (std::clamp(pan, -1.f, 1.f) + 1.f) * 0.25f * kPi;
    gL = std::cos(angle);
    gR = std::sin(angle);
    haasOnLeft = (gL < gR);
}

// ─── applyHaasDelay ───────────────────────────────────────────────────────────

float SlotPlayer::applyHaasDelay(int slot, float sample) noexcept {
    const float width = params_[slot].width.load(std::memory_order_relaxed);
    int delay = static_cast<int>(width * 0.025f * sampleRate_);
    if (delay <= 0) return sample;
    if (delay >= kHaasDelayMax) delay = kHaasDelayMax - 1;

    auto& buf = haasDelay_[slot];
    const int wp = haasWritePos_[slot];
    buf[static_cast<size_t>(wp)] = sample;
    const int rp = (wp - delay + kHaasDelayMax) & (kHaasDelayMax - 1);
    haasWritePos_[slot] = (wp + 1) & (kHaasDelayMax - 1);
    return buf[static_cast<size_t>(rp)];
}

// ─── handleTrigger ───────────────────────────────────────────────────────────

void SlotPlayer::handleTrigger(int slot, int64_t transportAnchor) noexcept {
    const PlayMode mode = params_[slot].mode.load(std::memory_order_relaxed);

    // 1. Pour OneShot/Free : mettre l'ancienne voix en fade-out anti-clic (≈1.5 ms).
    //    LoopSync exclu : position dérivée du transport, pas de double-voix possible.
    int lastActiveVoice = -1;
    if (mode != PlayMode::LoopSync) {
        for (int v = 0; v < 2; ++v) {
            Voice& vc = voices_[slot][v];
            if (vc.active.load(std::memory_order_relaxed)) {
                lastActiveVoice = v;
                if (!vc.fadingOut) {
                    vc.fadingOut = true;
                    vc.fadeGain  = 1.0f;
                    vc.fadeLeft  = kRetrigFadeLen;
                }
            }
        }
    }

    // 2. Sélectionner la nouvelle voix en alternant.
    const int vIdx = (lastActiveVoice == 0) ? 1 : 0;

    // 3. Initialiser la nouvelle voix.
    Voice& voice = voices_[slot][vIdx];
    voice.active.store(false, std::memory_order_relaxed);
    voice.fadingOut = false;
    voice.readPos   = 0;
    voice.readFrac  = 0.f;
    voice.fadeGain  = 1.0f;
    voice.fadeLeft  = 0;
    voice.active.store(true, std::memory_order_relaxed);
    voiceActive_[slot][vIdx].store(true, std::memory_order_relaxed);

    // Pour LOOP SYNC : mémoriser l'anchor dans les params.
    if (mode == PlayMode::LoopSync) {
        params_[slot].anchor.store(transportAnchor, std::memory_order_relaxed);
        stretchers_[slot].setParams(
            params_[slot].timeRatio.load(std::memory_order_relaxed),
            params_[slot].semitones.load(std::memory_order_relaxed));
        stretchers_[slot].reset();
    }

    // Micro-fade anti-clic uniquement si le premier échantillon dépasse le seuil.
    // Sur retrigger, PAS de fade-in forcé : la nouvelle voix démarre à plein gain
    // pour préserver l'attaque. Le clic est évité par le fade-out sur l'ancienne voix.
    const SlotPcm& pcm = activePcm(slot);
    if (pcm.numFrames > 0 && !pcm.data.empty()
            && std::abs(pcm.data[0]) > kFadeThreshold) {
        voice.fadeGain = 0.0f;
        voice.fadeLeft = kFadeLen;
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
        if (voices_[slot][v].active.load(std::memory_order_relaxed)) { anyActive = true; break; }
    if (!anyActive) return;

    const SlotPcm& pcm = activePcm(slot);
    if (pcm.numFrames <= 0 || pcm.data.empty()) return;

    const float gain      = params_[slot].gain.load(std::memory_order_relaxed)
                              * fadeScale * rampValue_[slot];
    const int32_t loopBts = params_[slot].loopBeats.load(std::memory_order_relaxed);
    const int64_t anchor  = params_[slot].anchor.load(std::memory_order_relaxed);

    if (loopBts <= 0) return;

    // durée de la loop en samples du projet
    const double loopLenProject = static_cast<double>(loopBts) * ts.samplesPerBeat;
    const double durOrig        = static_cast<double>(pcm.numFrames);

    const bool bypass = stretchers_[slot].isBypass();

    // Spatialisation pan + Haas (M9 étape 6) — gains calculés une fois par bloc.
    float gL, gR; bool haasOnLeft;
    spatialGains(slot, gL, gR, haasOnLeft);

    // Buffers temporaires planaires pour le stretch (pré-alloués en prepare).
    const int N = numFrames;
    if (static_cast<int>(tmpL_.size()) < N) tmpL_.resize(static_cast<size_t>(N), 0.f);
    if (static_cast<int>(tmpR_.size()) < N) tmpR_.resize(static_cast<size_t>(N), 0.f);

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

            const float sL = left  * gain;
            const float sR = right * gain;
            if (haasOnLeft) {
                out[static_cast<size_t>(f) * 2u]      += applyHaasDelay(slot, sL) * gL;
                out[static_cast<size_t>(f) * 2u + 1u] += sR * gR;
            } else {
                out[static_cast<size_t>(f) * 2u]      += sL * gL;
                out[static_cast<size_t>(f) * 2u + 1u] += applyHaasDelay(slot, sR) * gR;
            }

            const float p = std::max(std::abs(sL), std::abs(sR));
            if (p > slotPeak_[slot].load(std::memory_order_relaxed))
                slotPeak_[slot].store(p, std::memory_order_relaxed);
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
        if (static_cast<int>(srcL_.size()) < inputFrames) srcL_.resize(static_cast<size_t>(inputFrames), 0.f);
        if (static_cast<int>(srcR_.size()) < inputFrames) srcR_.resize(static_cast<size_t>(inputFrames), 0.f);

        for (int f = 0; f < inputFrames; ++f) {
            const int64_t sp = (srcPos0 + static_cast<int64_t>(f))
                               % static_cast<int64_t>(pcm.numFrames);

            if (pcm.numChannels == 1) {
                const float s = pcm.data[static_cast<size_t>(sp)];
                srcL_[static_cast<size_t>(f)] = s;
                srcR_[static_cast<size_t>(f)] = s;
            } else {
                srcL_[static_cast<size_t>(f)] = pcm.data[static_cast<size_t>(sp) * 2u];
                srcR_[static_cast<size_t>(f)] = pcm.data[static_cast<size_t>(sp) * 2u + 1u];
            }
        }

        float* inPtrs[2]  = { srcL_.data(), srcR_.data() };
        float* outPtrs[2] = { tmpL_.data(), tmpR_.data() };

        stretchers_[slot].process(
            const_cast<const float* const*>(inPtrs), inputFrames,
            outPtrs, N);

        // Mixer dans la sortie stéréo entrelacée.
        for (int f = 0; f < N; ++f) {
            const float sL = tmpL_[static_cast<size_t>(f)] * gain;
            const float sR = tmpR_[static_cast<size_t>(f)] * gain;
            if (haasOnLeft) {
                out[static_cast<size_t>(f) * 2u]      += applyHaasDelay(slot, sL) * gL;
                out[static_cast<size_t>(f) * 2u + 1u] += sR * gR;
            } else {
                out[static_cast<size_t>(f) * 2u]      += sL * gL;
                out[static_cast<size_t>(f) * 2u + 1u] += applyHaasDelay(slot, sR) * gR;
            }

            float p = std::abs(sL);
            if (std::abs(sR) > p) p = std::abs(sR);
            if (p > slotPeak_[slot].load(std::memory_order_relaxed))
                slotPeak_[slot].store(p, std::memory_order_relaxed);
        }
    }
}

// ─── renderVoice ─────────────────────────────────────────────────────────────

void SlotPlayer::renderVoice(int slot, int v, float* out, int numFrames,
                             const TransportState& /*ts*/) noexcept {
    Voice& voice       = voices_[slot][v];
    const SlotPcm& pcm = activePcm(slot);

    if (!voice.active.load(std::memory_order_relaxed) || pcm.numFrames <= 0 || pcm.data.empty())
        return;

    const float gain    = params_[slot].gain.load(std::memory_order_relaxed)
                        * rampValue_[slot];
    const PlayMode mode = params_[slot].mode.load(std::memory_order_relaxed);
    const bool loop     = (mode == PlayMode::Free);

    // Spatialisation pan + Haas (M9 étape 6) — gains calculés une fois par bloc.
    float gL, gR; bool haasOnLeft;
    spatialGains(slot, gL, gR, haasOnLeft);

    // Correction sample rate : si device SR ≠ sample SR, interpolation linéaire.
    // Exemple : sample 44100 Hz, device 48000 Hz → srRatio = 44100/48000 = 0.91875
    // → readPos avance de 0.91875 par frame output → pitch correct.
    const float srRatio = (sampleRate_ > 0.f) ? pcm.sampleRate / sampleRate_ : 1.f;
    const bool  needsSR = (std::abs(srRatio - 1.f) > 0.0005f);

    for (int f = 0; f < numFrames; ++f) {
        if (voice.readPos >= static_cast<int64_t>(pcm.numFrames)) {
            if (loop) { voice.readPos = 0; voice.readFrac = 0.f; }
            else       { deactivateVoice(slot, v); return; }
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
                deactivateVoice(slot, v);
                return;
            }
        } else if (voice.fadeLeft > 0) {
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
                    else       { deactivateVoice(slot, v); return; }
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

        const float g  = vGain * gain;
        const float sL = left  * g;
        const float sR = right * g;
        if (haasOnLeft) {
            out[static_cast<size_t>(f) * 2u]      += applyHaasDelay(slot, sL) * gL;
            out[static_cast<size_t>(f) * 2u + 1u] += sR * gR;
        } else {
            out[static_cast<size_t>(f) * 2u]      += sL * gL;
            out[static_cast<size_t>(f) * 2u + 1u] += applyHaasDelay(slot, sR) * gR;
        }

        // Pic de sortie du slot (VU) — avant spatialisation
        const float p = std::max(std::abs(sL), std::abs(sR));
        if (p > slotPeak_[slot].load(std::memory_order_relaxed))
            slotPeak_[slot].store(p, std::memory_order_relaxed);

        if (deactivateAfter) { deactivateVoice(slot, v); return; }
    }
}

// ─── Diag getters (S-C silence) ──────────────────────────────────────────────

int SlotPlayer::getPcmFrames(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return 0;
    return activePcm(slot).numFrames;
}
bool SlotPlayer::isPcmEmpty(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return true;
    const auto& p = activePcm(slot);
    return p.data.empty() || p.numFrames <= 0;
}
float SlotPlayer::getPcmMaxAbsFirst64(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return 0.f;
    const auto& p = activePcm(slot);
    if (p.data.empty() || p.numFrames <= 0) return 0.f;
    const int n = std::min(64, p.numFrames) * p.numChannels;
    float mx = 0.f;
    for (int i = 0; i < n; ++i) {
        const float a = std::abs(p.data[static_cast<size_t>(i)]);
        if (a > mx) mx = a;
    }
    return mx;
}
int SlotPlayer::getActiveVoiceCount(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return 0;
    return (voiceActive_[slot][0].load(std::memory_order_relaxed) ? 1 : 0)
         + (voiceActive_[slot][1].load(std::memory_order_relaxed) ? 1 : 0);
}
float SlotPlayer::getRampValue(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return 1.f;
    return rampValue_[slot];
}
int SlotPlayer::getRampLeft(int slot) const noexcept {
    if (slot < 0 || slot >= kSlots) return 0;
    return rampLeft_[slot].load(std::memory_order_relaxed);
}

// ─── processBlock ─────────────────────────────────────────────────────────────

void SlotPlayer::processBlock(const TransportState& ts,
                              float* output, int32_t numFrames,
                              const EventWithOffset* events, int numEvents,
                              float** perSlotL, float** perSlotR) noexcept {
    // Avancer les rampes de transition + reset des pics de sortie.
    advanceRamps(numFrames);
    for (int slot = 0; slot < kSlots; ++slot)
        slotPeak_[slot].store(0.f, std::memory_order_relaxed);

    // Dispatch temporel : renderer en sous-blocs aux frontières d'events.
    //
    // INVARIANT TEMPOREL (P0) : chaque segment [renderPos, nextEventPos) est
    // rendu avec l'état ANTERIEUR aux events de nextEventPos, puis les events
    // sont dispatchés (effet à partir de nextEventPos). Le temps utilisé par
    // renderLoopSync() ET par l'anchor de handleTrigger() correspond toujours
    // au début absolu réel du sous-bloc / de l'event — jamais ts.blockStart
    // brut passé le 1er segment, jamais un état post-event pour le passé.
    // eventTime et subTs ci-dessous font circuler explicitement ces temps.
    int evIdx = 0;
    int renderPos = 0;

    while (renderPos < numFrames) {
        // Trouver le prochain event dans ce sous-bloc
        int nextEventPos = numFrames;  // par défaut, renderer jusqu'à la fin
        if (evIdx < numEvents) {
            nextEventPos = events[evIdx].offset;
            if (nextEventPos > numFrames) nextEventPos = numFrames;
        }

        // 1. Renderer le sous-bloc [renderPos, nextEventPos), calé sur son
        // début absolu (renderLoopSync lit subTs.blockStart). Copie POD.
        {
            const int subLen = nextEventPos - renderPos;
            if (subLen > 0) {
                float* subOut = (output != nullptr)
                    ? output + static_cast<size_t>(renderPos) * 2u
                    : nullptr;
                TransportState subTs = ts;
                subTs.blockStart = ts.blockStart + static_cast<int64_t>(renderPos);
                renderSlots(subTs, subOut, subLen, renderPos, perSlotL, perSlotR);
            }
        }

        // 2. Dispatcher les events à nextEventPos (effet à partir de ce sample).
        // Temps absolu de l'event (== début absolu du segment qu'il ouvre).
        const int64_t eventTime = ts.blockStart + static_cast<int64_t>(nextEventPos);

        // Traiter les events à cette position
        while (evIdx < numEvents && events[evIdx].offset == nextEventPos) {
            const EngineEvent& ev = events[evIdx].ev;
            const int slot = static_cast<int>(ev.slot);
            if (slot >= 0 && slot < kSlots) {
                switch (ev.type) {
                    case EventType::Trigger:
                        if (loaded_[slot].load(std::memory_order_acquire))
                            handleTrigger(slot, eventTime);
                        break;
                    case EventType::Release: {
                        const int fadeLen = std::max(1, static_cast<int>(sampleRate_ * 0.01f));
                        for (int vi = 0; vi < 2; ++vi) {
                            Voice& vc = voices_[slot][vi];
                            if (!vc.active.load(std::memory_order_relaxed) || vc.fadingOut) continue;
                            vc.fadingOut = true;
                            vc.fadeLeft  = fadeLen;
                        }
                        break;
                    }
                    case EventType::Mute:
                        params_[slot].muted.store(true, std::memory_order_relaxed);
                        break;
                    case EventType::Unmute:
                        params_[slot].muted.store(false, std::memory_order_relaxed);
                        break;
                    case EventType::GainRamp: {
                        const int dur = static_cast<int>(ev.a);
                        if (dur <= 0) {
                            rampValue_ [slot] = ev.b;
                            rampTarget_[slot] = ev.b;
                            rampLeft_  [slot].store(0, std::memory_order_relaxed);
                        } else {
                            rampTarget_[slot] = ev.b;
                            rampLeft_  [slot].store(dur, std::memory_order_relaxed);
                        }
                        break;
                    }
                    case EventType::PcmFlip:
                        commitStagedPcm(slot);
                        break;
                    case EventType::ModeSet:
                        params_[slot].mode.store(static_cast<PlayMode>(static_cast<int>(ev.a)), std::memory_order_relaxed);
                        break;
                    case EventType::TransposeSet:
                    case EventType::SemitoneSet:
                        params_[slot].semitones.store(ev.a, std::memory_order_relaxed);
                        break;
                    case EventType::RoleSet:
                        // Role is handled via AudioGraph; no-op here (handled in EngineFacade COMMIT)
                        break;
                    default:
                        break;
                }
            }
            ++evIdx;
        }
        renderPos = nextEventPos;
    }
}

// ─── renderSlot ──────────────────────────────────────────────────────────────

void SlotPlayer::renderSlot(int slot, const TransportState& ts,
                            float* output, int32_t numFrames,
                            int frameOffset,
                            float* perSlotL, float* perSlotR) noexcept
{
    if (slot < 0 || slot >= kSlots) return;
    if (!loaded_[slot].load(std::memory_order_acquire)) return;
    if (params_[slot].muted.load(std::memory_order_relaxed)) return;

    const bool needScratch = (output == nullptr && (perSlotL != nullptr || perSlotR != nullptr));
    float* renderOut = output;
    if (needScratch) {
        std::fill(slotRenderScratch_.begin(),
                  slotRenderScratch_.begin() + static_cast<size_t>(numFrames) * 2, 0.f);
        renderOut = slotRenderScratch_.data();
    }

    const PlayMode mode = params_[slot].mode.load(std::memory_order_relaxed);

    if (mode == PlayMode::LoopSync) {
        bool anyActive = false;
        for (int v = 0; v < 2; ++v)
            if (voices_[slot][v].active.load(std::memory_order_relaxed)) { anyActive = true; break; }
        if (!anyActive) return;

        float fadeScale = 1.0f;
        Voice& vc0 = voices_[slot][0];
        if (vc0.fadingOut) {
            if (vc0.fadeLeft <= 0) {
                deactivateVoice(slot, 0);
                deactivateVoice(slot, 1);
                return;
            }
            fadeScale = vc0.fadeGain;
            const int consumed = std::min(vc0.fadeLeft, static_cast<int>(numFrames));
            vc0.fadeGain -= vc0.fadeGain * static_cast<float>(consumed)
                                         / static_cast<float>(vc0.fadeLeft);
            vc0.fadeLeft -= consumed;
            if (vc0.fadeLeft <= 0) {
                deactivateVoice(slot, 0);
                deactivateVoice(slot, 1);
            }
        }
        renderLoopSync(slot, renderOut, numFrames, ts, fadeScale);
    } else {
        for (int v = 0; v < 2; ++v) {
            if (voices_[slot][v].active.load(std::memory_order_relaxed))
                renderVoice(slot, v, renderOut, numFrames, ts);
        }
    }

    if (perSlotL != nullptr || perSlotR != nullptr) {
        float* slotL = (perSlotL != nullptr) ? perSlotL + frameOffset : nullptr;
        float* slotR = (perSlotR != nullptr) ? perSlotR + frameOffset : nullptr;
        const float* src = needScratch ? slotRenderScratch_.data() : renderOut;
        if (src != nullptr) {
            for (int f = 0; f < numFrames; ++f) {
                const float l = src[static_cast<size_t>(f) * 2];
                const float r = src[static_cast<size_t>(f) * 2 + 1];
                if (slotL != nullptr) slotL[f] += l;
                if (slotR != nullptr) slotR[f] += r;
            }
        }
    }
}

// ─── renderSlots (extrait de l'ancien processBlock) ──────────────────────────

void SlotPlayer::renderSlots(const TransportState& ts,
                              float* output, int32_t numFrames,
                              int frameOffset,
                              float** perSlotL, float** perSlotR) noexcept {

    const bool perSlotMode = (perSlotL != nullptr || perSlotR != nullptr);
    float* mixOut = perSlotMode ? nullptr : output;

    for (int slot = 0; slot < kSlots; ++slot) {
        float* slotL = (perSlotL != nullptr) ? perSlotL[slot] : nullptr;
        float* slotR = (perSlotR != nullptr) ? perSlotR[slot] : nullptr;
        renderSlot(slot, ts, mixOut, numFrames, frameOffset, slotL, slotR);
    }
}

} // namespace engine
