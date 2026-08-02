#include "engine/SlotPlayer.h"
#include <cmath>
#include <cstring>

namespace engine {

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

void SlotPlayer::handleTrigger(int slot) noexcept {
    // Choisir la voix inactive. Si les deux sont actives, prendre voice[1]
    // (la plus ancienne est voice[0]).
    int vIdx = 0;
    if (voices_[slot][0].active)
        vIdx = 1;

    Voice& voice = voices_[slot][vIdx];
    voice.active  = true;
    voice.readPos = 0;

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

// ─── renderVoice ─────────────────────────────────────────────────────────────

void SlotPlayer::renderVoice(int slot, int v, float* out, int numFrames) noexcept {
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

void SlotPlayer::processBlock(const TransportState& /*ts*/,
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
                    handleTrigger(slot);
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

        for (int v = 0; v < 2; ++v) {
            if (voices_[slot][v].active)
                renderVoice(slot, v, output, numFrames);
        }
    }
}

} // namespace engine
