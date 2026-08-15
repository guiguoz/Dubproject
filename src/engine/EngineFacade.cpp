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
        diagLastRole_[s].store(-1, std::memory_order_relaxed);
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

// ─── Thread de mix temps réel (50 ms) ────────────────────────────────────────
// Recalcule les cibles AutoMix (gains + sends) depuis les rôles courants.
// Équivalent temps réel de la simulation offline (§11.2).

void EngineFacade::startMixThread() noexcept
{
    if (mixThreadStarted_.load(std::memory_order_acquire)) return;
    mixThreadRun_.store(true, std::memory_order_release);
    mixThreadStarted_.store(true, std::memory_order_release);
    mixThread_ = std::thread([this] {
        while (mixThreadRun_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            graph_.updateAutoMixTargets();
        }
    });
}

void EngineFacade::stopMixThread() noexcept
{
    if (!mixThreadStarted_.load(std::memory_order_acquire)) return;
    mixThreadRun_.store(false, std::memory_order_release);
    if (mixThread_.joinable())
        mixThread_.join();
    mixThreadStarted_.store(false, std::memory_order_release);
}

// ─── Audio callback ───────────────────────────────────────────────────────────

void EngineFacade::processBlock(float* left, float* right, int numSamples,
                                const float* extInL, const float* extInR,
                                const float* serumL, const float* serumR,
                                float serumGain) noexcept
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    audioBlockCounter_.fetch_add(1, std::memory_order_release);

    const auto& ts = transport_.advance(numSamples);
    // blockStart est fourni par le transport (début du bloc courant),
    // l'invariant est ainsi centralisé et ne dépend plus de la taille du bloc.
    const int64_t blockStart = ts.blockStart;

    // ── Diagnostic régularité transport (M8c à retirer) ─────────────────────
    {
        const int64_t prev  = diagPrevBlockStart_.load(std::memory_order_relaxed);
        const int32_t prevN = diagPrevBlockN_.load(std::memory_order_relaxed);
        if (ts.playing && prev >= 0 && prevN > 0) {
            const int64_t expected = prev + static_cast<int64_t>(prevN);
            const int64_t delta    = blockStart - expected;   // 0 si consécutif
            diagBlockDeltaErr_.store(delta, std::memory_order_relaxed);
        } else {
            diagBlockDeltaErr_.store(0, std::memory_order_relaxed);
        }
        diagPrevBlockStart_.store(blockStart, std::memory_order_relaxed);
        diagPrevBlockN_.store(numSamples, std::memory_order_relaxed);
    }

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

    // ── Flush delay si stop demandé depuis message thread ────────────────────
    if (delayResetPending_.exchange(false, std::memory_order_acq_rel))
        graph_.delay().reset();

    // ── Log triggers kick pour diagnostic (M8c à retirer) ────────────────────
    for (int i = 0; i < evCount; ++i) {
        if (evBuf[i].type != EventType::Trigger) continue;
        if (static_cast<int>(evBuf[i].slot) != 2) continue;   // slot 2 = KCK
        const bool fromSeq = (evBuf[i].time != kNextBlock);
        const int64_t pos        = fromSeq ? evBuf[i].time : ts.samplePos;
        const int64_t globalStep = stepIndexAt(ts, pos);
        // Step de pattern (modulo) — c'est ce que le séquenceur a réellement matché
        const TrackPattern* pat  = graph_.sequencer().patterns().readBuffer(2);
        const int32_t nSteps     = (pat->numSteps > 0) ? pat->numSteps : 1;
        const int32_t patStep    = static_cast<int32_t>(globalStep % static_cast<int64_t>(nSteps));
        const int idx = diagTrigHead_.load(std::memory_order_relaxed) % kTrigDiag;
        diagTrigPos_ [idx].store(pos,     std::memory_order_relaxed);
        diagTrigStep_[idx].store(patStep, std::memory_order_relaxed);
        diagTrigSrc_ [idx].store(fromSeq, std::memory_order_relaxed);
        diagTrigHead_.fetch_add(1, std::memory_order_release);
    }

    // ── AudioGraph ────────────────────────────────────────────────────────────
    std::fill(interleavedOut_.begin(),
              interleavedOut_.begin() + numSamples * 2, 0.f);
    graph_.processBlock(ts, evBuf, evCount, interleavedOut_.data(), numSamples,
                        extInL, extInR, serumL, serumR, serumGain);
    scheduler_.clear();

    // ── Désentrelacement + RMS master ─────────────────────────────────────────
    // left == right : sortie mono → downmix (L+R)·0.5 dans le seul buffer
    // (le graphe rend toujours en stéréo entrelacé).
    const bool monoOut = (left == right);
    if (monoOut)
        downmixInterleavedToMono(interleavedOut_.data(), left, numSamples);
    float sumSq = 0.f;
    for (int i = 0; i < numSamples; ++i) {
        const float l = interleavedOut_[i * 2    ];
        const float r = interleavedOut_[i * 2 + 1];
        if (!monoOut) {
            left [i] = l;
            right[i] = r;
        }
        sumSq += l * l + r * r;
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

void EngineFacade::stop() noexcept {
    transport_.stop();
    constexpr uint16_t kAllSlots = (1u << kMaxSlots) - 1u;
    pendingStops_.fetch_or(kAllSlots, std::memory_order_release);
    delayResetPending_.store(true, std::memory_order_release);
}

void EngineFacade::setBpm(float bpm) noexcept
{
    if (bpm <= 0.f) return;
    transport_.setBpm(static_cast<double>(bpm));
}

// ─── Slots ────────────────────────────────────────────────────────────────────

// Déduit le PlayMode depuis le résultat d'analyse et le slot cible.
// Deux garde-fous indépendants pour OneShot :
//   1. Rôle ONNX percussif (Kick/Snare/HiHat/Perc/Unknown)
//   2. Rôle sémantique du slot (CLAUDE.md §1 — fixe, immuable)
// Un seul suffit : couvre les erreurs de classification ONNX sur les slots percussifs.
static PlayMode modeForResult(const AnalysisResult& r, int slot) noexcept
{
    if (r.autoLoopSync)
        return PlayMode::LoopSync;

    const bool rolePercussive = (r.role == SlotRoleV2::Kick   ||
                                 r.role == SlotRoleV2::Snare  ||
                                 r.role == SlotRoleV2::HiHat  ||
                                 r.role == SlotRoleV2::Perc   ||
                                 r.role == SlotRoleV2::Unknown);

    // Slots percussifs sémantiques fixes (CLAUDE.md) : KCK=2, SNR=3, HAT=4, PRC=7
    const bool slotPercussive = (slot == 2 || slot == 3 || slot == 4 || slot == 7);

    if (rolePercussive || slotPercussive)
        return PlayMode::OneShot;

    return PlayMode::Free;  // Bass, Melodic, Pad, Fx, Loop, MST, SYN, DRM
}

// Mappe le rôle ONNX (SlotRoleV2) vers le rôle moteur (SlotRole) utilisé par
// l'AutoMix (gain staging + sidechain) et la détection du slot kick.
static SlotRole mapRole(SlotRoleV2 r) noexcept
{
    switch (r) {
        case SlotRoleV2::Kick:    return SlotRole::Kick;
        case SlotRoleV2::Snare:   return SlotRole::Snare;
        case SlotRoleV2::HiHat:   return SlotRole::Perc;
        case SlotRoleV2::Bass:    return SlotRole::Bass;
        case SlotRoleV2::Melodic: return SlotRole::Melodic;
        case SlotRoleV2::Pad:     return SlotRole::Pad;
        case SlotRoleV2::Perc:    return SlotRole::Perc;
        case SlotRoleV2::Fx:      return SlotRole::Fx;
        case SlotRoleV2::Loop:    return SlotRole::Loop;
        default:                  return SlotRole::Loop;   // Unknown → neutre
    }
}

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

        diagLastRole_[slot].store(static_cast<int32_t>(result.role),
                                  std::memory_order_relaxed);

        // Rôle → AutoMix (gain staging, sends, sidechain) + détection kick.
        graph_.setSlotRole(slot, mapRole(result.role));

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
        const PlayMode mode = modeForResult(result, slot);
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

void EngineFacade::setSlotSolo(int slot, bool soloed) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    soloSlot_.store(soloed ? slot : -1, std::memory_order_relaxed);
    for (int i = 0; i < kMaxSlots; ++i)
        graph_.slotPlayer().setMuted(i, soloed ? (i != slot) : false);
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
    return graph_.slotPlayer().getSlotPeak(slot);
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

std::vector<float> EngineFacade::getSlotPcmSnapshot(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    return graph_.slotPlayer().getPcmSnapshot(slot);
}

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

std::string EngineFacade::getTriggerDiag(int slot) const noexcept
{
    (void)slot;  // actuellement hardcodé sur slot 2 (KCK)
    const int head = diagTrigHead_.load(std::memory_order_acquire);
    if (head == 0) return "no trig";

    // Lire les 4 derniers (ou moins si < 4 triggers au total)
    const int count = std::min(head, kTrigDiag);
    const int start = head - count;
    std::string s;
    for (int i = start; i < head; ++i) {
        const int idx = i % kTrigDiag;
        const int64_t pos  = diagTrigPos_ [idx].load(std::memory_order_relaxed);
        const int32_t step = diagTrigStep_[idx].load(std::memory_order_relaxed);
        const bool    src  = diagTrigSrc_ [idx].load(std::memory_order_relaxed);
        s += (src ? "S" : "U");   // S=Sequencer, U=UI/pad
        s += std::to_string(step);
        s += "@";
        s += std::to_string(pos);
        s += " ";
    }
    return s;
}

std::string EngineFacade::getPatternDiag(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return "slot invalide";

    // Lecture writePatterns_ (source de vérité message thread)
    const TrackPattern& wp = writePatterns_[slot];
    std::string wStr = "write:[";
    bool first = true;
    const int wn = std::min(wp.numSteps, kMaxSteps);
    for (int s = 0; s < wn; ++s) {
        if (wp.steps[s]) {
            if (!first) wStr += ",";
            wStr += std::to_string(s);
            first = false;
        }
    }
    wStr += "]";

    // Lecture du buffer actif (ce que le thread audio lit réellement)
    const TrackPattern* ap = graph_.sequencer().patterns().readBuffer(slot);
    std::string aStr = "active:[";
    first = true;
    const int an = std::min(ap->numSteps, kMaxSteps);
    for (int s = 0; s < an; ++s) {
        if (ap->steps[s]) {
            if (!first) aStr += ",";
            aStr += std::to_string(s);
            first = false;
        }
    }
    aStr += "]";

    return wStr + " " + aStr + " nSteps=" + std::to_string(wp.numSteps);
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

std::string EngineFacade::getVoiceDiag(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return "inv";
    const auto& sp       = graph_.slotPlayer();
    const PlayMode mode  = sp.getMode(slot);
    const float srRatio  = sp.diagSrRatio(slot);
    const auto d         = sp.getVoiceDiagInfo(slot);

    const char* mStr = (mode == PlayMode::OneShot) ? "1S"
                     : (mode == PlayMode::Free)     ? "FR" : "LS";

    // Rôle détecté par l'analyse (SlotRoleV2 as int)
    static const char* const kRoleNames[] = {
        "Kick","Snare","HiHat","Bass","Melodic","Pad","Perc","Fx","Loop","Unk"
    };
    const int32_t roleIdx = diagLastRole_[slot].load(std::memory_order_relaxed);
    const char* roleStr = (roleIdx >= 0 && roleIdx < 9) ? kRoleNames[roleIdx] : "?";

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s role=%s sr=%.3f nF=%d", mStr, roleStr, srRatio, d.numFrames);
    std::string s = buf;
    for (int v = 0; v < 2; ++v) {
        s += " v";
        s += static_cast<char>('0' + v);
        s += '[';
        if (!d.active[v]) {
            s += "off";
        } else {
            std::snprintf(buf, sizeof(buf), "r=%lld%s g=%.2f",
                static_cast<long long>(d.readPos[v]),
                d.fadingOut[v] ? "F" : "",
                static_cast<double>(d.fadeGain[v]));
            s += buf;
        }
        s += ']';
    }
    return s;
}

std::string EngineFacade::getTransportDiag() const noexcept
{
    char buf[64];
    const int64_t bs  = diagPrevBlockStart_.load(std::memory_order_relaxed);
    const int32_t n   = diagPrevBlockN_.load(std::memory_order_relaxed);
    const int64_t err = diagBlockDeltaErr_.load(std::memory_order_relaxed);
    std::snprintf(buf, sizeof(buf), "bs=%lld n=%d Δ=%lld",
        static_cast<long long>(bs), n, static_cast<long long>(err));
    return buf;
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
