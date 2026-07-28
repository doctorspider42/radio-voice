#pragma once

#include <imgui.h>

namespace rv::gui {

/// Rotary control.
///
/// Rotary rather than a slider because a channel strip needs a dozen controls
/// in a small area, and a knob is square. Vertical drag changes the value,
/// Shift gives fine resolution, and double-click returns it to `defaultValue` -
/// the conventions every audio application shares.
///
/// Returns true on the frames where the value changed.
bool knob(const char* label, float* value, float minimum, float maximum,
          const char* format, float defaultValue, float diameter = 46.0f);

/// Logarithmic variant, for frequencies: a linear knob spends most of its
/// travel above 5 kHz and is unusable for setting an 80 Hz high-pass.
bool knobLog(const char* label, float* value, float minimum, float maximum,
             const char* format, float defaultValue, float diameter = 46.0f);

/// Peak plus RMS bar with a dB scale. `peakDb` drives the thin bright line,
/// `rmsDb` the filled body.
void levelMeter(const char* id, float peakDb, float rmsDb, ImVec2 size,
                bool horizontal = false);

/// Downward-growing meter for gain reduction, which is always negative.
void gainReductionMeter(const char* id, float reductionDb, float rangeDb, ImVec2 size);

/// Section title with an optional enable switch on the right.
/// Returns true when the switch was toggled.
bool sectionHeader(const char* title, bool* enabled = nullptr);

/// Small rounded status badge.
void statusPill(const char* text, ImU32 colour);

/// Toggle switch, more legible at a glance than a checkbox for on/off state
/// that changes the signal path.
bool toggleSwitch(const char* id, bool* value);

/// Square button carrying a drawn cog, for the options menu.
///
/// The cog is drawn rather than typed: the interface font is loaded with the
/// Latin ranges only, and U+2699 is not among them. `tint` colours it - the
/// caller uses that to carry a warning through a button with no room for text.
/// `lit` keeps the button looking pressed while its menu is open.
///
/// Returns true on the frame it was clicked.
bool gearButton(const char* id, float size, ImU32 tint, bool lit = false);

/// Right-aligned dim caption on the current line.
void rightLabel(const char* text, ImU32 colour);

/// Key/value row used throughout the status panels.
void infoRow(const char* key, const char* value, ImU32 valueColour);

} // namespace rv::gui
