#pragma once

namespace dsp {

struct MusicContext
{
    float bpm     = 120.f;
    int   keyRoot = 0;     // 0=C … 11=B ; -1 = unknown
    bool  isMajor = true;

    enum class Style { None, Jazz, Funk, Rock, Electro, DubTechno };
    Style style = Style::None;
};

} // namespace dsp
