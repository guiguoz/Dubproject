#pragma once

#include "BpmDetector.h"
#include "KeyDetector.h"
#include "Sampler.h"
#include "SmartMixEngine.h"    // for MusicContext
#include "WsolaShifter.h"
#include "AiContentClassifier.h"

#ifdef SAXFX_HAS_ONNX
#include "FeatureExtractor.h"
#include "AiMixEngine.h"
#endif

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// SmartSamplerEngine
//
// Two entry points:
//
//   processSlotOnLoad(slot, path, ctx)  — called automatically when a slot
//       file is loaded AND the master context is available.  Runs BPM+key
//       detection, time-stretch and pitch correction in a per-slot background
//       thread.
//
//   applyMagicMix()  — triggered by ⚡.  Applies gain staging + frequency
//       balance (EQ by role) across all loaded slots.
//
// All heavy work runs in background threads.  Callbacks onSlotProgress /
// onDone fire on the JUCE message thread.
// ─────────────────────────────────────────────────────────────────────────────
class SmartSamplerEngine
{
public:
    // ── Instrument content type (public for UI override) ───────────────────────
    enum class ContentType { KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC, OTHER };

    static std::string contentTypeName(ContentType t)
    {
        switch (t)
        {
            case ContentType::KICK:  return "KICK";
            case ContentType::SNARE: return "SNR";
            case ContentType::HIHAT: return "HAT";
            case ContentType::BASS:  return "BASS";
            case ContentType::SYNTH: return "SYN";
            case ContentType::PAD:   return "PAD";
            case ContentType::PERC:  return "PRC";
            default:                 return "???";
        }
    }

    // ── Callbacks (message thread) ────────────────────────────────────────────
    std::function<void(int /*slot*/, float /*progress 0-1*/)> onSlotProgress;
    std::function<void()>                                     onDone;
    std::function<void()>                                     onTypesDetected;

    explicit SmartSamplerEngine(::dsp::Sampler& sampler, double sampleRate = 44100.0)
        : sampler_(sampler), sampleRate_(sampleRate)
    {}

    ~SmartSamplerEngine()
    {
        cancelAndWait();
        for (int i = 0; i < kSamplerSlots; ++i)
            cancelOnLoadWorker(i);
    }

    void setSampleRate(double sr) noexcept { sampleRate_ = sr; }

    /// Toggle AI mix engine on/off (requires SAXFX_HAS_ONNX; no-op otherwise).
    void setUseAiMix(bool use) noexcept { useAiMix_ = use; }
    bool getUseAiMix() const noexcept   { return useAiMix_; }

    // ── Cache helpers (public so MainComponent can use them) ──────────────────

    /// Returns the processed WAV cache path for a given original file path.
    /// Format: {dir}/{name}_saxfx.wav
    static std::string getCachePath(const std::string& originalPath)
    {
        if (originalPath.empty()) return {};
        juce::File orig(originalPath);
        return orig.getSiblingFile(orig.getFileNameWithoutExtension() + "_saxfx.wav")
                   .getFullPathName().toStdString();
    }

    /// Returns true if a valid (newer than original) processed cache exists.
    static bool cacheIsValid(const std::string& originalPath)
    {
        if (originalPath.empty()) return false;
        juce::File orig(originalPath);
        juce::File cache(getCachePath(originalPath));
        return cache.existsAsFile()
            && cache.getLastModificationTime() >= orig.getLastModificationTime();
    }

    void setMusicContext(MusicContext ctx) noexcept { musicCtx_ = ctx; }

    void setSlotFilePath(int slot, std::string path)
    {
        if (slot >= 0 && slot < kSamplerSlots)
            filePaths_[static_cast<std::size_t>(slot)] = std::move(path);
    }

    // ── On-load automatic processing ──────────────────────────────────────────

    /// Called automatically when a slot is loaded and master context is known.
    /// Cancels any previous on-load worker for this slot then starts a new one.
    void processSlotOnLoad(int slot, std::string path, MusicContext ctx)
    {
        if (slot < 0 || slot >= kSamplerSlots) return;
        cancelOnLoadWorker(slot);
        filePaths_[static_cast<std::size_t>(slot)] = path;
        onLoadWorkers_[static_cast<std::size_t>(slot)] =
            std::make_unique<OnLoadWorkerThread>(*this, slot, std::move(path), ctx);
        onLoadWorkers_[static_cast<std::size_t>(slot)]->startThread();
    }

    // ── Magic button action ───────────────────────────────────────────────────

    /// Toggle ⚡: first press applies Neutron mix, second press reverts to originals.
    void toggleMagicMix()
    {
        if (magicActive_.load(std::memory_order_acquire))
            startWorker(true);   // revert
        else
            startWorker(false);  // apply
    }

    /// Direct apply (kept for compatibility).
    void applyMagicMix() { startWorker(false); }

    bool isBusy()        const noexcept { return busy_       .load(std::memory_order_acquire); }
    bool isMagicActive() const noexcept { return magicActive_.load(std::memory_order_acquire); }

    // ── Content type API ──────────────────────────────────────────────────────

    ContentType getDetectedType(int slot) const noexcept
    {
        if (slot < 0 || slot >= kSamplerSlots) return ContentType::OTHER;
        return detectedTypes_[static_cast<std::size_t>(slot)];
    }

    /// Override the auto-detected type for a slot (used by right-click UI menu).
    void setTypeOverride(int slot, ContentType type) noexcept
    {
        if (slot < 0 || slot >= kSamplerSlots) return;
        overrideTypes_[static_cast<std::size_t>(slot)] = type;
        hasOverride_  [static_cast<std::size_t>(slot)] = true;
    }

    void clearTypeOverride(int slot) noexcept
    {
        if (slot < 0 || slot >= kSamplerSlots) return;
        hasOverride_[static_cast<std::size_t>(slot)] = false;
    }

private:
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr int kSamplerSlots = 8;   // S1–S8 only (master has its own analysis)

    // ── Parameters for per-slot DSP ───────────────────────────────────────────

    struct SlotProcessParams
    {
        int    targetKey;   // 0=C … 11=B, −1 = unknown
        float  masterBpm;   // 0 = unknown
        double sampleRate;
    };

    // ── Instrument content type helpers ───────────────────────────────────────

    /// Classify a slot by spectral band energies + transient analysis.
    /// Uses four 1st-order LP-derived bands (sub / bass / mid / high) and
    /// the peak-to-RMS ratio of the attack window to distinguish
    /// percussive from sustained content.
    static ContentType detectContentType(const std::vector<float>& pcm, double sampleRate)
    {
        if (pcm.empty()) return ContentType::OTHER;
        const float fs = static_cast<float>(sampleRate);

        // 1. Transient ratio: peak / RMS over first 20 ms
        const int attackN = std::min(static_cast<int>(pcm.size()),
                                     static_cast<int>(fs * 0.020f));
        float peak = 0.f, sumSqAtt = 0.f;
        for (int i = 0; i < attackN; ++i)
        {
            peak = std::max(peak, std::abs(pcm[static_cast<std::size_t>(i)]));
            sumSqAtt += pcm[static_cast<std::size_t>(i)] * pcm[static_cast<std::size_t>(i)];
        }
        const float rmsAtt = std::sqrt(sumSqAtt / std::max(attackN, 1));
        const float transientRatio = (rmsAtt > 0.001f) ? peak / rmsAtt : 1.0f;

        // 2. Duration in ms
        const float durationMs = static_cast<float>(pcm.size()) / fs * 1000.f;

        // 3. Band energies via cascaded 1st-order LP state variables
        const auto alphaFor = [fs](float fc) noexcept {
            const float t = juce::MathConstants<float>::twoPi * fc / fs;
            return t / (t + 1.f);
        };
        const float a150  = alphaFor(150.f);
        const float a500  = alphaFor(500.f);
        const float a3000 = alphaFor(3000.f);

        float y150 = 0.f, y500 = 0.f, y3000 = 0.f;
        float eSub = 0.f, eBass = 0.f, eMid = 0.f, eHigh = 0.f, eTotal = 0.f;

        const int N = std::min(static_cast<int>(pcm.size()),
                               static_cast<int>(sampleRate));  // max 1 s
        for (int i = 0; i < N; ++i)
        {
            const float x = pcm[static_cast<std::size_t>(i)];
            y150  = a150  * x + (1.f - a150)  * y150;
            y500  = a500  * x + (1.f - a500)  * y500;
            y3000 = a3000 * x + (1.f - a3000) * y3000;
            eSub  += y150  * y150;
            eBass += (y500 - y150)  * (y500 - y150);
            eMid  += (y3000 - y500) * (y3000 - y500);
            eHigh += (x - y3000)    * (x - y3000);
            eTotal += x * x;
        }
        if (eTotal < 1e-8f) return ContentType::OTHER;

        const float subFrac  = eSub  / eTotal;
        const float lowFrac  = (eSub + eBass) / eTotal;
        const float midFrac  = eMid  / eTotal;
        const float highFrac = eHigh / eTotal;

        // Classification rules (order matters — most specific first)
        if (transientRatio > 3.0f && subFrac   > 0.28f && durationMs < 600.f)
            return ContentType::KICK;
        if (transientRatio > 2.5f && highFrac  > 0.40f && durationMs < 400.f)
            return ContentType::HIHAT;
        if (transientRatio > 4.5f && highFrac  > 0.30f && durationMs < 500.f)
            return ContentType::HIHAT;
        if (transientRatio > 2.5f && midFrac   > 0.28f && durationMs < 700.f)
            return ContentType::SNARE;
        if (transientRatio < 2.0f && lowFrac   > 0.50f)
            return ContentType::BASS;
        if (transientRatio < 1.8f && durationMs > 1500.f && highFrac < 0.35f)
            return ContentType::PAD;
        if (transientRatio > 2.5f)
            return ContentType::PERC;
        if (transientRatio < 2.0f && midFrac   > 0.35f)
            return ContentType::SYNTH;
        return ContentType::OTHER;
    }

    // ── Biquad IIR filter ─────────────────────────────────────────────────────

    struct BiquadCoeffs { float b0, b1, b2, a1, a2; };  // a0 normalised to 1

    // True-peak estimate via 4× linear oversampling (ITU-R BS.1770-4 simplified).
    // Catches inter-sample peaks that exceed 0dBFS on a DAC even when PCM samples look safe.
    static float calculateTruePeak(const std::vector<float>& pcm)
    {
        const int n = static_cast<int>(pcm.size());
        if (n == 0) return 0.f;
        float peak = 0.f;
        // Upsample 4× by linear interpolation and track max absolute value.
        for (int i = 0; i < n - 1; ++i)
        {
            const float s0 = pcm[static_cast<std::size_t>(i)];
            const float s1 = pcm[static_cast<std::size_t>(i + 1)];
            peak = std::max(peak, std::abs(s0));
            peak = std::max(peak, std::abs(s0 + 0.25f * (s1 - s0)));
            peak = std::max(peak, std::abs(s0 + 0.50f * (s1 - s0)));
            peak = std::max(peak, std::abs(s0 + 0.75f * (s1 - s0)));
        }
        peak = std::max(peak, std::abs(pcm.back()));
        return peak;
    }

    static void applyBiquad(std::vector<float>& pcm, const BiquadCoeffs& c)
    {
        float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;
        for (auto& s : pcm)
        {
            const float x0 = s;
            const float y0 = c.b0*x0 + c.b1*x1 + c.b2*x2 - c.a1*y1 - c.a2*y2;
            x2 = x1; x1 = x0;
            y2 = y1; y1 = y0;
            s = y0;
        }
    }

    static BiquadCoeffs makeHP(float fc, double sampleRate)
    {
        const float w0    = juce::MathConstants<float>::twoPi * fc / static_cast<float>(sampleRate);
        const float cosw0 = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * 0.707f);
        const float a0    = 1.f + alpha;
        return { (1.f + cosw0) / (2.f * a0), -(1.f + cosw0) / a0,
                 (1.f + cosw0) / (2.f * a0), -2.f * cosw0 / a0, (1.f - alpha) / a0 };
    }

    static BiquadCoeffs makeLP(float fc, double sampleRate)
    {
        const float w0    = juce::MathConstants<float>::twoPi * fc / static_cast<float>(sampleRate);
        const float cosw0 = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * 0.707f);
        const float a0    = 1.f + alpha;
        return { (1.f - cosw0) / (2.f * a0), (1.f - cosw0) / a0,
                 (1.f - cosw0) / (2.f * a0), -2.f * cosw0 / a0, (1.f - alpha) / a0 };
    }

    static BiquadCoeffs makeLowShelf(float fc, float dBgain, double sampleRate)
    {
        const float A     = std::pow(10.f, dBgain / 40.f);
        const float w0    = juce::MathConstants<float>::twoPi * fc / static_cast<float>(sampleRate);
        const float cosw0 = std::cos(w0);
        const float q     = 2.f * std::sqrt(A) * std::sin(w0) / 2.f;  // S=1
        const float a0    = (A+1.f) + (A-1.f)*cosw0 + q;
        return { A * ((A+1.f) - (A-1.f)*cosw0 + q) / a0,
                 2.f * A * ((A-1.f) - (A+1.f)*cosw0) / a0,
                 A * ((A+1.f) - (A-1.f)*cosw0 - q) / a0,
                 -2.f * ((A-1.f) + (A+1.f)*cosw0) / a0,
                 ((A+1.f) + (A-1.f)*cosw0 - q) / a0 };
    }

    static BiquadCoeffs makeHighShelf(float fc, float dBgain, double sampleRate)
    {
        const float A     = std::pow(10.f, dBgain / 40.f);
        const float w0    = juce::MathConstants<float>::twoPi * fc / static_cast<float>(sampleRate);
        const float cosw0 = std::cos(w0);
        const float q     = 2.f * std::sqrt(A) * std::sin(w0) / 2.f;  // S=1
        const float a0    = (A+1.f) - (A-1.f)*cosw0 + q;
        return { A * ((A+1.f) + (A-1.f)*cosw0 + q) / a0,
                 -2.f * A * ((A-1.f) + (A+1.f)*cosw0) / a0,
                 A * ((A+1.f) + (A-1.f)*cosw0 - q) / a0,
                 2.f * ((A-1.f) - (A+1.f)*cosw0) / a0,
                 ((A+1.f) - (A-1.f)*cosw0 - q) / a0 };
    }

    static BiquadCoeffs makePeaking(float fc, float dBgain, float Q, double sampleRate)
    {
        const float A     = std::pow(10.f, dBgain / 40.f);
        const float w0    = juce::MathConstants<float>::twoPi * fc / static_cast<float>(sampleRate);
        const float cosw0 = std::cos(w0);
        const float alpha = std::sin(w0) / (2.f * Q);
        const float a0    = 1.f + alpha / A;
        return { (1.f + alpha*A) / a0, -2.f * cosw0 / a0, (1.f - alpha*A) / a0,
                 -2.f * cosw0 / a0,    (1.f - alpha/A) / a0 };
    }

    // ── Role-based EQ presets (Neutron Track Assistant-inspired) ────────────────
    //
    // Applies well-known mixing EQ curves to the PCM vector in-place.
    // Called once from the original (clean) PCM — never cumulative.

    static void applyRoleEQ(std::vector<float>& pcm, ContentType type, double sr)
    {
        switch (type)
        {
            case ContentType::KICK:
                applyBiquad(pcm, makeHP(30.f, sr));
                applyBiquad(pcm, makeLowShelf(60.f, 3.f, sr));          // sub body
                applyBiquad(pcm, makePeaking(300.f, -2.f, 1.5f, sr));   // cut mud
                applyBiquad(pcm, makePeaking(3000.f, 2.f, 2.f, sr));    // click
                break;
            case ContentType::SNARE:
                applyBiquad(pcm, makeHP(60.f, sr));
                applyBiquad(pcm, makePeaking(200.f, 2.f, 1.5f, sr));    // body
                applyBiquad(pcm, makePeaking(5000.f, 3.f, 2.f, sr));    // snap
                break;
            case ContentType::HIHAT:
                applyBiquad(pcm, makeHP(300.f, sr));
                applyBiquad(pcm, makeHighShelf(9000.f, 2.f, sr));        // air
                break;
            case ContentType::BASS:
                applyBiquad(pcm, makeHP(30.f, sr));
                applyBiquad(pcm, makeLowShelf(80.f, 2.f, sr));          // warmth
                applyBiquad(pcm, makePeaking(1000.f, -2.f, 1.5f, sr)); // clarity
                break;
            case ContentType::SYNTH:
                applyBiquad(pcm, makeHP(100.f, sr));
                applyBiquad(pcm, makePeaking(400.f, -1.f, 1.5f, sr));  // cut mud
                break;
            case ContentType::PAD:
                applyBiquad(pcm, makeHP(80.f, sr));
                applyBiquad(pcm, makePeaking(300.f, -2.f, 1.5f, sr));  // cut mud
                break;
            case ContentType::PERC:
                applyBiquad(pcm, makeHP(80.f, sr));
                applyBiquad(pcm, makePeaking(5000.f, 1.f, 2.f, sr));   // presence
                break;
            case ContentType::OTHER:
                applyBiquad(pcm, makeHP(60.f, sr));
                break;
        }
    }

    // ── Inter-track unmasking (Neutron Mix Assistant-inspired) ──────────────────
    //
    // Applies compensatory cuts where tracks compete in the same frequency band.
    // types[] must be filled for all kSamplerSlots before calling.

    static void applyUnmasking(std::vector<float>& pcm, int slot,
                               const ContentType* types, double sr)
    {
        const ContentType myType = types[slot];

        bool kickPresent = false;
        int  bassCount   = 0;
        int  percCount   = 0;

        for (int i = 0; i < kSamplerSlots; ++i)
        {
            if (types[i] == ContentType::KICK) kickPresent = true;
            if (types[i] == ContentType::BASS) ++bassCount;
            if (types[i] == ContentType::PERC
             || types[i] == ContentType::SNARE
             || types[i] == ContentType::KICK) ++percCount;
        }

        // Rule 1: KICK present → duck sub of BASS/SYNTH to let kick punch through
        if (kickPresent)
        {
            if (myType == ContentType::BASS)
                applyBiquad(pcm, makePeaking(70.f, -3.f, 1.0f, sr));
            if (myType == ContentType::SYNTH)
                applyBiquad(pcm, makePeaking(60.f, -1.f, 1.5f, sr));
        }

        // Rule 2: 2+ BASS tracks → secondary BASS slots get a 100 Hz cut
        if (myType == ContentType::BASS && bassCount >= 2)
        {
            bool isSecondary = false;
            for (int i = 0; i < slot; ++i)
                if (types[i] == ContentType::BASS) { isSecondary = true; break; }
            if (isSecondary)
                applyBiquad(pcm, makePeaking(100.f, -2.f, 1.0f, sr));
        }

        // Rule 3: KICK + SNARE overlap → SNARE cut at 300 Hz
        if (kickPresent && myType == ContentType::SNARE)
            applyBiquad(pcm, makePeaking(300.f, -2.f, 1.5f, sr));

        // Rule 4: 3+ percussive tracks → secondary ones cut 2 kHz presence
        if (percCount >= 3
         && (myType == ContentType::PERC || myType == ContentType::SNARE))
        {
            bool isSecondary = false;
            for (int i = 0; i < slot; ++i)
            {
                if (types[i] == ContentType::PERC
                 || types[i] == ContentType::SNARE)
                { isSecondary = true; break; }
            }
            if (isSecondary)
                applyBiquad(pcm, makePeaking(2000.f, -2.f, 1.5f, sr));
        }
    }

    // ── Target gain per instrument type ───────────────────────────────────────

    static float targetGainForType(ContentType type) noexcept
    {
        switch (type)
        {
            case ContentType::KICK:  return 0.75f;  // −2.5 dBFS — punch/headroom
            case ContentType::SNARE: return 0.60f;  // −4.4 dBFS
            case ContentType::HIHAT: return 0.30f;  // −10.5 dBFS — always discreet
            case ContentType::BASS:  return 0.55f;  // −5.2 dBFS
            case ContentType::SYNTH: return 0.45f;  // −7.0 dBFS
            case ContentType::PAD:   return 0.38f;  // −8.4 dBFS
            case ContentType::PERC:  return 0.55f;  // −5.2 dBFS
            default:                 return 0.50f;  // −6.0 dBFS
        }
    }

    // ── Neutron-inspired magic mix ─────────────────────────────────────────────
    //
    // For each loaded slot:
    //   1. Reload ORIGINAL PCM from disk (erases any previous processing)
    //   2. Detect instrument type (spectral + transient analysis)
    //   3. Apply role EQ preset
    //   4. Apply inter-track unmasking cuts
    //   5. Set gain = targetGainForType / peak
    //
    // Idempotent: pressing ⚡ multiple times gives the same result.

    void applyNeutronMix(juce::Thread* thread)
    {
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();

        ContentType           types[kSamplerSlots] {};
        std::vector<float>    pcms [kSamplerSlots];
        bool                  loaded[kSamplerSlots] {};
        int                   numLoaded = 0;

        // Phase 1: read original files + detect content type
        for (int i = 0; i < kSamplerSlots; ++i)
        {
            if (thread && thread->threadShouldExit()) return;

            const auto& path = filePaths_[static_cast<std::size_t>(i)];
            if (path.empty() || !sampler_.isLoaded(i)) continue;

            juce::File file(path);
            if (!file.existsAsFile()) continue;

            auto reader = std::unique_ptr<juce::AudioFormatReader>(
                fmt.createReaderFor(file));
            if (!reader) continue;

            const int n = static_cast<int>(reader->lengthInSamples);
            if (n <= 0) continue;

            juce::AudioBuffer<float> buf(1, n);
            reader->read(&buf, 0, n, 0, true, false);
            pcms[i].assign(buf.getReadPointer(0), buf.getReadPointer(0) + n);

            // Use AI classifier if available, fallback to heuristic
            #ifdef SAXFX_HAS_ONNX
            static AiContentClassifier classifier(
                "models/content_classifier.onnx",
                "models/content_classifier_norm.bin");
            types[i] = static_cast<ContentType>(classifier.classify(pcms[i], reader->sampleRate));
            #else
            types[i]  = detectContentType(pcms[i], reader->sampleRate);
            #endif
            loaded[i] = true;
            ++numLoaded;
        }

        if (numLoaded == 0) return;

        // Store detected types (use override when set)
        for (int i = 0; i < kSamplerSlots; ++i)
            if (loaded[i]) detectedTypes_[static_cast<std::size_t>(i)] = types[i];

        // Notify UI that types are available (message thread)
        {
            auto& eng = *this;
            juce::MessageManager::callAsync([&eng] {
                if (eng.onTypesDetected) eng.onTypesDetected();
            });
        }

        // Phase 2: EQ + gain staging (AI or heuristic)
        bool usedAi = false;

#ifdef SAXFX_HAS_ONNX
        if (useAiMix_)
        {
            // ── AI mix path ───────────────────────────────────────────────────
            static std::unique_ptr<AiMixEngine> aiMix;
            static bool aiMixFailed = false;
            if (!aiMix && !aiMixFailed)
            {
                try
                {
                    aiMix = std::make_unique<AiMixEngine>(
                        "models/mix_model.onnx",
                        "models/mix_model_norm.bin");
                }
                catch (...) { aiMixFailed = true; }
            }

            if (aiMix)
            {
                std::array<MixFeatures, kSamplerSlots> slotFeatures {};
                for (int i = 0; i < kSamplerSlots; ++i)
                    if (loaded[i])
                        slotFeatures[static_cast<std::size_t>(i)] =
                            FeatureExtractor::extract(pcms[i], sampleRate_);

                const auto decisions = aiMix->predict(slotFeatures);

                for (int i = 0; i < kSamplerSlots; ++i)
                {
                    if (thread && thread->threadShouldExit()) return;
                    if (!loaded[i]) continue;

                    const auto& d = decisions[static_cast<std::size_t>(i)];

                    // 3-band EQ from AI decision — corrected frequencies & order:
                    //   DC block (20Hz HP) → sub-bass shelf (100Hz) → presence peak (2500Hz)
                    //   → air shelf (8000Hz) → anti-alias LP (18kHz)
                    // Gains clamped to ±6dB to prevent saturation.
                    const float safeLoG = std::clamp(d.lowGain,  -6.f, 6.f);
                    const float safeMiG = std::clamp(d.midGain,  -6.f, 6.f);
                    const float safeHiG = std::clamp(d.highGain, -6.f, 6.f);
                    applyBiquad(pcms[i], makeHP       (20.f,           sampleRate_)); // DC block
                    applyBiquad(pcms[i], makeLowShelf (100.f,  safeLoG, sampleRate_)); // sub-bass
                    applyBiquad(pcms[i], makePeaking  (2500.f, safeMiG, 1.0f, sampleRate_)); // présence
                    applyBiquad(pcms[i], makeHighShelf(8000.f, safeHiG, sampleRate_)); // air
                    applyBiquad(pcms[i], makeLP       (18000.f,         sampleRate_)); // anti-alias

                    // Gain: target volume / true-peak (4× oversample), -3dBFS headroom
                    const float truePeak = calculateTruePeak(pcms[i]);
                    const float gain = juce::jlimit(0.f, 1.5f,
                        (d.volume * 0.707f) / std::max(truePeak, 0.001f));
                    sampler_.reloadSlotData(i, std::move(pcms[i]));
                    sampler_.setSlotGain(i, gain);

                    const int   reportSlot = i;
                    const float progress   = static_cast<float>(reportSlot + 1)
                                           / static_cast<float>(numLoaded);
                    auto& eng = *this;
                    juce::MessageManager::callAsync([&eng, reportSlot, progress]
                    {
                        if (eng.onSlotProgress) eng.onSlotProgress(reportSlot, progress);
                    });
                }
                usedAi = true;
            }
            // If aiMix failed to load, fall through to heuristic below
        }
#endif

        if (!usedAi)
        {
            // ── Heuristic mix path (fallback / ONNX disabled / toggle off) ───
            for (int i = 0; i < kSamplerSlots; ++i)
            {
                if (thread && thread->threadShouldExit()) return;
                if (!loaded[i]) continue;

                // Use manual override if set
                const ContentType effectiveType = hasOverride_[static_cast<std::size_t>(i)]
                    ? overrideTypes_[static_cast<std::size_t>(i)]
                    : types[i];

                // Apply role EQ preset (from clean original PCM)
                applyRoleEQ(pcms[i], effectiveType, sampleRate_);

                // Apply inter-track unmasking cuts (using effective types array)
                ContentType effectiveTypes[kSamplerSlots];
                for (int j = 0; j < kSamplerSlots; ++j)
                    effectiveTypes[j] = (loaded[j] && hasOverride_[static_cast<std::size_t>(j)])
                        ? overrideTypes_[static_cast<std::size_t>(j)]
                        : types[j];
                applyUnmasking(pcms[i], i, effectiveTypes, sampleRate_);

                // Gain calibration: true-peak (4× oversample) + -3dBFS headroom
                const float truePeak = calculateTruePeak(pcms[i]);
                const float gain = juce::jlimit(0.f, 1.5f,
                    (targetGainForType(effectiveType) * 0.707f) / std::max(truePeak, 0.001f));

                sampler_.reloadSlotData(i, std::move(pcms[i]));
                sampler_.setSlotGain(i, gain);

                const int   reportSlot = i;
                const float progress   = static_cast<float>(reportSlot + 1)
                                       / static_cast<float>(numLoaded);
                auto& eng = *this;
                juce::MessageManager::callAsync([&eng, reportSlot, progress]
                {
                    if (eng.onSlotProgress) eng.onSlotProgress(reportSlot, progress);
                });
            }
        }

        // Phase 3: Setup real-time sidechain (KICK → BASS/SYNTH)
        sampler_.clearSidechain();
        for (int kick = 0; kick < kSamplerSlots; ++kick)
        {
            if (!loaded[kick] || types[kick] != ContentType::KICK) continue;
            for (int tgt = 0; tgt < kSamplerSlots; ++tgt)
            {
                if (!loaded[tgt] || tgt == kick) continue;
                if (types[tgt] == ContentType::BASS || types[tgt] == ContentType::SYNTH)
                    sampler_.setSidechainPair(kick, tgt);
            }
        }
    }

    // ── Revert to original PCM (called when magic is toggled off) ─────────────

    void revertToOriginals(juce::Thread* thread)
    {
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();

        for (int i = 0; i < kSamplerSlots; ++i)
        {
            if (thread && thread->threadShouldExit()) return;
            const auto& path = filePaths_[static_cast<std::size_t>(i)];
            if (path.empty() || !sampler_.isLoaded(i)) continue;

            juce::File file(path);
            if (!file.existsAsFile()) continue;

            auto reader = std::unique_ptr<juce::AudioFormatReader>(
                fmt.createReaderFor(file));
            if (!reader) continue;

            const int n = static_cast<int>(reader->lengthInSamples);
            if (n <= 0) continue;

            juce::AudioBuffer<float> buf(1, n);
            reader->read(&buf, 0, n, 0, true, false);
            std::vector<float> pcm(buf.getReadPointer(0), buf.getReadPointer(0) + n);
            sampler_.reloadSlotData(i, std::move(pcm));
            sampler_.setSlotGain(i, 1.0f);
        }
        sampler_.clearSidechain();
    }

    // ── Semitone shift helper ─────────────────────────────────────────────────

    static int computeSemitoneShift(int sourceKey, int targetKey) noexcept
    {
        if (sourceKey < 0 || targetKey < 0) return 0;
        int delta = targetKey - sourceKey;
        if (delta >  6) delta -= 12;
        if (delta < -6) delta += 12;
        return delta;
    }

    // ── Pitch-shift on raw PCM ────────────────────────────────────────────────

    static std::vector<float> pitchShiftRaw(const std::vector<float>& data,
                                            float semitones,
                                            double sampleRate)
    {
        if (data.empty() || std::abs(semitones) < 0.25f)
            return {};

        WsolaShifter shifter;
        shifter.prepare(sampleRate, 4096);
        shifter.setShiftSemitones(semitones);

        std::vector<float> output(data.size(), 0.f);
        constexpr int kBlock = 1024;
        for (int i = 0; i < static_cast<int>(data.size()); i += kBlock)
        {
            const int n = std::min(kBlock, static_cast<int>(data.size()) - i);
            shifter.process(data.data() + i, output.data() + i, n, 0.f);
        }
        return output;
    }

    // ── Linear-interpolation time-stretch ────────────────────────────────────

    static std::vector<float> resample(const std::vector<float>& input, float ratio)
    {
        const int inN  = static_cast<int>(input.size());
        const int outN = static_cast<int>(std::round(static_cast<float>(inN) / ratio));
        if (outN <= 0 || inN <= 0) return {};

        std::vector<float> output(static_cast<std::size_t>(outN));
        for (int i = 0; i < outN; ++i)
        {
            const float srcPos = static_cast<float>(i) * ratio;
            const int   idx0   = static_cast<int>(srcPos);
            const int   idx1   = idx0 + 1;
            const float frac   = srcPos - static_cast<float>(idx0);
            const float s0 = (idx0 < inN) ? input[static_cast<std::size_t>(idx0)] : 0.f;
            const float s1 = (idx1 < inN) ? input[static_cast<std::size_t>(idx1)] : 0.f;
            output[static_cast<std::size_t>(i)] = s0 + frac * (s1 - s0);
        }
        return output;
    }

    // ── Core per-slot processing (shared by WorkerThread and OnLoadWorkerThread)
    //
    // Reads the file at filePath, detects BPM + key, applies time-stretch and
    // combined pitch correction, then reloads the slot.
    // Returns true if the slot data was modified.
    // thread may be nullptr (no early-exit check).
    // ─────────────────────────────────────────────────────────────────────────

    static bool processSlotData(::dsp::Sampler&       sampler,
                                int                   slot,
                                const std::string&    filePath,
                                const SlotProcessParams& params,
                                juce::Thread*         thread)
    {
        if (filePath.empty()) return false;

        const juce::File file(filePath);
        if (!file.existsAsFile()) return false;

        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
        if (!reader) return false;

        const double sr       = reader->sampleRate;
        const int    nSamples = static_cast<int>(reader->lengthInSamples);
        if (nSamples <= 0) return false;

        juce::AudioBuffer<float> buf(1, nSamples);
        reader->read(&buf, 0, nSamples, 0, true, false);

        std::vector<float> pcm(buf.getReadPointer(0),
                               buf.getReadPointer(0) + nSamples);

        // ── Trim leading silence — align first transient to beat 1 ───────────
        bool modified = (trimLeadingSilence(pcm, sr) > 0);

        // ── Key detection ────────────────────────────────────────────────────
        KeyDetector keyDet;
        constexpr int kChunk = 4096;
        for (int off = 0; off < nSamples; off += kChunk)
        {
            if (thread && thread->threadShouldExit()) return false;
            const int n = std::min(kChunk, nSamples - off);
            keyDet.process(pcm.data() + off, n, sr);
        }
        if (thread && thread->threadShouldExit()) return false;

        const int sourceKey = (keyDet.getResult().key >= 0) ? keyDet.getResult().key : -1;

        // ── BPM detection ─────────────────────────────────────────────────────
        const float slotBpm = BpmDetector::detectOffline(pcm.data(), nSamples, sr);

        // ── Stretch ratio ─────────────────────────────────────────────────────
        const float masterBpm    = params.masterBpm;
        const float stretchRatio = (masterBpm > 20.f && slotBpm > 20.f)
                                   ? masterBpm / slotBpm : 1.f;
        const bool  doStretch    = (std::abs(stretchRatio - 1.f) >= 0.05f)
                                   && stretchRatio > 0.33f
                                   && stretchRatio < 3.f;

        // ── Combined pitch: key correction + resample artifact fix ───────────
        const int   keyShift        = computeSemitoneShift(sourceKey, params.targetKey);
        const float stretchPitchFix = doStretch ? -12.f * std::log2(stretchRatio) : 0.f;
        const float totalSemitones  = static_cast<float>(keyShift) + stretchPitchFix;

        // Pass 1: resample for BPM alignment
        if (doStretch)
        {
            auto stretched = resample(pcm, stretchRatio);
            if (!stretched.empty()) { pcm = std::move(stretched); modified = true; }
        }
        if (thread && thread->threadShouldExit()) return false;

        // Pass 2: combined pitch correction (key + stretch pitch fix)
        if (std::abs(totalSemitones) >= 0.25f)
        {
            auto shifted = pitchShiftRaw(pcm, totalSemitones, params.sampleRate);
            if (!shifted.empty())
            {
                if (thread && thread->threadShouldExit()) return false;
                pcm = std::move(shifted);
                modified = true;
            }
        }

        // Pass 3: crop / pad to bar boundary for clean looping
        if (params.masterBpm > 0.f)
        {
            const auto sizeBefore = pcm.size();
            cropToBarBoundary(pcm, params.masterBpm, params.sampleRate);
            if (pcm.size() != sizeBefore) modified = true;
        }

        if (modified && !(thread && thread->threadShouldExit()))
        {
            // Persist the processed buffer as a WAV cache alongside the original
            writeCacheWav(filePath, pcm, params.sampleRate);

            sampler.reloadSlotData(slot, std::move(pcm));
            return true;
        }
        return false;
    }

    // ── Trim leading silence to first transient ───────────────────────────────
    //
    // Scans forward in 10 ms frames; removes everything before the first frame
    // whose RMS exceeds the threshold (~−40 dBFS).  Ensures the sample starts
    // on its first audible beat rather than on silence or pre-roll noise.
    //
    // Returns the number of samples removed (0 = nothing trimmed).
    // ─────────────────────────────────────────────────────────────────────────

    static int trimLeadingSilence(std::vector<float>& pcm,
                                  double sampleRate,
                                  float threshold = 0.01f)
    {
        if (pcm.empty()) return 0;

        const int frameSize = std::max(1, static_cast<int>(sampleRate * 0.01));  // 10 ms
        const int nSamples  = static_cast<int>(pcm.size());

        for (int start = 0; start < nSamples; start += frameSize)
        {
            const int end = std::min(start + frameSize, nSamples);
            float sumSq = 0.f;
            for (int i = start; i < end; ++i)
                sumSq += pcm[static_cast<std::size_t>(i)] * pcm[static_cast<std::size_t>(i)];
            const float rms = std::sqrt(sumSq / static_cast<float>(end - start));

            if (rms >= threshold)
            {
                if (start == 0) return 0;  // already starts on a transient
                pcm.erase(pcm.begin(), pcm.begin() + start);
                return start;
            }
        }
        return 0;  // all silence — leave unchanged
    }

    // ── Write processed PCM to WAV cache file ────────────────────────────────

    static void writeCacheWav(const std::string& originalPath,
                              const std::vector<float>& pcm,
                              double sampleRate)
    {
        if (originalPath.empty() || pcm.empty()) return;

        const std::string cachePath = getCachePath(originalPath);
        if (cachePath.empty()) return;

        juce::WavAudioFormat wavFmt;
        juce::File cacheFile(cachePath);

        auto* rawOs = cacheFile.createOutputStream().release();
        if (!rawOs) return;

        auto writer = std::unique_ptr<juce::AudioFormatWriter>(
            wavFmt.createWriterFor(rawOs, sampleRate, 1, 16, {}, 0));
        if (!writer) { delete rawOs; return; }

        juce::AudioBuffer<float> buf(1, static_cast<int>(pcm.size()));
        std::copy(pcm.begin(), pcm.end(), buf.getWritePointer(0));
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        // writer destructor flushes and closes the stream
    }

    // ── Crop / pad buffer to nearest bar boundary ────────────────────────────
    //
    // After BPM stretch, the buffer length may not be an exact multiple of a
    // measure (rounding of the BPM or the resample ratio).  This trims or
    // zero-pads the buffer to the nearest nBars × samplesPerBar length so
    // that the slot loops cleanly on the beat grid.
    //
    //   - Crop  : apply a 20 ms linear fade-out before truncating.
    //   - Pad   : zero-fill only when the gap is < 5 % of the target length.
    //   - Guard : do nothing if the difference exceeds half a bar (anomalous BPM).
    // ─────────────────────────────────────────────────────────────────────────

    static void cropToBarBoundary(std::vector<float>& pcm,
                                  float masterBpm,
                                  double sampleRate)
    {
        if (masterBpm <= 0.f || pcm.empty()) return;

        const double samplesPerBar =
            sampleRate * 60.0 / static_cast<double>(masterBpm) * 4.0;  // 4/4

        const int bufLen  = static_cast<int>(pcm.size());
        const int nBars   = static_cast<int>(
            std::round(static_cast<double>(bufLen) / samplesPerBar));
        if (nBars <= 0) return;

        const int targetLen = static_cast<int>(
            std::round(static_cast<double>(nBars) * samplesPerBar));
        const int delta     = std::abs(bufLen - targetLen);

        // Skip if the mismatch is too large (BPM detection was likely wrong)
        if (delta > static_cast<int>(samplesPerBar * 0.5)) return;

        if (bufLen > targetLen)
        {
            // Fade out the last 20 ms before cropping
            const int fadeLen = std::min(
                static_cast<int>(sampleRate * 0.02),
                std::min(targetLen / 4, delta + static_cast<int>(sampleRate * 0.02)));
            const int fadeStart = targetLen - fadeLen;
            for (int i = 0; i < fadeLen && (fadeStart + i) < bufLen; ++i)
            {
                const float t = 1.f - static_cast<float>(i) / static_cast<float>(fadeLen);
                pcm[static_cast<std::size_t>(fadeStart + i)] *= t;
            }
            pcm.resize(static_cast<std::size_t>(targetLen));
        }
        else if (bufLen < targetLen)
        {
            // Zero-pad only for small gaps (< 5 % of target length)
            if (static_cast<float>(delta) / static_cast<float>(targetLen) < 0.05f)
                pcm.resize(static_cast<std::size_t>(targetLen), 0.f);
        }
    }

    // ── Magic-button background worker (all slots) ────────────────────────────

    class WorkerThread : public juce::Thread
    {
    public:
        WorkerThread(SmartSamplerEngine& owner, bool revert)
            : juce::Thread("SmartSamplerWorker"), owner_(owner), revert_(revert)
        {}

        void run() override
        {
            try
            {
                if (revert_)
                    owner_.revertToOriginals(this);
                else
                    owner_.applyNeutronMix(this);
            }
            catch (const std::exception& e)
            {
                juce::Logger::writeToLog(juce::String("SmartSamplerEngine error: ") + e.what());
            }
            catch (...) {}
            finish();
        }

    private:
        void finish()
        {
            owner_.magicActive_.store(!revert_, std::memory_order_release);
            owner_.busy_.store(false, std::memory_order_release);
            auto& eng   = owner_;
            bool  rev   = revert_;
            juce::MessageManager::callAsync([&eng, rev]
            {
                if (eng.onDone) eng.onDone();
            });
        }

        SmartSamplerEngine& owner_;
        bool                revert_;
    };

    // ── On-load background worker (single slot) ───────────────────────────────

    class OnLoadWorkerThread : public juce::Thread
    {
    public:
        OnLoadWorkerThread(SmartSamplerEngine& owner, int slot,
                           std::string path, MusicContext ctx)
            : juce::Thread("SmartSamplerOnLoad"),
              owner_(owner), slot_(slot),
              path_(std::move(path)), ctx_(ctx)
        {}

        void run() override
        {
            const SlotProcessParams params { ctx_.keyRoot, ctx_.bpm, owner_.sampleRate_ };
            processSlotData(owner_.sampler_, slot_, path_, params, this);
            // No progress callback — on-load processing is silent (no UI indicator)
        }

    private:
        SmartSamplerEngine& owner_;
        int                 slot_;
        std::string         path_;
        MusicContext        ctx_;
    };

    // ── Thread management ─────────────────────────────────────────────────────

    void startWorker(bool revert)
    {
        cancelAndWait();
        busy_.store(true, std::memory_order_release);
        workerThread_ = std::make_unique<WorkerThread>(*this, revert);
        workerThread_->startThread();
    }

    void cancelAndWait()
    {
        if (workerThread_ && workerThread_->isThreadRunning())
            workerThread_->stopThread(3000);
        workerThread_.reset();
    }

    void cancelOnLoadWorker(int slot)
    {
        auto& w = onLoadWorkers_[static_cast<std::size_t>(slot)];
        if (w && w->isThreadRunning())
            w->stopThread(2000);
        w.reset();
    }

    // ── Members ───────────────────────────────────────────────────────────────

    ::dsp::Sampler&                          sampler_;
    double                                   sampleRate_ = 44100.0;
    MusicContext                             musicCtx_;
    bool                                     useAiMix_   = true;   // AI mix on by default
    std::array<std::string, kSamplerSlots>   filePaths_ {};
    std::atomic<bool>                        busy_        { false };
    std::atomic<bool>                        magicActive_ { false };
    ContentType detectedTypes_[kSamplerSlots] {};
    ContentType overrideTypes_[kSamplerSlots] {};
    bool        hasOverride_  [kSamplerSlots] {};
    std::unique_ptr<WorkerThread>            workerThread_;
    std::array<std::unique_ptr<OnLoadWorkerThread>, kSamplerSlots> onLoadWorkers_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SmartSamplerEngine)
};

} // namespace dsp
