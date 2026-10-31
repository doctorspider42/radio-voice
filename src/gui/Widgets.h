#pragma once

#include <imgui.h>

#include "gui/Theme.h"

namespace rv::gui {

/// Hands the widgets the fonts they draw labels and readouts with. Called by
/// the application whenever the fonts are (re)built.
void setWidgetFonts(const Fonts& fonts);

/// Rotary control, drawn as a piece of hardware: a tick ring, a lit value arc,
/// a shaded body and a small LCD readout underneath.
///
/// Rotary rather than a slider because a channel strip needs a dozen controls
/// in a small area, and a knob is square. Vertical drag changes the value,
/// Shift gives fine resolution, and double-click returns it to `defaultValue` -
/// the conventions every audio application shares.
///
/// `diameter` is in device pixels. Returns true on the frames where the value
/// changed.
bool knob(const char* label, float* value, float minimum, float maximum,
          const char* format, float defaultValue, float diameter = 64.0f);

/// Logarithmic variant, for frequencies: a linear knob spends most of its
/// travel above 5 kHz and is unusable for setting an 80 Hz high-pass.
bool knobLog(const char* label, float* value, float minimum, float maximum,
             const char* format, float defaultValue, float diameter = 64.0f);

/// Peak plus RMS bar with a dB scale. `peakDb` drives the thin bright line,
/// `rmsDb` the filled body. The fill runs along one fixed gradient, so a colour
/// always means the same level.
void levelMeter(const char* id, float peakDb, float rmsDb, ImVec2 size,
                bool horizontal = false);

/// Downward-growing meter for gain reduction, which is always negative.
void gainReductionMeter(const char* id, float reductionDb, float rangeDb, ImVec2 size);

/// Analogue-style gain-reduction meter: a needle over an arc scale.
void needleMeter(const char* id, float reductionDb, float rangeDb, ImVec2 size);

/// Section title with an optional enable switch on the right.
/// Returns true when the switch was toggled.
bool sectionHeader(const char* title, bool* enabled = nullptr);

/// Rounded status badge with a lit dot, in letter-spaced capitals.
void statusPill(const char* text, ImU32 colour);

/// Toggle switch, more legible at a glance than a checkbox for on/off state
/// that changes the signal path.
bool toggleSwitch(const char* id, bool* value);

/// Round power button, lit while `on`. Returns true when clicked.
bool powerButton(const char* id, bool on, float size);

/// A row of mutually exclusive choices in one sunken track. Returns true when
/// the selection changed.
bool segmented(const char* id, const char* const* labels, int count, int* selected,
               float height = 0.0f);

/// Pill-shaped toggle for a secondary option. Returns true when clicked.
bool chipButton(const char* label, bool lit, ImU32 litColour = theme::kAccent);

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

// --- drawing helpers --------------------------------------------------------
//
// Shared by the widgets and by the larger displays the application draws
// itself, so that every well, faceplate and glow is the same well, faceplate
// and glow.

/// Text with extra space between letters, for the small capitals labels.
/// Uses the current font.
float spacedTextWidth(const char* text, float spacing);
void  drawSpacedText(ImDrawList* draw, ImVec2 position, ImU32 colour, const char* text,
                     float spacing);

/// Letter-spaced label in the caps font, as an item.
void capsLabel(const char* text, ImU32 colour = theme::kTextFaint);

/// A sunken display well: near-black, a shadow at the top edge, and a lit lip
/// along the bottom.
void drawInset(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding);

/// A raised surface lit from above.
void drawFaceplate(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding,
                   ImU32 top = theme::kFaceTop, ImU32 bottom = theme::kFaceBottom);

/// A polyline with a soft glow under it.
void drawGlowLine(ImDrawList* draw, const ImVec2* points, int count, ImU32 colour,
                  float thickness);

/// The RadioVoice microphone mark at `size` pixels, top-left at `origin`.
void drawLogo(ImDrawList* draw, ImVec2 origin, float size, bool withArcs = true);

/// Colour along the level gradient for a position from 0 (silence) to 1 (full scale).
ImU32 levelGradient(float t);

} // namespace rv::gui
