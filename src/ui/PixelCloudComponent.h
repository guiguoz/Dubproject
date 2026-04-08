#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>

namespace ui {

/**
 * Nuage de particules animé — indicateur d'état de l'IA.
 *
 * États :
 *   Disabled → rouge   (IA inactive)
 *   Working  → orange  (traitement en cours)
 *   Active   → vert    (mix appliqué)
 *
 * Transitions douces (~500 ms). Particules à glow néon style.
 */
class PixelCloudComponent : public juce::Component, private juce::Timer
{
public:
    enum class State { Disabled, Working, Active };

    PixelCloudComponent()
    {
        initParticles();
        startTimerHz(30);
    }

    ~PixelCloudComponent() override { stopTimer(); }

    /** Change l'état — déclenche une transition de couleur si différent. */
    void setState(State newState)
    {
        if (newState == state_) return;
        prevState_    = state_;
        state_        = newState;
        transitionT_  = 0.f;
    }

    State getState() const noexcept { return state_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const float w = static_cast<float>(getWidth());
        const float h = static_cast<float>(getHeight());
        if (w <= 0.f || h <= 0.f) return;

        const juce::Colour cPrev   = stateColour(prevState_);
        const juce::Colour cTarget = stateColour(state_);
        const float        t       = transitionT_;

        for (const auto& p : particles_)
        {
            // Couleur interpolée entre prev et target
            const juce::Colour base = cPrev.interpolatedWith(cTarget, t);

            // Alpha flicker : 0.35 .. 1.0
            const float rawAlpha = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(p.phase));

            const float cx = p.x * w;
            const float cy = p.y * h;
            const float r  = p.radius;

            // Halo externe (grand, très transparent)
            {
                const float hr = r * 3.2f;
                juce::ColourGradient grad(base.withAlpha(rawAlpha * 0.12f), cx, cy,
                                          base.withAlpha(0.f), cx + hr, cy, true);
                g.setGradientFill(grad);
                g.fillEllipse(cx - hr, cy - hr, hr * 2.f, hr * 2.f);
            }
            // Halo moyen
            {
                const float hr = r * 1.8f;
                juce::ColourGradient grad(base.withAlpha(rawAlpha * 0.32f), cx, cy,
                                          base.withAlpha(0.f), cx + hr, cy, true);
                g.setGradientFill(grad);
                g.fillEllipse(cx - hr, cy - hr, hr * 2.f, hr * 2.f);
            }
            // Noyau lumineux
            g.setColour(base.withAlpha(rawAlpha * 0.90f));
            g.fillEllipse(cx - r, cy - r, r * 2.f, r * 2.f);
        }
    }

    void resized() override {}

private:
    //==========================================================================
    struct Particle
    {
        float x, y;          ///< position normalisée 0..1
        float vx, vy;        ///< vitesse (unités normalisées / frame)
        float radius;        ///< rayon en pixels
        float phase;         ///< flicker phase 0..2π
        float phaseSpeed;    ///< rad/frame
    };

    static constexpr int kCount = 48;
    std::array<Particle, kCount> particles_;

    State prevState_   { State::Disabled };
    State state_       { State::Disabled };
    float transitionT_ { 1.f };   ///< 0 = début transition, 1 = terminée

    juce::Random rng_ { 42 };

    //==========================================================================
    void initParticles()
    {
        for (auto& p : particles_)
        {
            p.x          = rng_.nextFloat();
            p.y          = rng_.nextFloat();
            // Vitesse de base : lente dérive aléatoire
            p.vx         = (rng_.nextFloat() - 0.5f) * 0.003f;
            p.vy         = (rng_.nextFloat() - 0.5f) * 0.003f;
            p.radius     = 2.f + rng_.nextFloat() * 3.f;   // 2..5 px
            p.phase      = rng_.nextFloat() * juce::MathConstants<float>::twoPi;
            p.phaseSpeed = 0.05f + rng_.nextFloat() * 0.10f;
        }
    }

    static juce::Colour stateColour(State s) noexcept
    {
        switch (s)
        {
            case State::Working:  return juce::Colour(0xFFFF8800u);  // orange
            case State::Active:   return juce::Colour(0xFF00DD66u);  // vert néon
            case State::Disabled: [[fallthrough]];
            default:              return juce::Colour(0xFFCC2200u);  // rouge sombre
        }
    }

    void timerCallback() override
    {
        const bool working = (state_ == State::Working);
        const float speedMult = working ? 1.8f : 1.0f;

        for (auto& p : particles_)
        {
            p.x += p.vx * speedMult;
            p.y += p.vy * speedMult;

            // Rebond sur les bords
            if (p.x < 0.f) { p.x = 0.f; p.vx = std::abs(p.vx); }
            if (p.x > 1.f) { p.x = 1.f; p.vx = -std::abs(p.vx); }
            if (p.y < 0.f) { p.y = 0.f; p.vy = std::abs(p.vy); }
            if (p.y > 1.f) { p.y = 1.f; p.vy = -std::abs(p.vy); }

            // Avancer le flicker (un peu plus rapide en Working)
            p.phase += p.phaseSpeed * (working ? 1.5f : 1.0f);
            if (p.phase > juce::MathConstants<float>::twoPi)
                p.phase -= juce::MathConstants<float>::twoPi;
        }

        // Transition de couleur : ~15 frames (500 ms à 30 fps)
        if (transitionT_ < 1.f)
        {
            transitionT_ = juce::jmin(1.f, transitionT_ + 0.067f);
        }

        repaint();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PixelCloudComponent)
};

} // namespace ui
