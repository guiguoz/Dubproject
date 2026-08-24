#include <JuceHeader.h>
#include "engine/EngineFacade.h"
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
    // Détecter la frontière Armed→Executing : la transition quantisée vient de
    // s'exécuter → poser le signal « fin de scène » pour l'UI (timer).
    const auto prevTransState = transition_.state();
    transition_.processBlock(ts, scheduler_);
    if (prevTransState == TransitionEngine::State::Armed
        && transition_.state() == TransitionEngine::State::Executing)
    {
        sceneEndFlag_.store(true, std::memory_order_release);
        pendingTransLen_.store(0, std::memory_order_release);
    }
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
                                     ImportCallback cb, int trimStart, int trimEnd)
{
    if (slot < 0 || slot >= kMaxSlots) return;

    // Enregistrer le fichier/trim ciblés DES MAINTENANT (message thread) :
    // applyScene peut relire slotFilePath() sans attendre la fin de l'import.
    slotPath_[slot] = filePath;
    slotTrimStart_[slot] = trimStart;
    slotTrimEnd_[slot]   = trimEnd;

    std::thread([this, slot, filePath, cb, trimStart, trimEnd]() {
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

        // Rôle → AutoMix (gain staging, sends, sidechain) + détection kick.
        graph_.setSlotRole(slot, mapRole(result.role));

        // Publier le rôle analysé (fiabilité = confiance ONNX suffisante et rôle
        // non indéterminé). Lecture sur message thread via release/acquire.
        slotRoleAnalyzed_[slot]  = mapRole(result.role);
        const bool reliable = (result.roleConfidence >= 0.75f &&
                               result.role != SlotRoleV2::Unknown);
        slotRoleReliable_[slot].store(reliable, std::memory_order_release);

        // Construction du SlotPcm (conserve la stéréo si disponible).
        // Trim optionnel (coordonnées fichier) : découpe AVANT stockage pour que
        // le SlotPlayer joue exactement la région voulue par la scène.
        int start = 0;
        int end   = numFrames;
        if (trimStart >= 0) start = std::min(numFrames, trimStart);
        if (trimEnd   >= 0) end   = std::min(numFrames, trimEnd);
        if (end <= start)   end   = start + 1;
        const int numTrim = end - start;

        SlotPcm pcm;
        pcm.numChannels = readCh;
        pcm.numFrames   = numTrim;
        pcm.sampleRate  = sr;
        pcm.data.resize(static_cast<size_t>(numTrim * readCh));
        if (readCh == 1) {
            for (int i = 0; i < numTrim; ++i)
                pcm.data[static_cast<size_t>(i)] = mono[static_cast<size_t>(start + i)];
        } else {
            const float* ch0 = buf.getReadPointer(0);
            const float* ch1 = buf.getReadPointer(1);
            for (int i = 0; i < numTrim; ++i) {
                pcm.data[static_cast<size_t>(i * 2)    ] = ch0[start + i];
                pcm.data[static_cast<size_t>(i * 2 + 1)] = ch1[start + i];
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
    slotPath_[slot].clear();
    slotTrimStart_[slot] = 0;
    slotTrimEnd_[slot]   = -1;
    slotRoleAnalyzed_[slot] = SlotRole::Loop;
    slotRoleReliable_[slot].store(false, std::memory_order_release);
}

const std::string& EngineFacade::slotFilePath(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return slotPath_[0];
    return slotPath_[slot];
}

int EngineFacade::slotTrimStart(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0;
    return slotTrimStart_[slot];
}

int EngineFacade::slotTrimEnd(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return -1;
    return slotTrimEnd_[slot];
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

void EngineFacade::setSlotMixState(int slot, float gain, float pan,
                                   float width, float depth) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    mix::setSlotMixState(mixState_, slot, gain, pan, width, depth);
    graph_.slotPlayer().setGain(slot, gain);
    graph_.slotPlayer().setSpatial(slot, pan, width);
}

mix::SlotMixState EngineFacade::getSlotMixState(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    return mix::slotMixState(mixState_, slot);
}

// ─── Magic mix asynchrone (worker — étape 8 / M9) ─────────────────────────────
// Le snapshot est capturé sur le message thread (appelant) pour éviter toute
// lecture concurrente du SlotPlayer, puis processHeuristic tourne sur un thread
// dédié. À la fin, l'état persistant (mixState_) est appliqué au runtime et le
// callback est redirigé sur le message thread (JuceHeader fournit
// MessageManager::callAsync).

namespace {

// Capture le snapshot runtime du magic mix (message thread) : PCM mono par
// slot, rôles courants, scène courante (densité). Partagé entre le chemin
// heuristique et le chemin IA.
mix::MixWorkerInputs captureMixSnapshot(const SceneData& sc, AudioGraph& graph,
                                        const std::atomic<bool>* slotLoaded,
                                        float masterBpm, double sampleRate,
                                        float serumRms, float serumCentroid,
                                        float serumMidFrac, float serumHighFrac,
                                        mix::MixContentType serumContentType,
                                        const mix::MixContentType* manualOverrides,
                                        const bool* manualOverrideActive)
{
    mix::MixWorkerInputs in;
    in.masterBpm  = masterBpm;
    in.sampleRate = sampleRate;

    in.serumRms      = serumRms;
    in.serumCentroid = serumCentroid;
    in.serumMidFrac  = serumMidFrac;
    in.serumHighFrac = serumHighFrac;
    in.serumContentType = serumContentType;

    for (int s = 0; s < kMaxSlots; ++s) {
        const SlotConfig& cfg = sc.slots[s];
        in.pcm[s]    = graph.slotPlayer().getPcmSnapshot(s);
        in.loaded[s] = slotLoaded[s].load(std::memory_order_acquire);
        in.muted[s]  = graph.slotPlayer().isMuted(s);
        in.types[s]  = roleToMixType(graph.slotRole(s));
        // Override manuel utilisateur (clic droit UI) → priorité au mix.
        if (manualOverrideActive[s] && manualOverrides[s] != mix::MixContentType::OTHER)
            in.types[s] = manualOverrides[s];
        in.scene.slotActive[static_cast<std::size_t>(s)] = cfg.active && in.loaded[s];
        in.scene.slotTypes [static_cast<std::size_t>(s)] = roleToMixType(cfg.role);
        if (cfg.active && in.loaded[s])
            ++in.scene.activeCount;
    }
    in.scene.isDrop      = in.scene.activeCount >= 6;
    in.scene.isBreakdown = in.scene.activeCount <= 2;
    return in;
}

} // namespace

void EngineFacade::triggerMagicMix() noexcept
{
    if (magicMixBusy_.load(std::memory_order_acquire)) return;
    magicMixBusy_.store(true, std::memory_order_release);

    // ── Snapshot runtime (message thread) ────────────────────────────────────
    const SceneData& sc = sceneStore_.getScene(currentScene_);
    mix::MixWorkerInputs in = captureMixSnapshot(
        sc, graph_, slotLoaded_, getBpm(), sampleRate_,
        serumRms_, serumCentroid_, serumMidFrac_, serumHighFrac_,
        serumContentType_, manualOverrides_, manualOverrideActive_);

    // ── Lancement du worker ──────────────────────────────────────────────────
    std::thread([this, in = std::move(in)]() mutable {
        const mix::MixStateArray state = mix::runHeuristicMix(in);

        // Application + callback sur le message thread (l'état persistant n'est
        // jamais écrit depuis un thread de travail).
        juce::MessageManager::callAsync([this, in = std::move(in), state]() {
            for (int s = 0; s < kMaxSlots; ++s) {
                const mix::SlotMixState st = mix::slotMixState(state, s);
                if (st.applied)
                    setSlotMixState(s, st.gain, st.pan, st.width, st.depth);
                detectedTypes_[s] = in.types[s];
            }
            magicMixBusy_.store(false, std::memory_order_release);
            magicMixActive_.store(true, std::memory_order_release);
            lastMixUsedFallback_.store(true, std::memory_order_release);
            if (magicMixDoneCb_)
                magicMixDoneCb_();
        });
    }).detach();
}

void EngineFacade::triggerAiMagicMix(
    const std::array<engine::mix::MixAiDecision, 8>& decisions) noexcept
{
    if (magicMixBusy_.load(std::memory_order_acquire)) return;
    magicMixBusy_.store(true, std::memory_order_release);

    const SceneData& sc = sceneStore_.getScene(currentScene_);
    mix::MixWorkerInputs in = captureMixSnapshot(
        sc, graph_, slotLoaded_, getBpm(), sampleRate_,
        serumRms_, serumCentroid_, serumMidFrac_, serumHighFrac_,
        serumContentType_, manualOverrides_, manualOverrideActive_);

    std::thread([this, in = std::move(in), decisions]() mutable {
        const mix::MixStateArray state = mix::runAiMix(in, decisions);

        juce::MessageManager::callAsync([this, in = std::move(in), state]() {
            for (int s = 0; s < kMaxSlots; ++s) {
                const mix::SlotMixState st = mix::slotMixState(state, s);
                if (st.applied)
                    setSlotMixState(s, st.gain, st.pan, st.width, st.depth);
                detectedTypes_[s] = in.types[s];
            }
            magicMixBusy_.store(false, std::memory_order_release);
            magicMixActive_.store(true, std::memory_order_release);
            lastMixUsedFallback_.store(false, std::memory_order_release);
            if (magicMixDoneCb_)
                magicMixDoneCb_();
        });
    }).detach();
}

void EngineFacade::toggleMagicMix() noexcept
{
    if (magicMixActive_.load(std::memory_order_acquire))
        revertMagicMix();
    else
        triggerMagicMix();
}

void EngineFacade::revertMagicMix() noexcept
{
    if (magicMixBusy_.load(std::memory_order_acquire)) return;

    // Reset de l'état persistant + application au runtime (gain 1 + spatial
    // neutre). Le PCM n'est jamais modifié (invariant transparence) — aucun
    // rechargement fichier nécessaire. Synchrone : message thread uniquement.
    mix::resetMixState(mixState_);
    for (int s = 0; s < kMaxSlots; ++s) {
        graph_.slotPlayer().setGain(s, 1.f);
        graph_.slotPlayer().setSpatial(s, 0.f, 0.f);
    }
    magicMixActive_.store(false, std::memory_order_release);

    if (magicMixDoneCb_)
        magicMixDoneCb_();
}

void EngineFacade::setMagicMixDoneCallback(std::function<void()> cb) noexcept
{
    magicMixDoneCb_ = std::move(cb);
}

void EngineFacade::setSerumContext(float rms, float centroid, float midFrac,
                                   float highFrac,
                                   mix::MixContentType contentType, bool active) noexcept
{
    serumRms_      = active ? rms      : 0.f;
    serumCentroid_ = active ? centroid : 0.f;
    serumMidFrac_  = active ? midFrac  : 0.f;
    serumHighFrac_ = active ? highFrac : 0.f;
    serumContentType_ = contentType;
}

engine::mix::MixContentType EngineFacade::getDetectedType(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return mix::MixContentType::OTHER;
    const auto t = detectedTypes_[static_cast<std::size_t>(slot)];
    return (t == mix::MixContentType::OTHER) ? roleToMixType(graph_.slotRole(slot)) : t;
}

void EngineFacade::setManualTypeOverride(int slot, int typeIndex) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    if (typeIndex < 0) {
        manualOverrideActive_[slot] = false;
        return;
    }
    manualOverrideActive_[slot] = true;
    manualOverrides_[slot] = static_cast<mix::MixContentType>(typeIndex);
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

engine::SlotRole EngineFacade::slotRole(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return SlotRole::Loop;
    return slotRoleAnalyzed_[static_cast<std::size_t>(slot)];
}

bool EngineFacade::isSlotRoleReliable(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slotRoleReliable_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
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

// ─── Édition sample (waveforms, éditeur de trim) ──────────────────────────────

std::vector<float> EngineFacade::getSlotPcmSnapshot(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    return graph_.slotPlayer().getPcmSnapshot(slot);
}

float EngineFacade::getSlotPcmSampleRate(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;
    return graph_.slotPlayer().getPcmSampleRate(slot);
}

void EngineFacade::reloadSlotPcm(int slot, std::vector<float> mono, float sampleRate) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    if (mono.empty()) return;
    const PlayMode mode = graph_.slotPlayer().getMode(slot);
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = static_cast<int>(mono.size());
    pcm.sampleRate  = sampleRate;
    pcm.data        = std::move(mono);
    graph_.slotPlayer().loadSlot(slot, std::move(pcm), mode);
    slotLoaded_[slot].store(true, std::memory_order_release);
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

// ─── Transitions de scène (Tier 1 — Phase 4a) ─────────────────────────────────
// La frontière Armed→Executing est détectée dans processBlock : elle pose
// sceneEndFlag_ (consommé par l'UI) et efface pendingTransLen_.

int EngineFacade::pendingSceneIdx() const noexcept
{
    return pendingScene_.load(std::memory_order_relaxed);
}

void EngineFacade::setPendingScene(int idx) noexcept
{
    if (idx < 0 || idx >= kMaxScenes) return;
    pendingScene_.store(idx, std::memory_order_relaxed);
}

int EngineFacade::consumePendingScene() noexcept
{
    return pendingScene_.exchange(-1, std::memory_order_acq_rel);
}

bool EngineFacade::hasPendingScene() const noexcept
{
    return pendingScene_.load(std::memory_order_relaxed) >= 0;
}

bool EngineFacade::hasPendingTransition() const noexcept
{
    return pendingTransLen_.load(std::memory_order_relaxed) > 0;
}

void EngineFacade::setPendingTransitionLen(int steps) noexcept
{
    pendingTransLen_.store(steps, std::memory_order_release);
}

bool EngineFacade::consumeSceneEnd() noexcept
{
    return sceneEndFlag_.exchange(false, std::memory_order_acq_rel);
}

// ─── Morphing PingPongDelay (Tier 2 — Phase 4a) ───────────────────────────────

void EngineFacade::startDubDelayMorph(int from, int to, float durationMs) noexcept
{
    if (from < 0 || to < 0 || from >= kMaxScenes || to >= kMaxScenes) return;
    morphState_ = { true, 0.f, (durationMs > 0.f) ? durationMs : 4000.f,
                    from, to, 0 };
}

void EngineFacade::updateMorphing() noexcept
{
    if (!morphState_.active) return;
    morphState_.tickCounter += 33;
    morphState_.progress = std::clamp(
        static_cast<float>(morphState_.tickCounter) / morphState_.durationMs,
        0.f, 1.f);
    if (morphState_.progress >= 1.f)
        morphState_.active = false;
}

bool EngineFacade::isMorphing() const noexcept          { return morphState_.active; }
float EngineFacade::getMorphProgress() const noexcept   { return morphState_.progress; }
int   EngineFacade::getMorphFromSceneIdx() const noexcept { return morphState_.fromScene; }
int   EngineFacade::getMorphToSceneIdx() const noexcept   { return morphState_.toScene; }

// ─── Énergie de scène (crossfade adaptatif) ───────────────────────────────────

void EngineFacade::setSceneEnergy(int idx, float energy) noexcept
{
    if (idx < 0 || idx >= kMaxScenes) return;
    sceneEnergy_[idx] = energy;
}

float EngineFacade::getSceneEnergy(int idx) const noexcept
{
    if (idx < 0 || idx >= kMaxScenes) return 0.f;
    return sceneEnergy_[idx];
}

// ─── Patterns / sequencer (helper) ────────────────────────────────────────────

void EngineFacade::prepareStepBuffer(const StepBuf& buf) noexcept
{
    for (int s = 0; s < kMaxSlots; ++s)
    {
        TrackPattern* dst = graph_.sequencer().patterns().writeBuffer(s);
        dst->numSteps = std::clamp(buf.trackStepCount[s], 1, kMaxSteps);
        for (int i = 0; i < kMaxSteps; ++i)
            dst->steps[i] = buf.steps[s][i];
    }
    graph_.sequencer().patterns().flip();
}

void EngineFacade::stopAllSlots(StopMode /*mode*/) noexcept
{
    constexpr uint16_t kAllSlots = (1u << kMaxSlots) - 1u;
    pendingStops_.fetch_or(kAllSlots, std::memory_order_release);
}

// ─── Playhead / séquenceur (pour l'UI) ─────────────────────────────────────

int32_t EngineFacade::getCurrentStep() const noexcept
{
    const TransportState& ts = transport_.state();
    if (!ts.playing) return 0;
    return static_cast<int32_t>(stepIndexAt(ts, ts.samplePos) % kMaxSteps);
}

double EngineFacade::getCurrentPhase() const noexcept
{
    const TransportState& ts = transport_.state();
    if (!ts.playing) return 0.0;
    return beatAt(ts, ts.samplePos);
}

} // namespace engine
