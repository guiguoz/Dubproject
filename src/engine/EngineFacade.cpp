#include "engine/EngineFacade.h"
#include <JuceHeader.h>
#include <thread>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace engine {

EngineFacade::EngineFacade()
{
    for (int s = 0; s < kMaxSlots; ++s) {
        trackBars_[s]              = 1;
        writePatterns_[s].numSteps = 16;
    }
}

// ─── Cycle de vie ─────────────────────────────────────────────────────────────

void EngineFacade::prepare(double sampleRate, int maxBlockSize) noexcept
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    const double bpm = (transport_.state().bpm > 0.0) ? transport_.state().bpm : 120.0;
    transport_.prepare(sampleRate, bpm);
    graph_.prepare(sampleRate, maxBlockSize);

    interleavedOut_.assign(static_cast<size_t>(maxBlockSize * 2), 0.f);
}

void EngineFacade::releaseResources() noexcept
{
    transport_.stop();
}

// ─── Audio callback ───────────────────────────────────────────────────────────

void EngineFacade::processBlock(float* left, float* right, int numSamples) noexcept
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    audioBlockCounter_.fetch_add(1, std::memory_order_release);

    const auto& ts = transport_.advance(numSamples);
    const int64_t blockStart = ts.samplePos - static_cast<int64_t>(numSamples);

    // ── Drainer les events UI (triggers/stops de pads live) ──────────────────
    {
        const uint16_t trigs = pendingTriggers_.exchange(0, std::memory_order_acq_rel);
        const uint16_t stops = pendingStops_.exchange(0, std::memory_order_acq_rel);
        for (int s = 0; s < kMaxSlots; ++s) {
            if (trigs & (1u << s)) {
                EngineEvent ev{};
                ev.time = kNextBlock;
                ev.type = EventType::Trigger;
                ev.slot = static_cast<uint8_t>(s);
                scheduler_.push(ev);
            }
            if (stops & (1u << s)) {
                EngineEvent ev{};
                ev.time = kNextBlock;
                ev.type = EventType::Release;
                ev.slot = static_cast<uint8_t>(s);
                scheduler_.push(ev);
            }
        }
    }

    // ── TransitionEngine + Séquenceur ─────────────────────────────────────────
    transition_.processBlock(ts, scheduler_);
    graph_.sequencer().generateEvents(ts, blockStart, numSamples,
                                      swingFactor_, scheduler_);

    // ── Extraire les events → buffer stack (pas d'allocation) ────────────────
    EngineEvent evBuf[256];
    const int evCount = std::min(scheduler_.size(), 256);
    for (int i = 0; i < evCount; ++i)
        evBuf[i] = scheduler_.at(i);

    // ── AudioGraph ────────────────────────────────────────────────────────────
    std::fill(interleavedOut_.begin(),
              interleavedOut_.begin() + numSamples * 2, 0.f);
    graph_.processBlock(ts, evBuf, evCount, interleavedOut_.data(), numSamples);
    scheduler_.clear();

    // ── Désentrelacement + RMS master ─────────────────────────────────────────
    float sumSq = 0.f;
    for (int i = 0; i < numSamples; ++i) {
        const float l = interleavedOut_[i * 2    ];
        const float r = interleavedOut_[i * 2 + 1];
        left [i] = l;
        right[i] = r;
        sumSq   += l * l + r * r;
    }
    if (numSamples > 0) {
        const float rms = std::sqrt(sumSq / static_cast<float>(numSamples * 2));
        masterRms_.store(rms, std::memory_order_relaxed);
    }

    // ── CPU load ──────────────────────────────────────────────────────────────
    const float elapsed = std::chrono::duration<float>(Clock::now() - t0).count();
    const float budget  = static_cast<float>(numSamples) / static_cast<float>(sampleRate_);
    if (budget > 0.f)
        cpuLoad_.store(elapsed / budget * 100.f, std::memory_order_relaxed);
}

// ─── Transport ────────────────────────────────────────────────────────────────

void EngineFacade::play() noexcept { transport_.play(); }
void EngineFacade::stop() noexcept { transport_.stop(); }

void EngineFacade::setBpm(float bpm) noexcept
{
    if (bpm <= 0.f) return;
    transport_.setBpm(static_cast<double>(bpm));
}

// ─── Slots ────────────────────────────────────────────────────────────────────

void EngineFacade::importSampleAsync(int slot, const std::string& filePath,
                                     ImportCallback cb)
{
    if (slot < 0 || slot >= kMaxSlots) return;

    std::thread([this, slot, filePath, cb]() {
        juce::AudioFormatManager fmtMgr;
        fmtMgr.registerBasicFormats();

        const auto file = juce::File(juce::String(filePath));
        std::unique_ptr<juce::AudioFormatReader> reader(fmtMgr.createReaderFor(file));
        if (!reader) {
            if (cb) cb(slot, AnalysisResult{});
            return;
        }

        const int   numCh     = static_cast<int>(reader->numChannels);
        const int   numFrames = static_cast<int>(reader->lengthInSamples);
        const float sr        = static_cast<float>(reader->sampleRate);

        if (numFrames <= 0) {
            if (cb) cb(slot, AnalysisResult{});
            return;
        }

        const int readCh = std::min(numCh, 2);
        juce::AudioBuffer<float> buf(readCh, numFrames);
        reader->read(&buf, 0, numFrames, 0, true, readCh > 1);

        // Downmix mono pour l'analyse
        std::vector<float> mono(static_cast<size_t>(numFrames));
        if (readCh == 1) {
            const float* ch0 = buf.getReadPointer(0);
            std::copy(ch0, ch0 + numFrames, mono.begin());
        } else {
            const float* ch0 = buf.getReadPointer(0);
            const float* ch1 = buf.getReadPointer(1);
            for (int i = 0; i < numFrames; ++i)
                mono[static_cast<size_t>(i)] = (ch0[i] + ch1[i]) * 0.5f;
        }

        // Analyse synchrone (BPM, rôle, key, autoLoopSync)
        const float projectBpm = getBpm();
        AnalysisResult result = importPipeline_.analyzeSync(
            mono.data(), numFrames, 1, sr, projectBpm);

        // Construction du SlotPcm (conserve la stéréo si disponible)
        SlotPcm pcm;
        pcm.numChannels = readCh;
        pcm.numFrames   = numFrames;
        pcm.sampleRate  = sr;
        pcm.data.resize(static_cast<size_t>(numFrames * readCh));
        if (readCh == 1) {
            std::copy(mono.begin(), mono.end(), pcm.data.begin());
        } else {
            const float* ch0 = buf.getReadPointer(0);
            const float* ch1 = buf.getReadPointer(1);
            for (int i = 0; i < numFrames; ++i) {
                pcm.data[static_cast<size_t>(i * 2)    ] = ch0[i];
                pcm.data[static_cast<size_t>(i * 2 + 1)] = ch1[i];
            }
        }

        // ── Garde-fou data race : attente de 3 blocs audio complets ──────────────
        // Le Release event sera traité dans le bloc N (prochain callback audio).
        // Après N+1 le fade est terminé (kFadeLen=16 ≪ taille de bloc).
        // N+2 donne une marge supplémentaire : garanti par construction que
        // voices_[slot][v].active == false avant l'écriture du nouveau PCM.
        stopSlot(slot, true);
        const int64_t targetBlock = audioBlockCounter_.load(std::memory_order_acquire) + 3;
        while (audioBlockCounter_.load(std::memory_order_acquire) < targetBlock)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Chargement dans le SlotPlayer (message thread / worker thread — pas audio)
        const PlayMode mode = result.autoLoopSync ? PlayMode::LoopSync : PlayMode::Free;
        graph_.slotPlayer().loadSlot(slot, std::move(pcm), mode);
        slotLoaded_[slot].store(true, std::memory_order_release);

        if (cb) cb(slot, result);
    }).detach();
}

void EngineFacade::clearSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.slotPlayer().clearSlot(slot);
    slotLoaded_[slot].store(false, std::memory_order_relaxed);
}

void EngineFacade::triggerSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    pendingTriggers_.fetch_or(static_cast<uint16_t>(1u << slot),
                              std::memory_order_release);
}

void EngineFacade::stopSlot(int slot, bool /*immediate*/) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    pendingStops_.fetch_or(static_cast<uint16_t>(1u << slot),
                           std::memory_order_release);
}

void EngineFacade::setSlotGain(int slot, float gain) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.slotPlayer().setGain(slot, gain);
}

float EngineFacade::getSlotGain(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 1.f;
    return graph_.slotPlayer().getGain(slot);
}

void EngineFacade::setSlotMuted(int slot, bool muted, bool /*quantized*/) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.slotPlayer().setMuted(slot, muted);
}

bool EngineFacade::isSlotMuted(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return graph_.slotPlayer().isMuted(slot);
}

void EngineFacade::setSlotTransposeSemitones(int slot, float semitones) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.slotPlayer().setSemitones(slot, semitones);
}

void EngineFacade::setSlotMode(int slot, PlayMode mode) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.slotPlayer().setMode(slot, mode);
}

void EngineFacade::setSlotRole(int slot, SlotRole role) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.setSlotRole(slot, role);
}

// ─── Métriques slot ───────────────────────────────────────────────────────────

float EngineFacade::getSlotPlayheadRatio(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    return graph_.slotPlayer().playheadRatio(slot);
}

float EngineFacade::getSlotOutputPeak(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    return slotPeak_[slot].load(std::memory_order_relaxed);
}

bool EngineFacade::isSlotPlaying(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return graph_.slotPlayer().isVoiceActive(slot);
}

bool EngineFacade::isSlotLoaded(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slotLoaded_[slot].load(std::memory_order_acquire);
}

// ─── Séquenceur ───────────────────────────────────────────────────────────────

void EngineFacade::setStep(int track, int step, bool active) noexcept
{
    if (track < 0 || track >= kMaxSlots) return;
    if (step  < 0 || step  >= kMaxSteps) return;
    writePatterns_[track].steps[step] = active;
}

bool EngineFacade::getStep(int track, int step) const noexcept
{
    if (track < 0 || track >= kMaxSlots) return false;
    if (step  < 0 || step  >= kMaxSteps) return false;
    return writePatterns_[track].steps[step];
}

int EngineFacade::getTrackStepCount(int track) const noexcept
{
    if (track < 0 || track >= kMaxSlots) return 16;
    return writePatterns_[track].numSteps;
}

void EngineFacade::setTrackBarCount(int track, int bars) noexcept
{
    if (track < 0 || track >= kMaxSlots) return;
    if (bars  <= 0) return;
    trackBars_[track]              = bars;
    writePatterns_[track].numSteps = bars * 16;
}

int EngineFacade::getTrackBarCount(int track) const noexcept
{
    if (track < 0 || track >= kMaxSlots) return 1;
    return trackBars_[track];
}

void EngineFacade::flipPatternBuffer() noexcept
{
    // Copie writePatterns_ → write side du PatternBuffer, puis flip atomique.
    for (int s = 0; s < kMaxSlots; ++s) {
        TrackPattern* dst = graph_.sequencer().patterns().writeBuffer(s);
        *dst = writePatterns_[s];
    }
    graph_.sequencer().patterns().flip();
}

// ─── Diagnostics slot (temporaire — M8c) ─────────────────────────────────────

float EngineFacade::getSlotSemitones(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    return graph_.slotPlayer().getSemitones(slot);
}

float EngineFacade::getSlotTimeRatio(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 1.f;
    return graph_.slotPlayer().getTimeRatio(slot);
}

PlayMode EngineFacade::getSlotMode(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return PlayMode::Free;
    return graph_.slotPlayer().getMode(slot);
}

int EngineFacade::getSlotLoopBeats(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0;
    return graph_.slotPlayer().getLoopBeats(slot);
}

// ─── Scènes ───────────────────────────────────────────────────────────────────

void EngineFacade::setCurrentScene(int idx) noexcept
{
    if (idx < 0 || idx >= kMaxScenes) return;
    currentScene_ = idx;
    applySceneInternal(idx);
}

void EngineFacade::requestTransition(int toScene) noexcept
{
    if (toScene < 0 || toScene >= kMaxScenes) return;
    transition_.requestTransition(currentScene_, toScene,
                                  sceneStore_, transport_.state());
}

void EngineFacade::applySceneInternal(int idx) noexcept
{
    const SceneData& sc = sceneStore_.getScene(idx);
    for (int s = 0; s < kMaxSlots; ++s) {
        const SlotConfig& cfg = sc.slots[s];
        graph_.slotPlayer().setGain(s, cfg.gain);
        graph_.slotPlayer().setMode(s, cfg.mode);
        graph_.slotPlayer().setSemitones(s, cfg.semitones);
        graph_.setSlotRole(s, cfg.role);
    }
}

} // namespace engine
