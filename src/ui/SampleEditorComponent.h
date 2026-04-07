#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SampleEditorComponent
//
// Waveform display with draggable IN/OUT trim markers.
//
//   onApply(startSample, endSample)  — called when user clicks Apply
//   onCancel()                       — called when user clicks Cancel
//
// Usage:
//   auto* ed = new SampleEditorComponent(pcmCopy, sampleRate);
//   ed->onApply = [](int s, int e){ /* trim */ };
//   // wrap in DialogWindow::LaunchOptions and call launchAsync()
// ─────────────────────────────────────────────────────────────────────────────
class SampleEditorComponent : public juce::Component, private juce::Timer
{
public:
    std::function<void(int startSample, int endSample)> onApply;
    std::function<void()>                               onCancel;
    std::function<void()>                               onPlayRequested;
    std::function<void()>                               onStopRequested;
    std::function<void()>                               onClose;          // ferme la fenêtre parente
    std::function<float()>                              getPlayheadRatio; // 0..1 position de lecture
    std::function<bool()>                               isSlotPlaying;    // true si le slot joue

    SampleEditorComponent(std::vector<float> pcm, double sampleRate)
        : pcm_        (std::move(pcm))
        , sampleRate_ (sampleRate > 0.0 ? sampleRate : 44100.0)
        , startSample_(0)
        , endSample_  (static_cast<int>(pcm_.size()))
    {
        buildEnvelope();
        startTimerHz(30);

        playBtn_.setButtonText("Play");
        playBtn_.setClickingTogglesState(true);
        playBtn_.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF1C2A1A));
        playBtn_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF226622));
        playBtn_.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFF4CDFA8));
        playBtn_.onClick = [this] {
            if (playBtn_.getToggleState()) {
                playBtn_.setButtonText("Stop");
                if (onPlayRequested) onPlayRequested();
            } else {
                playBtn_.setButtonText("Play");
                if (onStopRequested) onStopRequested();
            }
        };
        addAndMakeVisible(playBtn_);

        applyBtn_.setButtonText("Apply");
        applyBtn_.setColour(juce::TextButton::buttonColourId,
                            juce::Colour(0xFF1A3A2A));
        applyBtn_.setColour(juce::TextButton::textColourOffId,
                            juce::Colour(0xFF4CDFA8));
        applyBtn_.onClick = [this] {
            if (onApply) onApply(startSample_, endSample_);
            if (onClose) onClose();
        };
        addAndMakeVisible(applyBtn_);

        cancelBtn_.setButtonText("Cancel");
        cancelBtn_.setColour(juce::TextButton::buttonColourId,
                             juce::Colour(0xFF1C1B1C));
        cancelBtn_.setColour(juce::TextButton::textColourOffId,
                             juce::Colour(0xFF6B6E70));
        cancelBtn_.onClick = [this] {
            if (onCancel) onCancel();
            if (onClose) onClose();
        };
        addAndMakeVisible(cancelBtn_);

        infoLabel_.setColour(juce::Label::textColourId,   juce::Colour(0xFF6B6E70));
        infoLabel_.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        infoLabel_.setFont(juce::Font(juce::FontOptions{}.withHeight(11.f)));
        infoLabel_.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(infoLabel_);

        updateInfoLabel();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(8);

        // Bottom buttons strip: [Cancel] [Play] [Apply]
        auto btnStrip = b.removeFromBottom(28);
        b.removeFromBottom(4);
        const int w3 = btnStrip.getWidth() / 3;
        cancelBtn_.setBounds(btnStrip.removeFromLeft(w3).reduced(4, 0));
        playBtn_  .setBounds(btnStrip.removeFromLeft(w3).reduced(4, 0));
        applyBtn_ .setBounds(btnStrip.reduced(4, 0));

        // Info label
        infoLabel_.setBounds(b.removeFromBottom(30));
        b.removeFromBottom(2);

        waveformBounds_ = b;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF0A0A0A));

        if (envelope_.empty()) return;

        const auto wb = waveformBounds_.toFloat();
        const float cy = wb.getCentreY();
        const float hh = wb.getHeight() * 0.46f;
        const int   N  = static_cast<int>(envelope_.size());

        // ── Waveform fill ─────────────────────────────────────────────────────
        {
            juce::Path wf;
            for (int i = 0; i < N; ++i)
            {
                const float x   = wb.getX() + static_cast<float>(i) / static_cast<float>(N) * wb.getWidth();
                const float amp = envelope_[static_cast<std::size_t>(i)] * hh;
                const float top = cy - amp;
                const float bot = cy + amp;

                // Dim zones outside trim range
                const float sampleX = wb.getX() + sampleToX(startSample_);
                const float endX    = wb.getX() + sampleToX(endSample_);
                const juce::Colour col = (x >= sampleX && x <= endX)
                    ? juce::Colour(0xFF4CDFA8)
                    : juce::Colour(0xFF1A3A2A);

                g.setColour(col);
                g.drawVerticalLine(static_cast<int>(x),
                                   std::max(wb.getY(), top),
                                   std::min(wb.getBottom(), bot));
            }
        }

        // ── Centre line ───────────────────────────────────────────────────────
        g.setColour(juce::Colour(0xFF1C3020));
        g.drawHorizontalLine(static_cast<int>(cy), wb.getX(), wb.getRight());

        // ── Trim markers ──────────────────────────────────────────────────────
        const float sx = wb.getX() + sampleToX(startSample_);
        const float ex = wb.getX() + sampleToX(endSample_);

        // Start — yellow
        g.setColour(juce::Colour(0xFFFFDD00));
        g.drawVerticalLine(static_cast<int>(sx), wb.getY(), wb.getBottom());
        g.fillRect(sx - 4.f, wb.getY(), 8.f, 12.f);
        g.setColour(juce::Colour(0xFF111100));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
        g.drawText("IN", static_cast<int>(sx) - 8, static_cast<int>(wb.getY()) + 14,
                   20, 10, juce::Justification::centred);

        // End — red
        g.setColour(juce::Colour(0xFFFF4444));
        g.drawVerticalLine(static_cast<int>(ex), wb.getY(), wb.getBottom());
        g.fillRect(ex - 4.f, wb.getY(), 8.f, 12.f);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f)));
        g.drawText("OUT", static_cast<int>(ex) - 10, static_cast<int>(wb.getY()) + 14,
                   24, 10, juce::Justification::centred);

        // ── Playhead ──────────────────────────────────────────────────────────
        if (getPlayheadRatio && isSlotPlaying && isSlotPlaying())
        {
            const float ratio = getPlayheadRatio();
            const int px = static_cast<int>(wb.getX() + ratio * wb.getWidth());
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawVerticalLine(px, wb.getY(), wb.getBottom());
        }
    }

    // ── Mouse drag for markers ────────────────────────────────────────────────

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!waveformBounds_.contains(e.getPosition())) return;

        const float mx = static_cast<float>(e.x) - static_cast<float>(waveformBounds_.getX());
        const float sx = sampleToX(startSample_);
        const float ex = sampleToX(endSample_);

        const float distStart = std::abs(mx - sx);
        const float distEnd   = std::abs(mx - ex);

        if (distStart <= 8.f && distStart <= distEnd)
            dragTarget_ = 1;
        else if (distEnd <= 8.f)
            dragTarget_ = 2;
        else
            dragTarget_ = 0;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragTarget_ == 0 || pcm_.empty()) return;

        const float mx      = static_cast<float>(e.x) - static_cast<float>(waveformBounds_.getX());
        const int   total   = static_cast<int>(pcm_.size());
        const float ratio   = juce::jlimit(0.f, 1.f, mx / static_cast<float>(waveformBounds_.getWidth()));
        const int   sample  = static_cast<int>(ratio * static_cast<float>(total));

        if (dragTarget_ == 1)
            startSample_ = juce::jlimit(0, endSample_ - 1, sample);
        else
            endSample_   = juce::jlimit(startSample_ + 1, total, sample);

        updateInfoLabel();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override { dragTarget_ = 0; }

private:
    void timerCallback() override { repaint(); }

    std::vector<float>  pcm_;
    double              sampleRate_;
    int                 startSample_;
    int                 endSample_;
    int                 dragTarget_ { 0 };  // 0=none 1=start 2=end

    std::vector<float>  envelope_;          // peak envelope (one value per pixel column)
    juce::Rectangle<int> waveformBounds_;

    juce::TextButton    playBtn_;
    juce::TextButton    applyBtn_;
    juce::TextButton    cancelBtn_;
    juce::Label         infoLabel_;

    // ── Build peak envelope (one entry per display bin) ──────────────────────
    void buildEnvelope()
    {
        constexpr int kBins = 800;
        if (pcm_.empty()) return;

        envelope_.resize(static_cast<std::size_t>(kBins));
        const int total = static_cast<int>(pcm_.size());

        for (int b = 0; b < kBins; ++b)
        {
            const int first = static_cast<int>(
                static_cast<long long>(b)     * total / kBins);
            const int last  = static_cast<int>(
                static_cast<long long>(b + 1) * total / kBins);
            float peak = 0.f;
            for (int i = first; i < last && i < total; ++i)
                peak = std::max(peak, std::abs(pcm_[static_cast<std::size_t>(i)]));
            envelope_[static_cast<std::size_t>(b)] = peak;
        }
    }

    // ── Convert sample index → x pixel (relative to waveform area left) ─────
    float sampleToX(int sample) const noexcept
    {
        if (pcm_.empty()) return 0.f;
        return static_cast<float>(sample) / static_cast<float>(pcm_.size())
               * static_cast<float>(waveformBounds_.getWidth());
    }

    void updateInfoLabel()
    {
        const double sr    = sampleRate_;
        const double inSec = static_cast<double>(startSample_) / sr;
        const double outSec= static_cast<double>(endSample_)   / sr;
        const double durSec= outSec - inSec;

        const auto fmt = [](double s) {
            return juce::String(s, 3) + "s";
        };

        infoLabel_.setText(
            "IN: "  + fmt(inSec)  + "  (" + juce::String(startSample_) + " spl)"
            "   |   "
            "OUT: " + fmt(outSec) + "  (" + juce::String(endSample_)   + " spl)"
            "   |   "
            "Dur: " + fmt(durSec),
            juce::dontSendNotification);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleEditorComponent)
};

} // namespace ui
