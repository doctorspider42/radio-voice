#pragma once

#include <imgui.h>

namespace rv::gui {

/// Colour palette, kept in one place so panels never hard-code a value.
///
/// The palette is built around a dark neutral base with a single saturated
/// accent. Everything that carries meaning in a metering context - signal
/// present, gain reduction, clipping - gets its own hue, and nothing else is
/// allowed to use those hues, so a glance at the window is unambiguous.
namespace theme {

inline constexpr ImU32 kBackground   = IM_COL32(0x12, 0x14, 0x18, 0xFF);
inline constexpr ImU32 kPanel        = IM_COL32(0x1A, 0x1D, 0x23, 0xFF);
inline constexpr ImU32 kPanelRaised  = IM_COL32(0x22, 0x26, 0x2E, 0xFF);
inline constexpr ImU32 kBorder       = IM_COL32(0x2C, 0x31, 0x3B, 0xFF);
inline constexpr ImU32 kBorderBright = IM_COL32(0x3A, 0x41, 0x4E, 0xFF);

inline constexpr ImU32 kText       = IM_COL32(0xE6, 0xE9, 0xEF, 0xFF);
inline constexpr ImU32 kTextDim    = IM_COL32(0x8C, 0x94, 0xA2, 0xFF);
inline constexpr ImU32 kTextFaint  = IM_COL32(0x5A, 0x62, 0x70, 0xFF);

inline constexpr ImU32 kAccent      = IM_COL32(0x3D, 0xD1, 0xC4, 0xFF);
inline constexpr ImU32 kAccentDim   = IM_COL32(0x27, 0x86, 0x7E, 0xFF);
inline constexpr ImU32 kAccentFaint = IM_COL32(0x1B, 0x4A, 0x47, 0xFF);

/// Metering semantics: green below nominal, amber approaching full scale,
/// red at or above it, violet for gain reduction.
inline constexpr ImU32 kSignal    = IM_COL32(0x4C, 0xC9, 0x7A, 0xFF);
inline constexpr ImU32 kWarning   = IM_COL32(0xE0, 0xA3, 0x3E, 0xFF);
inline constexpr ImU32 kDanger    = IM_COL32(0xE0, 0x5A, 0x4E, 0xFF);
inline constexpr ImU32 kReduction = IM_COL32(0x9B, 0x7F, 0xE8, 0xFF);

inline constexpr ImU32 kSpectrum     = IM_COL32(0x3D, 0xD1, 0xC4, 0x50);
inline constexpr ImU32 kSpectrumLine = IM_COL32(0x3D, 0xD1, 0xC4, 0xB0);
inline constexpr ImU32 kCurve        = IM_COL32(0xFF, 0xC4, 0x66, 0xFF);
inline constexpr ImU32 kGrid         = IM_COL32(0x2A, 0x2F, 0x38, 0xFF);
inline constexpr ImU32 kGridStrong   = IM_COL32(0x38, 0x3E, 0x4A, 0xFF);

inline ImVec4 toVec4(ImU32 colour) { return ImGui::ColorConvertU32ToFloat4(colour); }

} // namespace theme

/// Fonts loaded at start-up. Falls back to the built-in font when the system
/// fonts are unavailable, so the application never fails to draw.
struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium  = nullptr; ///< Slightly larger, for values that matter.
    ImFont* small   = nullptr;
    ImFont* heading = nullptr;
};

/// Applies the palette, spacing and rounding to the current ImGui context.
void applyTheme();

/// Loads Segoe UI at the sizes the layout uses, scaled for `dpiScale`.
Fonts loadFonts(float dpiScale);

} // namespace rv::gui
