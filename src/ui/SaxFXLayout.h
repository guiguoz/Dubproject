#pragma once

namespace ui::SaxFXLayout {

// ─────────────────────────────────────────────────────────────────────────────
// Spacing scale (base 4px, multiples of 4)
// ─────────────────────────────────────────────────────────────────────────────
constexpr int xxs = 2;
constexpr int xs  = 4;
constexpr int sm  = 8;
constexpr int md  = 16;  // ← default gap between elements
constexpr int lg  = 24;
constexpr int xl  = 32;
constexpr int xxl = 48;

// ─────────────────────────────────────────────────────────────────────────────
// Component sizing
// ⚠ buttonHeight must stay ≥ 40 for live/touch use
// slot_height is NOT used — StepSequencerPanel uses dynamic height
// ─────────────────────────────────────────────────────────────────────────────
constexpr int buttonHeight = 44;  // minimum touch-safe height

// ─────────────────────────────────────────────────────────────────────────────
// Border radii
// ─────────────────────────────────────────────────────────────────────────────
constexpr float radiusSm  =  4.f;  // small elements (badges, indicators)
constexpr float radiusMd  =  6.f;  // buttons, inputs
constexpr float radiusLg  = 10.f;  // panels, slots

// ─────────────────────────────────────────────────────────────────────────────
// Border / stroke widths
// ─────────────────────────────────────────────────────────────────────────────
constexpr float borderThin   = 1.f;
constexpr float borderMedium = 2.f;
constexpr float borderThick  = 3.f;
constexpr float glowSpread   = 6.f;  // outer glow spread radius

} // namespace ui::SaxFXLayout
