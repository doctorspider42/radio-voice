#pragma once

#include <imgui.h>

namespace rv::gui {

/// Colour palette, kept in one place so panels never hard-code a value.
///
/// Graphite surfaces lit by a single teal signal - the same two things the
/// application icon is made of. Everything that carries meaning in a metering
/// context gets its own hue and nothing else is allowed to use it: teal is
/// signal, violet is gain reduction, amber is a warning, red is a stop. A glance
/// at the window is then unambiguous.
namespace theme {

// --- surfaces, darkest first ------------------------------------------------
inline constexpr ImU32 kInset        = IM_COL32(0x05, 0x07, 0x0A, 0xFF); ///< Display wells.
inline constexpr ImU32 kChrome       = IM_COL32(0x08, 0x09, 0x0C, 0xFF);
inline constexpr ImU32 kBackground   = IM_COL32(0x0A, 0x0C, 0x10, 0xFF);
inline constexpr ImU32 kPanel        = IM_COL32(0x10, 0x13, 0x1A, 0xFF);
inline constexpr ImU32 kPanelRaised  = IM_COL32(0x16, 0x1A, 0x22, 0xFF);
inline constexpr ImU32 kControl      = IM_COL32(0x1D, 0x22, 0x2C, 0xFF);
inline constexpr ImU32 kBorder       = IM_COL32(0x22, 0x28, 0x33, 0xFF);
inline constexpr ImU32 kBorderBright = IM_COL32(0x2D, 0x34, 0x41, 0xFF);

/// The plugin faceplates and the header strip are lit from above, like the
/// icon tile: a vertical fade between these pairs plus a one-pixel highlight.
inline constexpr ImU32 kFaceTop      = IM_COL32(0x19, 0x1D, 0x25, 0xFF);
inline constexpr ImU32 kFaceBottom   = IM_COL32(0x0F, 0x12, 0x16, 0xFF);
inline constexpr ImU32 kStripTop     = IM_COL32(0x1E, 0x23, 0x2C, 0xFF);
inline constexpr ImU32 kStripBottom  = IM_COL32(0x15, 0x19, 0x1F, 0xFF);
inline constexpr ImU32 kHighlight    = IM_COL32(0xFF, 0xFF, 0xFF, 0x10);
inline constexpr ImU32 kShadow       = IM_COL32(0x00, 0x00, 0x00, 0x8C);

// --- text -------------------------------------------------------------------
inline constexpr ImU32 kText       = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
inline constexpr ImU32 kTextDim    = IM_COL32(0x9A, 0xA3, 0xB2, 0xFF);
inline constexpr ImU32 kTextFaint  = IM_COL32(0x7D, 0x86, 0x96, 0xFF);
inline constexpr ImU32 kTextGhost  = IM_COL32(0x4A, 0x53, 0x61, 0xFF); ///< Axis labels only.

// --- the signal colour --------------------------------------------------------
inline constexpr ImU32 kAccentLight = IM_COL32(0x7A, 0xF0, 0xE4, 0xFF);
inline constexpr ImU32 kAccent      = IM_COL32(0x3D, 0xD1, 0xC4, 0xFF);
inline constexpr ImU32 kAccentDim   = IM_COL32(0x27, 0x86, 0x7E, 0xFF);
inline constexpr ImU32 kAccentFaint = IM_COL32(0x10, 0x2E, 0x2C, 0xFF);
inline constexpr ImU32 kAccentWash  = IM_COL32(0x3D, 0xD1, 0xC4, 0x1E);
inline constexpr ImU32 kGlow        = IM_COL32(0x3D, 0xD1, 0xC4, 0x40);

/// Metering semantics. Signal present is the accent itself - one hue fewer to
/// learn, and the one the icon already taught.
inline constexpr ImU32 kSignal         = kAccent;
inline constexpr ImU32 kWarning        = IM_COL32(0xE8, 0xB0, 0x4A, 0xFF);
inline constexpr ImU32 kDanger         = IM_COL32(0xF0, 0x64, 0x5A, 0xFF);
inline constexpr ImU32 kReduction      = IM_COL32(0xA5, 0x8B, 0xF0, 0xFF);
inline constexpr ImU32 kReductionLight = IM_COL32(0xC7, 0xB6, 0xF7, 0xFF);

inline constexpr ImU32 kSpectrum     = IM_COL32(0x3D, 0xD1, 0xC4, 0x38);
inline constexpr ImU32 kSpectrumLine = IM_COL32(0x3D, 0xD1, 0xC4, 0x90);
inline constexpr ImU32 kCurve        = kAccentLight;
inline constexpr ImU32 kGrid         = IM_COL32(0x12, 0x18, 0x20, 0xFF);
inline constexpr ImU32 kGridStrong   = IM_COL32(0x1F, 0x26, 0x30, 0xFF);

inline ImVec4 toVec4(ImU32 colour) { return ImGui::ColorConvertU32ToFloat4(colour); }

/// `colour` with its alpha replaced, for glows and washes of a palette entry.
inline ImU32 withAlpha(ImU32 colour, float alpha)
{
    ImVec4 v = toVec4(colour);
    v.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

} // namespace theme

/// Fonts loaded at start-up. Falls back to the built-in font when the system
/// fonts are unavailable, so the application never fails to draw.
struct Fonts {
    ImFont* regular   = nullptr;
    ImFont* medium    = nullptr; ///< Slightly larger, for values that matter.
    ImFont* small     = nullptr;
    ImFont* heading   = nullptr;
    ImFont* caps      = nullptr; ///< Bold and small, for letter-spaced labels.
    ImFont* title     = nullptr; ///< Bold, for module names.
    ImFont* mono      = nullptr; ///< Every number, so readouts do not jitter.
    ImFont* monoLarge = nullptr; ///< Big readouts on the plugin faceplates.
};

/// Applies the palette, spacing and rounding to the current ImGui context.
void applyTheme();

/// Loads Segoe UI and Consolas at the sizes the layout uses, scaled for `dpiScale`.
Fonts loadFonts(float dpiScale);

} // namespace rv::gui
