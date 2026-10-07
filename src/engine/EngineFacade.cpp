#include <JuceHeader.h>
#include "engine/EngineFacade.h"
#include "engine/TransitionPolicy.h"
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
        slotRoleAuto_[s].store(SlotRole::Unknown, std::memory_order_relaxed);
    }
}

EngineFacade::~EngineFacade()
{
    // Arrêter le thread de mix avant la destruction des membres.
    // Sans ça, std::thread::~thread() appelle std::terminate() si joinable.
    stopMixThread();
    // weakRefMaster_ est automatiquement clear() par JUCE_DECLARE_WEAK_REFERENCEABLE.
}

// ─── Cycle de vie ─────────────────────────────────────────────────────────────

void EngineFacade::prepare(double sampleRate, int maxBlockSize) noexcept
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    const double bpm = (transport_.snapshot().bpm > 0.0) ? transport_.snapshot().bpm : 120.0;
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

    const auto ts = transport_.advance(numSamples);
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

    // ── Morph dub audio-safe (DIRECT) ────────────────────────────────────────
    // Progression basée uniquement sur la timeline sample, jamais sur timer.
    // Chaque bloc interpole les 4 params du delay. Serum reste hors garantie.
    if (transitionPlanValid_.load(std::memory_order_acquire) && transitionPlan_.morphActive) {
        const int64_t elapsed = ts.blockStart - transitionPlan_.morphStart;
        float progress = 0.f;
        if (elapsed >= transitionPlan_.morphDur) progress = 1.f;
        else if (elapsed > 0) progress = static_cast<float>(elapsed) / static_cast<float>(transitionPlan_.morphDur);
        auto lerp = [](float a, float b, float t){ return a + (b - a) * t; };
        auto& d = graph_.delay();
        d.setFeedback(lerp(transitionPlan_.morphFrom[0], transitionPlan_.morphTo[0], progress));
        d.setWet(lerp(transitionPlan_.morphFrom[1], transitionPlan_.morphTo[1], progress));
        d.setTone(lerp(transitionPlan_.morphFrom[2], transitionPlan_.morphTo[2], progress));
        d.setDrive(lerp(transitionPlan_.morphFrom[3], transitionPlan_.morphTo[3], progress));
    }

    graph_.sequencer().generateEvents(ts, blockStart, numSamples,
                                      swingFactor_.load(std::memory_order_relaxed), scheduler_);

    // ── Dispatch temporel : events avec offset dans le bloc ───────────────────
    // Utilise EventScheduler::processBlock() qui filtre par time et calcule
    // l'offset en samples depuis le début du bloc pour chaque event.
    // evBuf_ est un membre préalloué (pas d'allocation pile dans le callback).
    evCount_ = 0;
    scheduler_.processBlock(blockStart, numSamples,
        [this](int32_t offset, const EngineEvent& ev) {
            if (evCount_ < kMaxEvBuf) {
                evBuf_[evCount_].offset = offset;
                evBuf_[evCount_].ev     = ev;
                ++evCount_;
            }
        });

    // ── Flush delay si stop demandé depuis message thread ────────────────────
    if (delayResetPending_.exchange(false, std::memory_order_acq_rel))
        graph_.delay().reset();

    // ── AudioGraph ────────────────────────────────────────────────────────────
    std::fill(interleavedOut_.begin(),
              interleavedOut_.begin() + numSamples * 2, 0.f);
    graph_.processBlock(ts, evBuf_, evCount_, interleavedOut_.data(), numSamples,
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

    // ── Instrumentation diagnostics (temporaire — stopped-nav silence) ─────────
    if (diagState_.load(std::memory_order_relaxed) == 1) {
        for (int i = 0; i < evCount_; ++i) {
            const int s = static_cast<int>(evBuf_[i].ev.slot);
            if (s < 0 || s >= kMaxSlots) continue;
            switch (evBuf_[i].ev.type) {
                case EventType::Trigger:
                    diagGen_[s].fetch_add(1, std::memory_order_relaxed);
                    if (graph_.slotPlayer().isLoaded(s)) {
                        const int32_t hdl = diagHdl_[s].fetch_add(1, std::memory_order_relaxed);
                        if (hdl == 0) {  // premier trigger de ce slot dans la fenêtre
                            const float rv = graph_.slotPlayer().getRampValue(s);
                            const float gn = graph_.slotPlayer().getGain(s);
                            diagRampValueSnap_[s].store(rv, std::memory_order_relaxed);
                            diagEffGainSnap_  [s].store(rv * gn, std::memory_order_relaxed);
                            diagPcmFrames_    [s].store(graph_.slotPlayer().getPcmFrames(s), std::memory_order_relaxed);
                            diagPcmNull_      [s].store(graph_.slotPlayer().isPcmEmpty(s) ? 1 : 0, std::memory_order_relaxed);
                            diagPcmMaxAbs_    [s].store(graph_.slotPlayer().getPcmMaxAbsFirst64(s), std::memory_order_relaxed);
                            diagVoiceCount_   [s].store(graph_.slotPlayer().getActiveVoiceCount(s), std::memory_order_relaxed);
                        }
                    }
                    break;
                case EventType::Release:
                    diagReleaseCount_[s].fetch_add(1, std::memory_order_relaxed);
                    break;
                case EventType::PcmFlip:
                    pushTrace(TraceEvt::Op::CommitPcm, s,
                              graph_.slotPlayer().getPcmFrames(s));
                    break;
                default:
                    break;
            }
        }
        for (int s = 0; s < kMaxSlots; ++s) {
            const float p = graph_.slotPlayer().getSlotPeak(s);
            if (p > diagPeak_[s].load(std::memory_order_relaxed))
                diagPeak_[s].store(p, std::memory_order_relaxed);
        }
        const float mp = masterRms_.load(std::memory_order_relaxed);
        if (mp > diagMasterPeak_.load(std::memory_order_relaxed))
            diagMasterPeak_.store(mp, std::memory_order_relaxed);
        const int left = diagBlocksLeft_.fetch_sub(1, std::memory_order_relaxed);
        if (left <= 1)
            diagState_.store(2, std::memory_order_release);
    }

    // ── CPU load ──────────────────────────────────────────────────────────────
    const float elapsed = std::chrono::duration<float>(Clock::now() - t0).count();
    const float budget  = static_cast<float>(numSamples) / static_cast<float>(sampleRate_);
    if (budget > 0.f)
        cpuLoad_.store(elapsed / budget * 100.f, std::memory_order_relaxed);
}

// ─── Transport ────────────────────────────────────────────────────────────────

// ─── Snapshot état de transition (indicateur UI) ─────────────────────────────

TransitionStatusSnapshot EngineFacade::getTransitionStatusSnapshot(int selectedScene) const noexcept
{
    TransitionStatusSnapshot sn;
    sn.selectedScene  = selectedScene;
    sn.runtimeScene   = currentScene_.load(std::memory_order_relaxed);
    sn.timeSigNum     = 4;
    sn.timeSigDen     = 4;
    sn.pendingScene   = -1;
    sn.boundarySample = -1;
    sn.policy         = 0;

    const auto ts = transport_.snapshot();
    sn.sampleRate = ts.sampleRate;
    sn.bpm        = ts.bpm;
    sn.playing    = ts.playing;
    sn.nowSample  = ts.playing ? ts.samplePos : 0LL;
    sn.state      = static_cast<uint8_t>(transition_.state());

    // Priorité 1 : plan DIRECT armé (PREPARE→COMMIT path)
    if (transitionPlanValid_.load(std::memory_order_acquire) && transitionPlan_.valid)
    {
        sn.pendingScene   = transitionPlan_.toScene;
        sn.boundarySample = transitionPlan_.boundary;
        sn.policy         = transitionPlan_.policy;
        sn.dubActive      = transitionPlan_.dubActive;
    }
    // Priorité 2 : transition quantisée (TransitionEngine morph)
    else if (transition_.state() != TransitionEngine::State::Idle)
    {
        const auto& plan = transition_.plan();
        if (plan.valid && plan.toScene >= 0)
        {
            sn.pendingScene   = plan.toScene;
            sn.boundarySample = plan.executionSample;
            switch (plan.type)
            {
                case TransitionType::Build:
                case TransitionType::Dub:
                    sn.policy = 1; break;  // BUILD
                case TransitionType::Breakdown:
                    sn.policy = 2; break;  // BREAKDOWN
                default:
                    sn.policy = 0; break;  // DIRECT / Smooth / Cut
            }
        }
    }
    // Priorité 3 : legacy pending (index sans timing)
    else
    {
        const int legacy = pendingScene_.load(std::memory_order_relaxed);
        if (legacy >= 0)
        {
            sn.pendingScene   = legacy;
            sn.boundarySample = -1;
            sn.policy         = 0;
        }
    }

    return sn;
}

// ─── Instrumentation diagnostics ─────────────────────────────────────────────

int EngineFacade::transitionStateRaw() const noexcept {
    return static_cast<int>(transition_.state());
}

void EngineFacade::diagStart() noexcept {
    for (int s = 0; s < kMaxSlots; ++s) {
        diagGen_[s].store(0, std::memory_order_relaxed);
        diagHdl_[s].store(0, std::memory_order_relaxed);
        diagPeak_[s].store(0.f, std::memory_order_relaxed);
        // S-C extended
        diagVoiceCount_   [s].store(0,   std::memory_order_relaxed);
        diagReleaseCount_ [s].store(0,   std::memory_order_relaxed);
        diagRampValueSnap_[s].store(0.f, std::memory_order_relaxed);
        diagEffGainSnap_  [s].store(0.f, std::memory_order_relaxed);
        diagPcmFrames_    [s].store(0,   std::memory_order_relaxed);
        diagPcmNull_      [s].store(0,   std::memory_order_relaxed);
        diagPcmMaxAbs_    [s].store(0.f, std::memory_order_relaxed);
    }
    diagMasterPeak_.store(0.f, std::memory_order_relaxed);
    diagBlocksLeft_.store(8, std::memory_order_relaxed);
    diagState_.store(1, std::memory_order_release);
}

bool EngineFacade::diagIsReady() const noexcept {
    return diagState_.load(std::memory_order_acquire) == 2;
}

EngineFacade::DiagCounters EngineFacade::diagReadAndClear() noexcept {
    DiagCounters d;
    for (int s = 0; s < kMaxSlots; ++s) {
        d.generated[s] = diagGen_[s].load(std::memory_order_relaxed);
        d.handled  [s] = diagHdl_[s].load(std::memory_order_relaxed);
        d.slotPeak [s] = diagPeak_[s].load(std::memory_order_relaxed);
        d.loaded   [s] = graph_.slotPlayer().isLoaded(s);
        d.muted    [s] = graph_.slotPlayer().isMuted(s);
        d.gain     [s] = graph_.slotPlayer().getGain(s);
        // S-C extended
        d.voiceCount   [s] = diagVoiceCount_   [s].load(std::memory_order_relaxed);
        d.releaseCount [s] = diagReleaseCount_ [s].load(std::memory_order_relaxed);
        d.rampValue    [s] = diagRampValueSnap_[s].load(std::memory_order_relaxed);
        d.effectiveGain[s] = diagEffGainSnap_  [s].load(std::memory_order_relaxed);
        d.pcmFrames    [s] = diagPcmFrames_    [s].load(std::memory_order_relaxed);
        d.pcmNull      [s] = diagPcmNull_      [s].load(std::memory_order_relaxed) != 0;
        d.pcmMaxAbs    [s] = diagPcmMaxAbs_    [s].load(std::memory_order_relaxed);
    }
    d.masterPeak         = diagMasterPeak_.load(std::memory_order_relaxed);
    d.teState            = transitionStateRaw();
    d.hasPending         = hasPendingTransition();
    d.pendingStopSnapshot = pendingStops_.load(std::memory_order_relaxed);
    for (int s = 0; s < kMaxSlots; ++s) {
        d.autoMixGain    [s] = graph_.autoMix().currentGainLinear(s);
        d.autoMixTargetDb[s] = graph_.autoMix().targets().gainDb[s];
    }

    // Dump trace ring (D) — derniers kTraceRingCap events writers
    const int head = traceHead_.load(std::memory_order_relaxed);
    const int num  = std::min(head, kTraceRingCap);
    if (num > 0) {
        juce::Logger::writeToLog("[DIAG] TRACE writers (" + juce::String(num) + " derniers) :");
        static constexpr const char* kOpNames[] =
            {"None","ClearSlot","StopSlot","LoadSlot","ImportStart","StagePcm","CommitPcm"};
        const int start = head - num;
        for (int n = 0; n < num; ++n) {
            const auto& ev = traceRing_[(start + n) & (kTraceRingCap - 1)];
            const int opIdx = static_cast<int>(ev.op);
            const char* name = (opIdx >= 0 && opIdx < 7) ? kOpNames[opIdx] : "?";
            juce::String line = "[DIAG]   [b=" + juce::String(ev.block)
                + "] " + juce::String(name) + " slot=" + juce::String(ev.slot);
            if (ev.frames > 0)
                line += " frames=" + juce::String(ev.frames);
            if (ev.assetLo > 0)
                line += " asset=0x" + juce::String::toHexString(ev.assetLo);
            juce::Logger::writeToLog(line);
        }
    }

    diagState_.store(0, std::memory_order_relaxed);
    return d;
}

void EngineFacade::pushTrace(TraceEvt::Op op, int slot, int frames, uint32_t assetLo) noexcept {
    const int i = traceHead_.fetch_add(1, std::memory_order_relaxed) & (kTraceRingCap - 1);
    traceRing_[i] = {op, static_cast<uint8_t>(slot >= 0 ? slot : 0),
                     static_cast<uint32_t>(audioBlockCounter_.load(std::memory_order_relaxed)),
                     frames, assetLo};
}

// ─── Transport ────────────────────────────────────────────────────────────────

void EngineFacade::play() noexcept {
    juce::Logger::writeToLog("[DIAG] play() scene=" + juce::String(currentScene_.load())
        + " hasPend=" + juce::String((int)hasPendingTransition())
        + " teState=" + juce::String(transitionStateRaw()));
    diagStart();
    graph_.sequencer().clearStaged();          // stage obsolète au redémarrage
    graph_.sequencer().resetPatternPhase();    // transport repart de 0 → base à 0
    transport_.play();
}

void EngineFacade::stop() noexcept {
    transport_.stop();
    graph_.sequencer().clearStaged();   // transport figé : la frontière ne viendra pas
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
// restoredRole != Unknown → projet restauré : utilise le rôle sauvegardé pour
// la décision OneShot/Free, mais garde autoLoopSync du BPM (toujours analysé).
static PlayMode modeForResult(const AnalysisResult& r, int slot,
                              SlotRole restoredRole = SlotRole::Unknown) noexcept
{
    if (r.autoLoopSync)
        return PlayMode::LoopSync;

    // Slots percussifs sémantiques fixes (CLAUDE.md) : KCK=2, SNR=3, HAT=4, PRC=7
    const bool slotPercussive = (slot == 2 || slot == 3 || slot == 4 || slot == 7);

    if (restoredRole != SlotRole::Unknown) {
        // Projet restauré : décision basée sur le rôle sauvegardé (SlotRole domain)
        const bool restored_percussive = (restoredRole == SlotRole::Kick  ||
                                          restoredRole == SlotRole::Snare ||
                                          restoredRole == SlotRole::Perc  ||
                                          restoredRole == SlotRole::Unknown);
        return (restored_percussive || slotPercussive) ? PlayMode::OneShot : PlayMode::Free;
    }

    const bool rolePercussive = (r.role == SlotRoleV2::Kick   ||
                                 r.role == SlotRoleV2::Snare  ||
                                 r.role == SlotRoleV2::HiHat  ||
                                 r.role == SlotRoleV2::Perc   ||
                                 r.role == SlotRoleV2::Unknown);

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
        default:                  return SlotRole::Unknown; // Unknown → traitement neutre
    }
}

void EngineFacade::importSampleAsync(int slot, const std::string& filePath,
                                     ImportCallback cb, int trimStart, int trimEnd,
                                     SlotRole restoredRole, bool wasManual)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    // Incrémenter la génération avant de lancer : invalide tout worker précédent pour ce slot.
    slotGen_[slot].fetch_add(1, std::memory_order_release);
    const int32_t capturedGen = slotGen_[slot].load(std::memory_order_relaxed);
    pushTrace(TraceEvt::Op::ImportStart, slot);

    // Enregistrer le fichier/trim ciblés DES MAINTENANT (message thread) :
    // applyScene peut relire slotFilePath() sans attendre la fin de l'import.
    slotPath_[slot] = filePath;
    slotTrimStart_[slot] = trimStart;
    slotTrimEnd_[slot]   = trimEnd;

    juce::WeakReference<EngineFacade> weakThis(this);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::thread([weakThis, slot, filePath, cb, trimStart, trimEnd,
                 restoredRole, wasManual, alive, capturedGen]() {
        juce::AudioFormatManager fmtMgr;
        fmtMgr.registerBasicFormats();

        const auto file = juce::File(juce::String(filePath));
        std::unique_ptr<juce::AudioFormatReader> reader(fmtMgr.createReaderFor(file));
        if (!reader) {
            if (auto* self = weakThis.get(); self && cb) cb(slot, AnalysisResult{});
            return;
        }

        const int   numCh     = static_cast<int>(reader->numChannels);
        const int   numFrames = static_cast<int>(reader->lengthInSamples);
        const float sr        = static_cast<float>(reader->sampleRate);

        if (numFrames <= 0) {
            if (auto* self = weakThis.get(); self && cb) cb(slot, AnalysisResult{});
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
        // getBpm() nécessite self ; on ne peut pas analyser si self détruit.
        auto* selfForAnalyze = weakThis.get();
        if (!selfForAnalyze || !alive->load(std::memory_order_acquire)) return;
        const float projectBpm = selfForAnalyze->getBpm();

        // Toujours analyser BPM+key (pour modeForResult/autoLoopSync).
        // Si restoredRole != Unknown (projet chargé) : la classification de rôle
        // est court-circuitée — on passe filePath="" pour qu'analyzeSync
        // n'exécute pas le mapping nom+ONNX inutilement.
        const std::string analyzeFilePath = (restoredRole == SlotRole::Unknown)
                                            ? filePath : std::string{};
        AnalysisResult result = selfForAnalyze->importPipeline_.analyzeSync(
            mono.data(), numFrames, 1, sr, projectBpm, analyzeFilePath);

        // Si l'objet a été détruit pendant l'analyse, abandonner.
        auto* self = weakThis.get();
        if (!self || !alive->load(std::memory_order_acquire)) return;

        // Déterminer le rôle final : projet restauré → restoredRole ; nouveau fichier → ONNX/nom.
        const SlotRole finalRole = (restoredRole != SlotRole::Unknown)
                                   ? restoredRole
                                   : mapRole(result.role);

        // Rôle → AutoMix (gain staging, sends, sidechain) + détection kick.
        self->graph_.setSlotRole(slot, finalRole);

        // Publier le rôle + fiabilité (release/acquire — lu sur message thread).
        self->slotRoleAnalyzed_[slot].store(finalRole, std::memory_order_release);
        const bool reliable = (restoredRole != SlotRole::Unknown) ||
                              (result.roleConfidence >= 0.75f &&
                               result.role != SlotRoleV2::Unknown);
        self->slotRoleReliable_[slot].store(reliable, std::memory_order_release);
        self->isRoleManual_[slot] = (restoredRole != SlotRole::Unknown) && wasManual;
        // Mémoriser le rôle AUTO (jamais écrasé par un forçage manuel ultérieur).
        // Rôle restauré manuel (projet) : pas de valeur auto à retenir.
        if (!((restoredRole != SlotRole::Unknown) && wasManual)) {
            self->slotRoleAuto_[slot].store(finalRole, std::memory_order_release);
            self->slotRoleAutoReliable_[slot].store(reliable, std::memory_order_release);
        }

        // Construction du SlotPcm (conserve la stéréo si disponible).
        // Trim optionnel (coordonnées fichier) : découpe AVANT stockage pour que
        // le SlotPlayer joue exactement la région voulue par la scène.
        int start = 0;
        int end   = numFrames;
        if (trimStart >= 0) start = std::min(numFrames, trimStart);
        if (trimEnd   >= 0) end   = std::min(numFrames, trimEnd);
        if (end <= start)   end   = numFrames;  // trim invalide → tout le fichier
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
        // Mutex par slot : sérialise stopSlot → loadSlot pour éviter
        // que deux imports simultanés sur le même slot corrompent le PCM.
        {
            auto* selfLocked = weakThis.get();
            if (!selfLocked || !alive->load(std::memory_order_acquire)) return;
            std::lock_guard<std::mutex> lock(selfLocked->importSlotMutex_[slot]);
            // Re-vérifier après prise du verrou (weakThis peut avoir expiré).
            selfLocked = weakThis.get();
            if (!selfLocked || !alive->load(std::memory_order_acquire)) return;
            selfLocked->stopSlot(slot, true);
            const int64_t targetBlock = selfLocked->audioBlockCounter_.load(std::memory_order_acquire) + 3;
            while (selfLocked->audioBlockCounter_.load(std::memory_order_acquire) < targetBlock) {
                if (weakThis.wasObjectDeleted()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // Guard génération : si clearSlot ou activateSceneStopped a incrémenté
            // slotGen_ pendant notre analyse, ce worker est périmé → abort.
            if (selfLocked->slotGen_[slot].load(std::memory_order_acquire) != capturedGen)
                return;

            // Chargement dans le SlotPlayer (worker thread — pas audio)
            const PlayMode mode = modeForResult(result, slot, restoredRole);
            selfLocked->graph_.slotPlayer().loadSlot(slot, std::move(pcm), mode);
            selfLocked->slotLoaded_[slot].store(true, std::memory_order_release);
            const int   loadedFrames = selfLocked->graph_.slotPlayer().getPcmFrames(slot);
            const AssetId loadedAsset = assetIdFor(selfLocked->slotPath_[slot],
                                                    selfLocked->slotTrimStart_[slot],
                                                    selfLocked->slotTrimEnd_[slot]);
            selfLocked->pushTrace(TraceEvt::Op::LoadSlot, slot, loadedFrames,
                                  static_cast<uint32_t>(loadedAsset));
        }

        if (auto* selfCb = weakThis.get(); selfCb && cb) cb(slot, result);
    }).detach();
}

void EngineFacade::clearSlot(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slotGen_[slot].fetch_add(1, std::memory_order_release);  // invalide workers en vol
    pushTrace(TraceEvt::Op::ClearSlot, slot);
    graph_.slotPlayer().clearSlot(slot);
    slotLoaded_[slot].store(false, std::memory_order_relaxed);
    slotPath_[slot].clear();
    slotTrimStart_[slot] = 0;
    slotTrimEnd_[slot]   = -1;
    slotRoleAnalyzed_[slot].store(SlotRole::Unknown, std::memory_order_relaxed);
    slotRoleReliable_[slot].store(false, std::memory_order_release);
    isRoleManual_[slot] = false;
    slotRoleAuto_[slot].store(SlotRole::Unknown, std::memory_order_relaxed);
    slotRoleAutoReliable_[slot].store(false, std::memory_order_relaxed);
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
    pushTrace(TraceEvt::Op::StopSlot, slot);
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
    const SceneData& sc = sceneStore_.getScene(currentScene_.load(std::memory_order_relaxed));
    mix::MixWorkerInputs in = captureMixSnapshot(
        sc, graph_, slotLoaded_, getBpm(), sampleRate_,
        serumRms_, serumCentroid_, serumMidFrac_, serumHighFrac_,
        serumContentType_, manualOverrides_, manualOverrideActive_);

    // ── Lancement du worker ──────────────────────────────────────────────────
    // ref créé sur le message thread (avant detach) pour éviter la race entre
    // runHeuristicMix() et ~EngineFacade() lors de la construction de WeakReference.
    juce::WeakReference<EngineFacade> ref(this);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::thread([ref, in = std::move(in), alive]() mutable {
        const mix::MixStateArray state = mix::runHeuristicMix(in);

        // Application + callback sur le message thread (l'état persistant n'est
        // jamais écrit depuis un thread de travail).
        juce::MessageManager::callAsync([ref, alive, in = std::move(in), state]() {
            if (!alive->load(std::memory_order_acquire)) return;
            auto* self = ref.get();
            if (!self) return;
            for (int s = 0; s < kMaxSlots; ++s) {
                const mix::SlotMixState st = mix::slotMixState(state, s);
                if (st.applied)
                    self->setSlotMixState(s, st.gain, st.pan, st.width, st.depth);
                self->detectedTypes_[s] = in.types[s];
            }
            self->magicMixBusy_.store(false, std::memory_order_release);
            self->magicMixActive_.store(true, std::memory_order_release);
            self->lastMixUsedFallback_.store(true, std::memory_order_release);
            if (self->magicMixDoneCb_)
                self->magicMixDoneCb_();
        });
    }).detach();
}

void EngineFacade::triggerAiMagicMix(
    const std::array<engine::mix::MixAiDecision, 8>& decisions) noexcept
{
    if (magicMixBusy_.load(std::memory_order_acquire)) return;
    magicMixBusy_.store(true, std::memory_order_release);

    const SceneData& sc = sceneStore_.getScene(currentScene_.load(std::memory_order_relaxed));
    mix::MixWorkerInputs in = captureMixSnapshot(
        sc, graph_, slotLoaded_, getBpm(), sampleRate_,
        serumRms_, serumCentroid_, serumMidFrac_, serumHighFrac_,
        serumContentType_, manualOverrides_, manualOverrideActive_);

    juce::WeakReference<EngineFacade> ref(this);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::thread([ref, in = std::move(in), decisions, alive]() mutable {
        const mix::MixStateArray state = mix::runAiMix(in, decisions);

        juce::MessageManager::callAsync([ref, alive, in = std::move(in), state]() {
            if (!alive->load(std::memory_order_acquire)) return;
            auto* self = ref.get();
            if (!self) return;
            for (int s = 0; s < kMaxSlots; ++s) {
                const mix::SlotMixState st = mix::slotMixState(state, s);
                if (st.applied)
                    self->setSlotMixState(s, st.gain, st.pan, st.width, st.depth);
                self->detectedTypes_[s] = in.types[s];
            }
            self->magicMixBusy_.store(false, std::memory_order_release);
            self->magicMixActive_.store(true, std::memory_order_release);
            self->lastMixUsedFallback_.store(false, std::memory_order_release);
            if (self->magicMixDoneCb_)
                self->magicMixDoneCb_();
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
    return slotRoleAnalyzed_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
}

bool EngineFacade::isSlotRoleReliable(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return slotRoleReliable_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
}

void EngineFacade::setSlotRoleManual(int slot, SlotRole role) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    graph_.setSlotRole(slot, role);
    slotRoleAnalyzed_[slot].store(role, std::memory_order_release);
    slotRoleReliable_[slot].store(true, std::memory_order_release);
    isRoleManual_[slot] = true;
}

bool EngineFacade::isSlotRoleManual(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    return isRoleManual_[slot];
}

engine::SlotRoleInfo EngineFacade::getSlotRoleInfo(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return {};
    if (!isSlotLoaded(slot)) return {};   // slot vide → neutre, pas de rôle à afficher
    const auto idx = static_cast<std::size_t>(slot);
    return { graph_.slotRole(slot), isRoleManual_[idx] };
}

// Entrée du menu rôle (0-8) → rôle effectif. Miroir de mapRole() pour le
// sens inverse (le menu parle ContentType, le moteur parle SlotRole).
static SlotRole contentTypeToRole(mix::MixContentType ct) noexcept
{
    using MT = mix::MixContentType;
    switch (ct) {
        case MT::KICK:  return SlotRole::Kick;
        case MT::SNARE: return SlotRole::Snare;
        case MT::HIHAT: return SlotRole::Perc;
        case MT::BASS:  return SlotRole::Bass;
        case MT::SYNTH: return SlotRole::Melodic;
        case MT::PAD:   return SlotRole::Pad;
        case MT::PERC:  return SlotRole::Perc;
        case MT::LOOP:  return SlotRole::Loop;
        default:        return SlotRole::Fx;   // OTHER → Fx (roleToMixType(Fx)==OTHER)
    }
}

void EngineFacade::setSlotRoleChoice(int slot, int typeIndex) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    const auto idx = static_cast<std::size_t>(slot);
    if (typeIndex < 0 || typeIndex > 8) {
        // Auto : retire les overrides, restaure le dernier rôle auto analysé
        // (aucune reclassification — slotRoleAuto_ n'est jamais écrasé par manuel).
        manualOverrideActive_[idx] = false;
        isRoleManual_[idx] = false;
        const SlotRole autoRole = slotRoleAuto_[idx].load(std::memory_order_acquire);
        graph_.setSlotRole(slot, autoRole);
        slotRoleAnalyzed_[idx].store(autoRole, std::memory_order_release);
        slotRoleReliable_[idx].store(slotRoleAutoReliable_[idx].load(std::memory_order_acquire),
                                     std::memory_order_release);
        return;
    }
    // Force : override mix (comportement historique) + rôle effectif manuel.
    setManualTypeOverride(slot, typeIndex);
    setSlotRoleManual(slot, contentTypeToRole(static_cast<mix::MixContentType>(typeIndex)));
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
    const int oldSteps = writePatterns_[track].numSteps;
    const int newSteps = bars * 16;
    trackBars_[track]              = bars;
    writePatterns_[track].numSteps = newSteps;
    if (newSteps > oldSteps && oldSteps > 0)
        for (int i = oldSteps; i < newSteps && i < kMaxSteps; ++i)
            writePatterns_[track].steps[i] = writePatterns_[track].steps[i % oldSteps];
}

int EngineFacade::getTrackBarCount(int track) const noexcept
{
    if (track < 0 || track >= kMaxSlots) return 1;
    return trackBars_[track];
}

void EngineFacade::flipPatternBuffer() noexcept
{
    for (int s = 0; s < kMaxSlots; ++s)
        *graph_.sequencer().patterns().writeBuffer(s) = writePatterns_[s];
    graph_.sequencer().patterns().publish();
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
    graph_.sequencer().clearStaged();          // application directe : annule le stage
    graph_.sequencer().resetPatternPhase();    // navigation → transport figé, phase à 0
    currentScene_.store(idx, std::memory_order_relaxed);
    applySceneInternal(idx);
}

void EngineFacade::requestTransition(int toScene) noexcept
{
    if (toScene < 0 || toScene >= kMaxScenes) return;
    transition_.requestTransition(currentScene_, toScene,
                                   sceneStore_, transport_.snapshot());
}

bool EngineFacade::cancelPendingTransition() noexcept
{
    if (!transitionPlanValid_.load(std::memory_order_acquire) || !transitionPlan_.valid)
        return false;

    // Trop tard : le COMMIT audio a déjà eu lieu (Armed → Executing).
    if (transition_.state() != TransitionEngine::State::Armed)
        return false;

    const int from = transitionPlan_.fromScene;
    const int to   = transitionPlan_.toScene;

    // 1. Stage pattern + gates ENTER — NE PAS toucher phaseBase_ (invariant D1).
    graph_.sequencer().clearStaged();

    // 2. PCM stagés : évite qu'un futur plan committe un PCM d'une préparation abandonnée (H1/H2).
    for (int s = 0; s < kMaxSlots; ++s)
        graph_.slotPlayer().clearStagedPcm(s);

    // 3. Restauration exacte des valeurs pré-ARM pour les slots ENTER.
    for (int s = 0; s < kMaxSlots; ++s) {
        if (transitionPlan_.slots[s].action != SlotPlanAction::Enter) continue;
        slotPath_[s]      = preArmPath_[s];
        slotTrimStart_[s] = preArmTrimStart_[s];
        slotTrimEnd_[s]   = preArmTrimEnd_[s];
    }

    // 4. Plan + machine à états + flags UI-side.
    clearTransitionPlan();
    transition_.reset();
    setPendingTransitionLen(0);
    setPendingScene(-1);

    transitionTrace_.push({TraceEntry::Type::Cancelled, -1,
                           static_cast<int32_t>(from), static_cast<int32_t>(to),
                           255, 0.f, 0.f});
    return true;
}

bool EngineFacade::canPrepareDirectPlan(int fromIdx, int toIdx) noexcept
{
    if (fromIdx < 0 || fromIdx >= kMaxScenes || toIdx < 0 || toIdx >= kMaxScenes) return false;
    if (fromIdx == toIdx) return false;

    const SceneData& from = sceneStore_.getScene(fromIdx);
    const SceneData& to   = sceneStore_.getScene(toIdx);

    for (int s = 0; s < kMaxSlots; ++s) {
        const SlotConfig& a = from.slots[s];
        const SlotConfig& b = to.slots[s];
        const SlotPlanAction action = classifySlotForPlan(a, b,
                                                          assetIdForSlot(a),
                                                          assetIdForSlot(b));
        if (action != SlotPlanAction::Enter) continue;

        const AssetId targetAsset = assetIdForSlot(b);
        if (targetAsset == 0) return false;

        // Vérifie que le PCM stagé correspond bien à la cible (pas un reste du plan B annulé).
        const bool stagedForTarget = graph_.slotPlayer().hasStagedPcm(s)
                                  && transitionPlan_.slots[s].asset == targetAsset;
        if (stagedForTarget) continue;

        const AssetId cur = assetIdFor(slotPath_[s], slotTrimStart_[s], slotTrimEnd_[s]);
        if (cur == targetAsset && slotLoaded_[s].load(std::memory_order_acquire)) continue;

        const juce::File f(juce::String(b.filePath));
        if (!f.existsAsFile() || f.getSize() <= 0) return false;
    }
    return true;
}

// DETTE PREPARE synchrone — documentée, non migrée ce milestone :
// prepareDirectPlan fait actuellement du file I/O JUCE synchrone sur le
// message thread (AudioFormatManager::createReaderFor + read). Sur des
// ENTER nombreux/volumineux, cela peut freezer l'UI quelques dizaines de ms.
// L'architecture future doit être PREPARING(worker)→READY→ARMED : le worker
// décode/stage en arrière-plan, READY est posé quand tous les ENTER sont
// préchargés, ARM n'est autorisé que depuis READY. Tant qu'aucun freeze
// n'est constaté en usage réel (scènes 2→6→2, fichiers < 30s), on garde le
// synchrone pour sa simplicité et sa déterminisme de test. Ne migrer que sur
// freeze avéré (mesure > 50 ms sur message thread).
bool EngineFacade::prepareDirectPlan(int fromIdx, int toIdx) noexcept
{
    if (fromIdx < 0 || fromIdx >= kMaxScenes || toIdx < 0 || toIdx >= kMaxScenes) return false;
    if (fromIdx == toIdx) return false;
    if (transition_.state() != TransitionEngine::State::Idle) return false;
    const SceneData& from = sceneStore_.getScene(fromIdx);
    const SceneData& to   = sceneStore_.getScene(toIdx);
    // Frontière = FIN DU CYCLE de la scène courante (et non « prochaine mesure ») :
    // le pattern de A va au bout de son cycle, puis le plan COMMIT au même sample.
    // La scène entrante est recalée sur son step 0 à cette frontière (stageForBoundary
    // 3 args), donc elle repart du début de son pattern même si les deux cycles
    // ont des longueurs différentes (A = 4 mesures → B = 3 mesures, etc.).
    const TransportState tsSnap = transport_.snapshot();
    TransitionBoundary tb = planTransitionBoundary(tsSnap, from);
    int64_t boundary     = tb.sample;
    int64_t boundaryStep = tb.step;
    const int cycleSteps = tb.cycleSteps;
    SceneTransitionPlan plan;
    buildDirectPlan(from, to, fromIdx, toIdx, boundary, sampleRate_, plan);
    if (!plan.valid) return false;
    transitionTrace_.push({TraceEntry::Type::Prepared, boundary, fromIdx, toIdx, 255, 0.f, 0.f});

    // Policy pure : ajuste les atSample (BUILD/BREAKDOWN) sans toucher au DSP
    {
        PolicyContext pctx{ sampleRate_, tsSnap.bpm, 4, 4, boundary };
        // D2 (Option A) : shiftSlotEvents() clamp tout event avant boundary → guard obsolète
        PolicyType ptype = TransitionPolicy::choose(plan);
        TransitionPolicy::apply(plan, pctx);
        plan.policy = static_cast<uint8_t>(ptype);
    }

    // DUB modifier : après policy (les atSample LEAVE sont définitifs)
    {
        const double spstep = transport_.snapshot().samplesPerStep;
        applyDubModifier(plan, from, to,
                         static_cast<float>(sampleRate_), spstep, dubMode_);
    }

    // Gate d'activation ENTER : avant PcmFlip, les triggers B pour ce slot sont ignorés
    // (évite double trigger / pré-écho entre T et activation). KEEP/MORPH/LEAVE restent actifs.
    for (int s = 0; s < kMaxSlots; ++s) {
        if (plan.slots[s].action == SlotPlanAction::Enter) {
            int64_t activateAt = plan.boundary;
            for (int i = 0; i < plan.numEvents; ++i) if (plan.events[i].slot == s && plan.events[i].type == PlanEventType::PcmFlip) { activateAt = plan.events[i].atSample; break; }
            graph_.sequencer().setSlotActiveAt(s, activateAt);
        } else {
            graph_.sequencer().clearSlotActiveAt(s);
        }
    }

    // Readiness gate : tous les ENTER doivent être préchargés
    for (int s = 0; s < kMaxSlots; ++s) {
        if (plan.slots[s].action != SlotPlanAction::Enter) continue;
        // H1/H2 : purge un éventuel PCM stagé d'une préparation abandonnée avant
        // de re-précharger — sinon il serait committé pour la mauvaise cible.
        if (graph_.slotPlayer().hasStagedPcm(s))
            graph_.slotPlayer().clearStagedPcm(s);
        const SlotConfig& cfg = to.slots[s];
        const AssetId targetAsset = assetIdForSlot(cfg);
        if (targetAsset == 0) return false; // fichier manquant → pas d'ARM
        // Déjà chargé avec bon asset ?
        const AssetId curAsset = assetIdFor(slotPath_[s], slotTrimStart_[s], slotTrimEnd_[s]);
        if (curAsset == targetAsset && slotLoaded_[s].load(std::memory_order_acquire)) continue;
        // Essayer précharge synchrone depuis fichier
        // Si fichier n'existe pas ou lecture échoue → pas d'ARM (pas de demi-scène)
        juce::AudioFormatManager fmtMgr;
        fmtMgr.registerBasicFormats();
        const auto file = juce::File(juce::String(cfg.filePath));
        if (!file.existsAsFile()) return false;
        std::unique_ptr<juce::AudioFormatReader> reader(fmtMgr.createReaderFor(file));
        if (!reader || reader->lengthInSamples <= 0 || reader->numChannels <= 0) return false;
        const int numFrames = static_cast<int>(reader->lengthInSamples);
        const int numCh = static_cast<int>(reader->numChannels);
        const float sr = static_cast<float>(reader->sampleRate);
        juce::AudioBuffer<float> buf(numCh, numFrames);
        if (!reader->read(&buf, 0, numFrames, 0, true, numCh > 1)) return false;
        // Trim
        int start = std::clamp(cfg.trimStart, 0, numFrames);
        int end = (cfg.trimEnd > 0) ? std::clamp(cfg.trimEnd, 0, numFrames) : numFrames;
        if (end <= start) end = start + 1;
        const int trimFrames = end - start;
        SlotPcm pcm;
        pcm.numChannels = std::min(numCh, 2);
        pcm.numFrames = trimFrames;
        pcm.sampleRate = sr;
        pcm.data.reserve(static_cast<size_t>(trimFrames * pcm.numChannels));
        if (pcm.numChannels == 1) {
            const float* ch0 = buf.getReadPointer(0);
            pcm.data.assign(ch0 + start, ch0 + end);
        } else {
            pcm.data.resize(static_cast<size_t>(trimFrames * 2));
            const float* ch0 = buf.getReadPointer(0);
            const float* ch1 = buf.getReadPointer(1);
            for (int i = 0; i < trimFrames; ++i) {
                pcm.data[static_cast<size_t>(i*2)] = ch0[start + i];
                pcm.data[static_cast<size_t>(i*2+1)] = ch1[start + i];
            }
        }
        graph_.slotPlayer().stagePcm(s, std::move(pcm), cfg.mode);
        pushTrace(TraceEvt::Op::StagePcm, s);
        // Vérifier que le staging a réussi
        if (!graph_.slotPlayer().hasStagedPcm(s)) return false;
    }
    transitionTrace_.push({TraceEntry::Type::Ready, boundary, fromIdx, toIdx, 255, 0.f, 0.f});

    // Stocker le plan et armer la transition
    transitionPlan_ = plan;
    transitionPlanValid_.store(true, std::memory_order_release);
    transition_.armWithPlan(plan);
    transitionTrace_.push({TraceEntry::Type::Armed, plan.boundary, fromIdx, toIdx, 255, 0.f, 0.f});
    for (int s = 0; s < kMaxSlots; ++s) {
        auto act = plan.slots[s].action;
        TraceEntry::Type t = TraceEntry::Type::Keep;
        if (act == SlotPlanAction::Keep) t = TraceEntry::Type::Keep;
        else if (act == SlotPlanAction::Morph) t = TraceEntry::Type::Morph;
        else if (act == SlotPlanAction::Leave) t = TraceEntry::Type::Leave;
        else if (act == SlotPlanAction::Enter) t = TraceEntry::Type::Enter;
        transitionTrace_.push({t, plan.boundary, fromIdx, toIdx, static_cast<uint8_t>(s), 0.f, 0.f});
    }
    for (int i = 0; i < plan.numEvents; ++i) {
        const auto& ev = plan.events[i];
        TraceEntry::Type t = TraceEntry::Type::GainRamp;
        if (ev.type == PlanEventType::PcmFlip) t = TraceEntry::Type::PcmFlip;
        else if (ev.type == PlanEventType::GainRamp) t = TraceEntry::Type::GainRamp;
        else if (ev.type == PlanEventType::Release) t = TraceEntry::Type::Release;
        transitionTrace_.push({t, ev.atSample, fromIdx, toIdx, ev.slot, ev.a, ev.b});
    }

    // Stage pattern B à la même frontière (sample-accurate, même sample que GainRamps)
    // + recalage de phase : la scène entrante démarre sur le step 0 de son pattern.
    {
        StepBuf nextBuf;
        for (int i = 0; i < kMaxSlots; ++i) {
            const auto& tr = to.trackBarCounts[static_cast<size_t>(i)];
            const int nSteps = tr * 16;
            nextBuf.trackStepCount[i] = nSteps;
            for (int s = 0; s < kMaxSteps; ++s)
                nextBuf.steps[i][s] = to.steps[static_cast<size_t>(i)][static_cast<size_t>(s)];
        }
        stageStepBufferForBoundary(nextBuf, boundary, boundaryStep);
    }

    // Mettre à jour slotPath_/trim pour les slots stagés (évite re-import dans applyScene)
    // + capture pré-ARM : cancelPendingTransition() restaure ces valeurs si l'annulation
    // survient avant COMMIT (sinon le prochain diff par AssetId croit le slot déjà sur
    // l'asset de la cible et saute un PcmFlip).
    for (int s = 0; s < kMaxSlots; ++s) {
        if (plan.slots[s].action != SlotPlanAction::Enter) continue;
        preArmPath_[s]      = slotPath_[s];
        preArmTrimStart_[s] = slotTrimStart_[s];
        preArmTrimEnd_[s]   = slotTrimEnd_[s];
        slotPath_[s]      = to.slots[s].filePath;
        slotTrimStart_[s] = to.slots[s].trimStart;
        slotTrimEnd_[s]   = to.slots[s].trimEnd;
    }

    // Durée affichée / garde anti-re-navigation : le cycle de la scène courante
    // (c'est lui qu'on attend), pas la longueur de la scène cible.
    setPendingTransitionLen(cycleSteps);
    setPendingScene(toIdx);

    return true;
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

// ─── Fix P0 — STOPPED nav + Play ─────────────────────────────────────────────

void EngineFacade::activateSceneStopped(int sceneIdx) noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kMaxScenes) return;
    if (transport_.snapshot().playing) return;  // ne rien faire si déjà en lecture

    // 1. Annuler toute transition STOP pendante (BUILD/BREAKDOWN/DUB).
    //    Le plan DIRECT (transitionPlanValid_) n'est pas touché ici.
    transition_.reset();
    graph_.sequencer().clearStaged();
    graph_.sequencer().resetPatternPhase();    // navigation arrêtée → base à 0
    pendingScene_.store(-1, std::memory_order_relaxed);
    pendingTransLen_.store(0, std::memory_order_relaxed);
    // Annuler les PCM stagés pour les ENTER d'une éventuelle transition annulée.
    for (int s = 0; s < kMaxSlots; ++s)
        graph_.slotPlayer().clearStagedPcm(s);

    // 2. Syncer les patterns depuis la SceneDefinition (sans flip audio — transport arrêté).
    currentScene_.store(sceneIdx, std::memory_order_relaxed);
    syncWritePatternsFromScene(sceneIdx);
    flipPatternBuffer();

    // 3. Garantir PCM valide pour chaque slot de la scène.
    const SceneData& sc = sceneStore_.getScene(sceneIdx);
    for (int s = 0; s < kMaxSlots; ++s) {
        const SlotConfig& cfg = sc.slots[s];

        if (cfg.filePath.empty()) {
            // Slot absent de cette scène → assurer état cleared (sans toucher sceneStore).
            if (slotLoaded_[s].load(std::memory_order_acquire)) {
                clearSlot(s);  // incrémente slotGen_, arrête voix, vide runtime
            }
            continue;
        }

        // Comparer l'asset runtime avec l'asset attendu par la scène.
        const AssetId sceneAsset   = assetIdForSlot(cfg);
        const AssetId runtimeAsset = assetIdFor(slotPath_[s], slotTrimStart_[s], slotTrimEnd_[s]);
        const int runtimeFrames    = graph_.slotPlayer().getPcmFrames(s);

        if (runtimeAsset == sceneAsset
            && slotLoaded_[s].load(std::memory_order_acquire)
            && runtimeFrames >= kMinValidFrames) {
            // PCM déjà valide — mettre à jour uniquement les paramètres de lecture.
            graph_.slotPlayer().setGain(s, cfg.gain);
            graph_.slotPlayer().setMode(s, cfg.mode);
            graph_.slotPlayer().setSemitones(s, cfg.semitones);
            graph_.setSlotRole(s, cfg.role);
            continue;
        }

        // PCM absent, invalide (frames < kMinValidFrames) ou mauvais asset → force-load.
        loadSlotFromFileSync(s, cfg);
    }
}

void EngineFacade::loadSlotFromFileSync(int slot, const SlotConfig& cfg) noexcept
{
    // Incrémenter la génération : invalide tout worker en vol pour ce slot.
    slotGen_[slot].fetch_add(1, std::memory_order_release);

    juce::AudioFormatManager fmtMgr;
    fmtMgr.registerBasicFormats();
    const auto file = juce::File(juce::String(cfg.filePath));
    if (!file.existsAsFile()) {
        clearSlot(slot);
        return;
    }
    std::unique_ptr<juce::AudioFormatReader> reader(fmtMgr.createReaderFor(file));
    if (!reader || reader->lengthInSamples <= 0 || reader->numChannels <= 0) {
        clearSlot(slot);
        return;
    }
    const int   numFrames = static_cast<int>(reader->lengthInSamples);
    const int   numCh     = static_cast<int>(reader->numChannels);
    const float sr        = static_cast<float>(reader->sampleRate);
    juce::AudioBuffer<float> buf(numCh, numFrames);
    if (!reader->read(&buf, 0, numFrames, 0, true, numCh > 1)) {
        clearSlot(slot);
        return;
    }

    int start = (cfg.trimStart >= 0) ? std::min(cfg.trimStart, numFrames) : 0;
    int end   = (cfg.trimEnd   >= 0) ? std::min(cfg.trimEnd,   numFrames) : numFrames;
    if (end <= start) end = numFrames;  // trim invalide → tout le fichier
    const int trimFrames = end - start;

    SlotPcm pcm;
    pcm.numChannels = std::min(numCh, 2);
    pcm.numFrames   = trimFrames;
    pcm.sampleRate  = sr;
    pcm.data.resize(static_cast<size_t>(trimFrames * pcm.numChannels));
    if (pcm.numChannels == 1) {
        const float* ch0 = buf.getReadPointer(0);
        for (int i = 0; i < trimFrames; ++i)
            pcm.data[static_cast<size_t>(i)] = ch0[start + i];
    } else {
        const float* ch0 = buf.getReadPointer(0);
        const float* ch1 = buf.getReadPointer(1);
        for (int i = 0; i < trimFrames; ++i) {
            pcm.data[static_cast<size_t>(i * 2)    ] = ch0[start + i];
            pcm.data[static_cast<size_t>(i * 2 + 1)] = ch1[start + i];
        }
    }

    // Acquisition du mutex de slot : sérialise avec tout worker en vol.
    // Le worker vérifiera le gen et abortera puisqu'on l'a déjà incrémenté.
    std::lock_guard<std::mutex> lock(importSlotMutex_[slot]);
    graph_.slotPlayer().clearSlot(slot);   // arrêt voix + marked unloaded
    graph_.slotPlayer().loadSlot(slot, std::move(pcm), cfg.mode);
    slotLoaded_[slot].store(true, std::memory_order_release);
    slotPath_     [slot] = cfg.filePath;
    slotTrimStart_[slot] = cfg.trimStart >= 0 ? cfg.trimStart : 0;
    slotTrimEnd_  [slot] = cfg.trimEnd;
    graph_.slotPlayer().setGain(slot, cfg.gain);
    graph_.slotPlayer().setSemitones(slot, cfg.semitones);
    graph_.setSlotRole(slot, cfg.role);

    const AssetId loadedAsset = assetIdForSlot(cfg);
    pushTrace(TraceEvt::Op::LoadSlot, slot, trimFrames,
              static_cast<uint32_t>(loadedAsset));
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

void EngineFacade::syncWritePatternsFromScene(int idx) noexcept
{
    if (idx < 0 || idx >= kMaxScenes) return;
    const SceneData& sc = sceneStore_.getScene(idx);
    for (int s = 0; s < kMaxSlots; ++s)
    {
        const std::size_t si   = static_cast<std::size_t>(s);
        const int         bars = sc.trackBarCounts[si] > 0 ? sc.trackBarCounts[si] : 1;
        writePatterns_[s].numSteps = bars * 16;
        trackBars_[s]              = bars;
        for (int i = 0; i < kMaxSteps; ++i)
            writePatterns_[s].steps[i] = sc.steps[si][static_cast<std::size_t>(i)];
    }
    // No flipPatternBuffer() — le triple-buffer audio reste intact.
    // getStep() / getTrackStepCount() reflèteront immédiatement la nouvelle scène.
}

void EngineFacade::prepareStepBuffer(const StepBuf& buf) noexcept
{
    graph_.sequencer().clearStaged();   // publication immédiate : annule le stage
    for (int s = 0; s < kMaxSlots; ++s)
    {
        const int steps = buf.trackStepCount[s] > 0 ? buf.trackStepCount[s] : 16;
        writePatterns_[s].numSteps = steps;
        trackBars_[s] = steps / 16;
        for (int i = 0; i < kMaxSteps; ++i)
            writePatterns_[s].steps[i] = buf.steps[s][i];
    }
    flipPatternBuffer();
}

void EngineFacade::stageStepBufferForBoundary(const StepBuf& buf,
                                              int64_t boundarySample) noexcept
{
    stageStepBufferForBoundary(buf, boundarySample, kKeepPhaseBase);
}

void EngineFacade::stageStepBufferForBoundary(const StepBuf& buf,
                                              int64_t boundarySample,
                                              int64_t boundaryStep) noexcept
{
    TrackPattern staged[kMaxSlots];
    for (int s = 0; s < kMaxSlots; ++s)
    {
        const int steps = buf.trackStepCount[s] > 0 ? buf.trackStepCount[s] : 16;
        staged[s].numSteps = steps;
        for (int i = 0; i < kMaxSteps; ++i)
            staged[s].steps[i] = buf.steps[s][i];
    }
    if (boundaryStep == kKeepPhaseBase)
        graph_.sequencer().stageForBoundary(staged, boundarySample);
    else
        graph_.sequencer().stageForBoundary(staged, boundarySample, boundaryStep);
}

int64_t EngineFacade::transitionExecutionSample() const noexcept
{
    if (transition_.state() != TransitionEngine::State::Armed) return -1;
    return transition_.plan().executionSample;
}

void EngineFacade::stopAllSlots(StopMode /*mode*/) noexcept
{
    constexpr uint16_t kAllSlots = (1u << kMaxSlots) - 1u;
    pendingStops_.fetch_or(kAllSlots, std::memory_order_release);
}

// ─── Playhead / séquenceur (pour l'UI) ─────────────────────────────────────

int32_t EngineFacade::getCurrentStep() const noexcept
{
    const TransportState ts = transport_.snapshot();
    if (!ts.playing) return 0;
    return static_cast<int32_t>(stepIndexAt(ts, ts.samplePos) % kMaxSteps);
}

int32_t EngineFacade::getTrackStep(int track) const noexcept
{
    if (track < 0 || track >= kMaxSlots) return 0;
    const TransportState ts = transport_.snapshot();
    if (!ts.playing) return 0;
    const int64_t g = stepIndexAt(ts, ts.samplePos);
    const int32_t n = graph_.sequencer().numStepsForSlot(track);
    int64_t rel = (g - graph_.sequencer().phaseBase(track)) % n;
    if (rel < 0) rel += n;
    return static_cast<int32_t>(rel);
}

double EngineFacade::getCurrentPhase() const noexcept
{
    const TransportState ts = transport_.snapshot();
    if (!ts.playing) return 0.0;
    return beatAt(ts, ts.samplePos);
}

} // namespace engine
