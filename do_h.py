import re

with open("src/dsp/Sampler.h", "r", encoding="utf-8") as f:
    code = f.read()

# Update SampleSlot
code = re.sub(
    r"struct SampleSlot.*?std::atomic<bool>\s+muted\s*\{\s*false\s*\};\s*// silenced but keeps playing\n\};",
    """struct SampleSlot
{
    std::vector<float> data[2];          // Double buffer for smooth sample changes
    int                sampleCount[2] { 0, 0 };
    std::atomic<int>   activeDataIdx{ 0 };

    std::atomic<float> gain        { 1.0f };
    std::atomic<bool>  loopEnabled { false };
    std::atomic<bool>  oneShot     { true };
    std::atomic<bool>  loaded      { false }; // set after data is ready
    std::atomic<bool>  muted       { false }; // silenced but keeps playing
};""",
    code, flags=re.DOTALL
)

# Update PlayState
code = re.sub(
    r"    struct PlayState.*?bool\s+stopAfterFadeOut\{\s*false\s*\};\n    \};",
    """    struct VoiceState
    {
        bool playing{ false };
        int  dataIdx{ 0 };
        int  readPos{ 0 };
        int  fadeIn{ 0 };
        int  fadeOut{ 0 };
        int  fadeOutTotal{ 256 };
        bool retriggering{ false };
        bool stopAfterFadeOut{ false };
    };

    struct PlayState
    {
        std::atomic<bool> triggerPending  { false };
        std::atomic<bool> stopPending     { false };
        std::atomic<bool> quantTrigPending{ false };
        std::atomic<int>  quantDiv        { static_cast<int>(GridDiv::Quarter) };
        
        VoiceState voices[2];
        int currentVoice{ 0 };
    };""",
    code, flags=re.DOTALL
)

with open("src/dsp/Sampler.h", "w", encoding="utf-8") as f:
    f.write(code)

print("Sampler.h modified successfully.")
