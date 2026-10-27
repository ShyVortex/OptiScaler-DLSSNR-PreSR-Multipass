#pragma once

#include <iterator>
#include <cstdint>
#include <optional>

// NR model preset labels for the menu, kept free of ImGui so tests can include them.
// Every pass can override the model: PassProfiles applies Pass{N}Preset for all passes.
namespace DlssNr::MenuPresets
{
inline constexpr const char* Names[] = { "Automatic", "Original", "Optimized" };
inline constexpr uint32_t Count = static_cast<uint32_t>(std::size(Names));
inline constexpr const char* Inherit = "Inherit pass 1";
inline constexpr const char* Unavailable = "Unavailable preset";

constexpr bool Inherited(bool allowInherit, const std::optional<uint32_t>& preset)
{
    return allowInherit && !preset.has_value();
}

constexpr const char* Preview(bool allowInherit, const std::optional<uint32_t>& preset)
{
    if (Inherited(allowInherit, preset))
        return Inherit;
    const uint32_t value = preset.value_or(0u);
    return value < Count ? Names[value] : Unavailable;
}
} // namespace DlssNr::MenuPresets
