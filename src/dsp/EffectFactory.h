#pragma once

#include "IEffect.h"

#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// EffectFactory
//
// Pure C++ — no JUCE dependency.
// Maps between EffectType enum and canonical string names,
// and creates concrete IEffect instances by name.
//
// Used by:
//   - ProjectLoader   (creates effects when loading a .saxfx file)
//   - ProjectSaver    (converts EffectType → string when saving)
//   - AddEffectPopup  (lists available effect types)
// ─────────────────────────────────────────────────────────────────────────────
namespace dsp {

/// Canonical string name for an effect type (used in JSON / UI).
const char* effectTypeName(EffectType t) noexcept;

/// Create a default-constructed IEffect from its string name.
/// Returns nullptr if the name is unknown.
std::unique_ptr<IEffect> createEffect(const std::string& typeName) noexcept;

/// Convenience: create by enum value.
std::unique_ptr<IEffect> createEffect(EffectType t) noexcept;

} // namespace dsp
