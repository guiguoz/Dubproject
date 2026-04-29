#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>

namespace dsp {

enum class SoloPreset : int { Off = 0, Prudent = 1, Dub = 2 };

class SoloAssistant
{
public:
    void configure(int rootNote, int scaleType) noexcept
    {
        root_      = rootNote;
        scaleType_ = scaleType;
        lastNote_  = -1;
        buildScaleMap();
    }

    void setPreset(SoloPreset p) noexcept { preset_ = p; }
    SoloPreset preset() const noexcept    { return preset_; }

    void recordNote(int midiNote) noexcept { lastNote_ = midiNote; }

    std::vector<int> suggest(int minNote = 48, int maxNote = 84) const noexcept
    {
        if (preset_ == SoloPreset::Off || lastNote_ < 0 || scaleNotes_.empty())
            return {};

        const int count   = (preset_ == SoloPreset::Prudent) ? 2 : 3;
        const int lastDeg = degreeOf(lastNote_);
        if (lastDeg < 0) return {};

        const int N = static_cast<int>(scaleNotes_.size());
        std::vector<std::pair<float, int>> scored;
        scored.reserve(32);

        for (int note = minNote; note <= maxNote; ++note)
        {
            const int deg = degreeOf(note);
            if (deg < 0) continue;
            float w = transitionWeight(lastDeg, deg, N);
            if (deg == 0) w += 0.05f;  // tonic resolution bonus
            scored.push_back({ w, note });
        }

        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        std::vector<int> result;
        result.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count && i < static_cast<int>(scored.size()); ++i)
            result.push_back(scored[static_cast<std::size_t>(i)].second);
        return result;
    }

private:
    // Scale intervals matching PianoKeyboardPanel::ScaleType order
    static const int* scaleIntervals(int t, int& len) noexcept
    {
        static constexpr int kMaj[7]  = { 0,2,4,5,7,9,11 };
        static constexpr int kMin[7]  = { 0,2,3,5,7,8,10 };
        static constexpr int kPM[5]   = { 0,2,4,7,9 };
        static constexpr int kPm[5]   = { 0,3,5,7,10 };
        static constexpr int kBl[6]   = { 0,3,5,6,7,10 };
        static constexpr int kDor[7]  = { 0,2,3,5,7,9,10 };
        switch (t) {
            case 0:  len=7; return kMaj;
            case 1:  len=7; return kMin;
            case 2:  len=5; return kPM;
            case 3:  len=5; return kPm;
            case 4:  len=6; return kBl;
            case 5:  len=7; return kDor;
            default: len=7; return kMaj;
        }
    }

    void buildScaleMap() noexcept
    {
        int len = 0;
        const int* iv = scaleIntervals(scaleType_, len);
        scaleNotes_.clear();
        for (int i = 0; i < len; ++i)
            scaleNotes_.push_back((root_ + iv[i]) % 12);
    }

    int degreeOf(int midiNote) const noexcept
    {
        const int pc = ((midiNote % 12) + 12) % 12;
        for (int i = 0; i < static_cast<int>(scaleNotes_.size()); ++i)
            if (scaleNotes_[static_cast<std::size_t>(i)] == pc) return i;
        return -1;
    }

    float transitionWeight(int from, int to, int scaleLen) const noexcept
    {
        const int dist = std::abs(to - from);
        const int cd   = std::min(dist, scaleLen - dist);
        if (preset_ == SoloPreset::Prudent) {
            switch (cd) {
                case 0:  return 0.05f;
                case 1:  return 0.35f;
                case 2:  return 0.25f;
                case 3:  return 0.15f;
                default: return 0.04f / static_cast<float>(cd);
            }
        }
        // Dub: répétition forte, stepwise solide
        switch (cd) {
            case 0:  return 0.30f;
            case 1:  return 0.25f;
            case 2:  return 0.18f;
            case 3:  return 0.12f;
            default: return 0.05f / static_cast<float>(cd);
        }
    }

    SoloPreset        preset_    { SoloPreset::Off };
    int               root_      { 0 };
    int               scaleType_ { 0 };
    int               lastNote_  { -1 };
    std::vector<int>  scaleNotes_;
};

} // namespace dsp
