#include "gui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>

#include <imgui_internal.h>

#include "gui/Theme.h"

namespace rv::gui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Knobs sweep 270 degrees, leaving a gap at the bottom so the extremes are
/// visually distinct rather than meeting behind the control.
constexpr float kArcStart = 0.75f * kPi;
constexpr float kArcEnd   = 2.25f * kPi;

/// Meter range. -60 dB is below any usable signal; full scale is the top,
/// because nothing above it reaches the cable intact.
constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb = 0.0f;

Fonts g_fonts{};

float meterPosition(float db)
{
    return std::clamp((db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb), 0.0f, 1.0f);
}

ImU32 lerpColour(ImU32 a, ImU32 b, float t)
{
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

/// The level gradient's stops: teal for signal, warming to amber near the top
/// and red at full scale.
struct Stop {
    float t;
    ImU32 colour;
};
constexpr Stop kLevelStops[] = {
    {0.00f, IM_COL32(0x1F, 0x7F, 0x77, 0xFF)},
    {0.62f, theme::kAccent},
    {0.78f, theme::kAccentLight},
    {0.89f, theme::kWarning},
    {0.97f, theme::kDanger},
    {1.00f, theme::kDanger},
};

ImFont* fontOr(ImFont* font)
{
    return font ? font : ImGui::GetFont();
}

/// Draws a rounded, horizontally graded bar from `min` to `max`, where the bar
/// covers `[0, t]` of a gradient laid out over `fullWidth`.
void drawGradientBar(ImDrawList* draw, ImVec2 min, ImVec2 max, float fullWidth, float alpha = 1.0f)
{
    const float width = max.x - min.x;
    if (width <= 0.5f)
        return;

    for (size_t i = 0; i + 1 < std::size(kLevelStops); ++i) {
        const float x0 = min.x + kLevelStops[i].t * fullWidth;
        const float x1 = min.x + kLevelStops[i + 1].t * fullWidth;
        if (x0 >= max.x)
            break;
        const float right = std::min(x1, max.x);
        const float span  = std::max(1.0f, x1 - x0);
        const ImU32 c0    = theme::withAlpha(kLevelStops[i].colour, alpha);
        const ImU32 c1    = theme::withAlpha(lerpColour(kLevelStops[i].colour, kLevelStops[i + 1].colour,
                                                        (right - x0) / span),
                                             alpha);
        draw->AddRectFilledMultiColor(ImVec2(x0, min.y), ImVec2(right, max.y), c0, c1, c1, c0);
    }
}

bool knobImpl(const char* label, float* value, float minimum, float maximum,
              const char* format, float defaultValue, float diameter, bool logarithmic)
{
    ImGui::PushID(label);

    ImFont* capsFont = fontOr(g_fonts.caps);
    ImFont* monoFont = fontOr(g_fonts.mono);

    const float labelSpacing = diameter * 0.03f;
    const float labelHeight  = capsFont->LegacySize;
    const float lcdHeight    = monoFont->LegacySize + 8.0f;
    const ImVec2 origin      = ImGui::GetCursorScreenPos();

    char initialText[32];
    std::snprintf(initialText, sizeof(initialText), format, static_cast<double>(*value));

    ImGui::PushFont(capsFont, capsFont->LegacySize);
    const float labelWidth = spacedTextWidth(label, labelSpacing);
    ImGui::PopFont();
    ImGui::PushFont(monoFont, monoFont->LegacySize);
    const float valueWidth = ImGui::CalcTextSize(initialText).x + 16.0f;
    ImGui::PopFont();

    // The item is as wide as its widest part, not just the dial, so that a long
    // label never runs into the knob beside it.
    const float itemWidth = std::max({diameter, labelWidth, valueWidth});

    // Wrap onto the next line when this knob will not fit on the current one,
    // rather than letting the last knobs of a row be clipped out of sight.
    if (ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 1.0f &&
        ImGui::GetContentRegionAvail().x < itemWidth) {
        ImGui::NewLine();
    }

    const float gap = diameter * 0.08f;
    const ImVec2 totalSize(itemWidth, labelHeight + gap + diameter + gap + lcdHeight);

    ImGui::InvisibleButton("##knob", totalSize);
    const bool hovered  = ImGui::IsItemHovered();
    const bool active   = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;

    bool changed = false;

    auto toNormalized = [&](float v) {
        if (logarithmic) {
            const float lo = std::log(std::max(1.0e-6f, minimum));
            const float hi = std::log(std::max(1.0e-6f, maximum));
            return (std::log(std::max(1.0e-6f, v)) - lo) / (hi - lo);
        }
        return (v - minimum) / (maximum - minimum);
    };
    auto fromNormalized = [&](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        if (logarithmic) {
            const float lo = std::log(std::max(1.0e-6f, minimum));
            const float hi = std::log(std::max(1.0e-6f, maximum));
            return std::exp(lo + t * (hi - lo));
        }
        return minimum + t * (maximum - minimum);
    };

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImGuiIO& io = ImGui::GetIO();
        // 200 pixels of travel covers the full range; Shift stretches that to
        // 2000 for setting a value precisely.
        const float travel = io.KeyShift ? 2000.0f : 200.0f;
        const float delta  = -io.MouseDelta.y / travel;

        if (delta != 0.0f) {
            *value  = std::clamp(fromNormalized(toNormalized(*value) + delta), minimum, maximum);
            changed = true;
        }
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *value  = std::clamp(defaultValue, minimum, maximum);
        changed = true;
    }

    // --- draw -------------------------------------------------------------
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 centre(origin.x + itemWidth * 0.5f,
                        origin.y + labelHeight + gap + diameter * 0.5f);
    const float  r     = diameter * 0.5f;
    const float  t     = std::clamp(toNormalized(*value), 0.0f, 1.0f);
    const float  angle = kArcStart + t * (kArcEnd - kArcStart);

    // Tick ring: a mark every 9 degrees, a longer one every 45.
    for (int i = 0; i <= 30; ++i) {
        const float a     = kArcStart + (kArcEnd - kArcStart) * static_cast<float>(i) / 30.0f;
        const float inner = r * (i % 5 == 0 ? 0.89f : 0.93f);
        draw->AddLine(ImVec2(centre.x + std::cos(a) * inner, centre.y + std::sin(a) * inner),
                      ImVec2(centre.x + std::cos(a) * r * 0.98f,
                             centre.y + std::sin(a) * r * 0.98f),
                      IM_COL32(0x3E, 0x45, 0x53, 0xFF), std::max(1.0f, r * 0.03f));
    }

    // Value arc in a sunken track, with a glow under the lit part.
    const float arcRadius = r * 0.80f;
    const float arcWidth  = std::max(2.5f, r * 0.09f);
    draw->PathArcTo(centre, arcRadius, kArcStart, kArcEnd, 64);
    draw->PathStroke(theme::kInset, arcWidth);

    const ImU32 arcColour = disabled ? theme::kTextGhost
                          : active   ? theme::kAccentLight
                                     : theme::kAccent;
    if (t > 0.002f) {
        if (!disabled) {
            draw->PathArcTo(centre, arcRadius, kArcStart, angle, 64);
            draw->PathStroke(theme::withAlpha(theme::kAccent, 0.22f), arcWidth * 2.6f);
        }
        draw->PathArcTo(centre, arcRadius, kArcStart, angle, 64);
        draw->PathStroke(arcColour, arcWidth);
    }

    // Body: a soft drop shadow, then concentric discs offset towards the top
    // left, which reads as a shaded dome without needing a radial gradient.
    const float body = r * 0.62f;
    draw->AddCircleFilled(ImVec2(centre.x, centre.y + r * 0.06f), body * 1.06f,
                          theme::withAlpha(IM_COL32(0, 0, 0, 255), 0.55f), 48);
    constexpr int kShades = 10;
    for (int i = 0; i < kShades; ++i) {
        const float s = static_cast<float>(i) / (kShades - 1);
        const ImVec2 c(centre.x - body * 0.22f * s, centre.y - body * 0.26f * s);
        draw->AddCircleFilled(c, body * (1.0f - 0.62f * s),
                              lerpColour(IM_COL32(0x0D, 0x10, 0x14, 0xFF),
                                         hovered || active ? IM_COL32(0x4E, 0x57, 0x66, 0xFF)
                                                           : IM_COL32(0x43, 0x4B, 0x59, 0xFF),
                                         s * s),
                              40);
    }
    draw->AddCircle(centre, body, IM_COL32(0x07, 0x09, 0x0C, 0xFF), 48, 1.2f);

    const float cap = r * 0.44f;
    draw->AddCircleFilled(centre, cap, IM_COL32(0x1F, 0x24, 0x2D, 0xFF), 40);
    draw->PathArcTo(centre, cap - 0.5f, kPi * 1.1f, kPi * 1.9f, 24);
    draw->PathStroke(theme::kHighlight, 1.0f);

    const ImU32 pointerColour = disabled ? theme::kTextFaint : theme::kAccentLight;
    const ImVec2 p0(centre.x + std::cos(angle) * r * 0.18f, centre.y + std::sin(angle) * r * 0.18f);
    const ImVec2 p1(centre.x + std::cos(angle) * r * 0.40f, centre.y + std::sin(angle) * r * 0.40f);
    const float  pointerWidth = std::max(2.0f, r * 0.065f);
    if (!disabled)
        draw->AddLine(p0, p1, theme::withAlpha(theme::kAccentLight, 0.3f), pointerWidth * 2.4f);
    draw->AddLine(p0, p1, pointerColour, pointerWidth);

    // Label above, letter-spaced.
    ImGui::PushFont(capsFont, capsFont->LegacySize);
    drawSpacedText(draw, ImVec2(origin.x + (itemWidth - labelWidth) * 0.5f, origin.y),
                   hovered || active ? theme::kText : theme::kTextDim, label, labelSpacing);
    ImGui::PopFont();

    // Readout below, in a small LCD. Re-formatted after the drag so it is never
    // a frame stale.
    char valueText[32];
    std::snprintf(valueText, sizeof(valueText), format, static_cast<double>(*value));
    ImGui::PushFont(monoFont, monoFont->LegacySize);
    const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
    const float  lcdWidth  = std::max(valueSize.x + 16.0f, diameter * 0.9f);
    const ImVec2 lcdMin(origin.x + (itemWidth - lcdWidth) * 0.5f, centre.y + r + gap);
    const ImVec2 lcdMax(lcdMin.x + lcdWidth, lcdMin.y + lcdHeight);
    drawInset(draw, lcdMin, lcdMax, 5.0f);
    draw->AddText(ImVec2(lcdMin.x + (lcdWidth - valueSize.x) * 0.5f,
                         lcdMin.y + (lcdHeight - valueSize.y) * 0.5f),
                  disabled ? theme::kTextFaint : theme::kAccentLight, valueText);
    ImGui::PopFont();

    ImGui::PopID();
    return changed;
}

} // namespace

void setWidgetFonts(const Fonts& fonts)
{
    g_fonts = fonts;
}

bool knob(const char* label, float* value, float minimum, float maximum,
          const char* format, float defaultValue, float diameter)
{
    return knobImpl(label, value, minimum, maximum, format, defaultValue, diameter, false);
}

bool knobLog(const char* label, float* value, float minimum, float maximum,
             const char* format, float defaultValue, float diameter)
{
    return knobImpl(label, value, minimum, maximum, format, defaultValue, diameter, true);
}

ImU32 levelGradient(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < std::size(kLevelStops); ++i) {
        if (t <= kLevelStops[i + 1].t) {
            const float span = std::max(1.0e-4f, kLevelStops[i + 1].t - kLevelStops[i].t);
            return lerpColour(kLevelStops[i].colour, kLevelStops[i + 1].colour,
                              (t - kLevelStops[i].t) / span);
        }
    }
    return kLevelStops[std::size(kLevelStops) - 1].colour;
}

void levelMeter(const char* id, float peakDb, float rmsDb, ImVec2 size, bool horizontal)
{
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##meter", size);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    const float  rounding = std::min(size.x, size.y) * 0.35f;

    drawInset(draw, origin, max, rounding);

    const float rmsT  = meterPosition(rmsDb);
    const float peakT = meterPosition(peakDb);
    const ImVec2 inner0(origin.x + 1.0f, origin.y + 1.0f);
    const ImVec2 inner1(max.x - 1.0f, max.y - 1.0f);

    if (horizontal) {
        const float width = inner1.x - inner0.x;

        // Up to the peak line the same gradient runs dimmed, under the solid
        // RMS body: the peak is the top of the same signal, not a second
        // marker wandering on its own.
        if (peakT > 0.0f) {
            drawGradientBar(draw, ImVec2(inner0.x, inner0.y + 1.0f),
                            ImVec2(inner0.x + peakT * width, inner1.y - 1.0f), width, 0.32f);
        }
        if (rmsT > 0.0f) {
            drawGradientBar(draw, ImVec2(inner0.x, inner0.y + 1.0f),
                            ImVec2(inner0.x + rmsT * width, inner1.y - 1.0f), width);
        }
        if (peakT > 0.0f) {
            const float x = inner0.x + peakT * width;
            draw->AddLine(ImVec2(x, inner0.y), ImVec2(x, inner1.y),
                          theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.35f), 5.0f);
            draw->AddLine(ImVec2(x, inner0.y), ImVec2(x, inner1.y), IM_COL32(0xF2, 0xF4, 0xF7, 0xFF), 2.0f);
        }
    } else {
        const float height = inner1.y - inner0.y;
        if (rmsT > 0.0f) {
            draw->AddRectFilled(ImVec2(inner0.x, inner1.y - rmsT * height), inner1,
                                levelGradient(rmsT), rounding);
        }
        if (peakT > 0.0f) {
            const float y = inner1.y - peakT * height;
            draw->AddLine(ImVec2(inner0.x, y), ImVec2(inner1.x, y), IM_COL32(0xF2, 0xF4, 0xF7, 0xFF), 2.0f);
        }
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("peak %.1f dB\nRMS  %.1f dB",
                          static_cast<double>(peakDb), static_cast<double>(rmsDb));
    }

    ImGui::PopID();
}

void gainReductionMeter(const char* id, float reductionDb, float rangeDb, ImVec2 size)
{
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##gr", size);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    const float  rounding = size.x * 0.5f;

    drawInset(draw, origin, max, rounding);

    // Reduction is negative and grows downward from the top of the meter,
    // which is how every compressor displays it.
    const float t = std::clamp(-reductionDb / std::max(1.0f, -rangeDb), 0.0f, 1.0f);
    if (t > 0.001f) {
        const ImVec2 fillMax(max.x - 1, origin.y + 1 + t * (size.y - 2));
        draw->AddRectFilled(ImVec2(origin.x - 2, origin.y), ImVec2(max.x + 2, fillMax.y + 2),
                            theme::withAlpha(theme::kReduction, 0.18f), rounding);
        draw->AddRectFilled(ImVec2(origin.x + 1, origin.y + 1), fillMax, theme::kReduction,
                            rounding);
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("gain reduction %.1f dB", static_cast<double>(reductionDb));

    ImGui::PopID();
}

void needleMeter(const char* id, float reductionDb, float rangeDb, ImVec2 size)
{
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##needle", size);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    drawInset(draw, origin, max, 10.0f);

    // Pivot below the visible area's centre, so the arc spans most of the width.
    const ImVec2 pivot(origin.x + size.x * 0.5f, origin.y + size.y * 0.86f);
    const float  radius = std::min(size.x * 0.52f, size.y * 0.66f);
    const float  range  = std::max(1.0f, -rangeDb);

    // 0 dB at the right, full range at the left: an analogue GR meter rests at
    // the right and swings left as it works.
    const float rightAngle = kPi * (335.0f / 180.0f);
    const float leftAngle  = kPi * (205.0f / 180.0f);
    auto angleFor = [&](float db) {
        const float t = std::clamp(-db / range, 0.0f, 1.0f);
        return rightAngle + (leftAngle - rightAngle) * t;
    };
    auto point = [&](float a, float rr) {
        return ImVec2(pivot.x + std::cos(a) * rr, pivot.y + std::sin(a) * rr);
    };

    draw->PathArcTo(pivot, radius + 3.0f, leftAngle, rightAngle, 48);
    draw->PathStroke(theme::kBorder, 2.0f);

    const float now = angleFor(reductionDb);
    if (reductionDb < -0.05f) {
        draw->PathArcTo(pivot, radius + 3.0f, now, rightAngle, 32);
        draw->PathStroke(theme::kReduction, 4.0f);
    }

    ImGui::PushFont(fontOr(g_fonts.mono), fontOr(g_fonts.mono)->LegacySize);
    const int steps = static_cast<int>(range);
    for (int d = 0; d <= steps; ++d) {
        const bool major = d % 5 == 0;
        const float a = angleFor(-static_cast<float>(d));
        draw->AddLine(point(a, radius - (major ? 10.0f : 5.0f)), point(a, radius),
                      theme::kTextFaint, major ? 1.6f : 1.0f);
        if (major) {
            char text[8];
            std::snprintf(text, sizeof(text), d == 0 ? "0" : "-%d", d);
            ImGui::PushFont(fontOr(g_fonts.small), fontOr(g_fonts.small)->LegacySize);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 at = point(a, radius - 22.0f);
            draw->AddText(ImVec2(at.x - ts.x * 0.5f, at.y - ts.y * 0.5f), theme::kTextFaint, text);
            ImGui::PopFont();
        }
    }
    ImGui::PopFont();

    const ImVec2 tip = point(now, radius - 6.0f);
    draw->AddLine(pivot, tip, theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.25f), 5.0f);
    draw->AddLine(pivot, tip, IM_COL32(0xF2, 0xF4, 0xF7, 0xFF), 2.0f);
    draw->AddCircleFilled(pivot, 7.0f, theme::kControl, 20);
    draw->AddCircle(pivot, 7.0f, theme::kBorderBright, 20, 1.0f);
    draw->AddCircleFilled(pivot, 2.5f, theme::kAccentLight, 12);

    ImGui::PopID();
}

bool sectionHeader(const char* title, bool* enabled)
{
    bool toggled = false;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    if (enabled) {
        ImGui::SameLine();
        const float switchWidth = ImGui::GetFrameHeight() * 0.75f * 1.9f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImGui::GetContentRegionAvail().x - switchWidth);
        toggled = toggleSwitch(title, enabled);
    }

    ImGui::Spacing();
    return toggled;
}

bool toggleSwitch(const char* id, bool* value)
{
    ImGui::PushID(id);

    const float height = ImGui::GetFrameHeight() * 0.72f;
    const float width  = height * 1.85f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##toggle", ImVec2(width, height));
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value  = !*value;
        changed = true;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float radius = height * 0.5f;
    const ImVec2 max(origin.x + width, origin.y + height);
    const bool hovered = ImGui::IsItemHovered();

    if (*value) {
        draw->AddRectFilled(ImVec2(origin.x - 2, origin.y - 2), ImVec2(max.x + 2, max.y + 2),
                            theme::withAlpha(theme::kAccent, 0.18f), radius + 2);
        draw->AddRectFilled(origin, max, hovered ? theme::kAccentLight : theme::kAccent, radius);
    } else {
        draw->AddRectFilled(origin, max, theme::kControl, radius);
        draw->AddRect(origin, max, hovered ? theme::kTextGhost : theme::kBorderBright, radius);
    }

    const float knobX = *value ? max.x - radius : origin.x + radius;
    draw->AddCircleFilled(ImVec2(knobX, origin.y + radius), radius - 3.0f,
                          *value ? theme::kBackground : theme::kTextFaint, 20);

    ImGui::PopID();
    return changed;
}

void statusPill(const char* text, ImU32 colour)
{
    ImFont* font = fontOr(g_fonts.caps);
    ImGui::PushFont(font, font->LegacySize);

    const float  spacing  = font->LegacySize * 0.14f;
    const float  textW    = spacedTextWidth(text, spacing);
    const float  height   = ImGui::GetFrameHeight() * 0.86f;
    const float  dot      = height * 0.24f;
    const ImVec2 padding(height * 0.42f, 0.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(padding.x * 2 + dot * 2 + 7.0f + textW, height);

    ImGui::InvisibleButton(text, size);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);

    // The badge body is the status colour at low alpha with a softer outline:
    // readable without shouting louder than the meters.
    draw->AddRectFilled(origin, max, theme::withAlpha(colour, 0.10f), height * 0.5f);
    draw->AddRect(origin, max, theme::withAlpha(colour, 0.40f), height * 0.5f);

    const ImVec2 dotCentre(origin.x + padding.x + dot, origin.y + height * 0.5f);
    draw->AddCircleFilled(dotCentre, dot * 2.0f, theme::withAlpha(colour, 0.20f), 16);
    draw->AddCircleFilled(dotCentre, dot, colour, 16);

    drawSpacedText(draw,
                   ImVec2(dotCentre.x + dot + 7.0f, origin.y + (height - font->LegacySize) * 0.5f),
                   colour == theme::kAccent ? theme::kAccentLight : colour, text, spacing);

    ImGui::PopFont();
}

bool powerButton(const char* id, bool on, float size)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed  = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered  = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float  r = size * 0.5f - 1.0f;

    if (on) {
        draw->AddCircleFilled(centre, r + 4.0f, theme::withAlpha(theme::kAccent, 0.16f), 32);
        draw->AddCircleFilled(centre, r, IM_COL32(0x0E, 0x1F, 0x1E, 0xFF), 32);
        draw->AddCircleFilled(ImVec2(centre.x, centre.y - r * 0.15f), r * 0.75f,
                              IM_COL32(0x1B, 0x4A, 0x47, 0xFF), 32);
        draw->AddCircle(centre, r, hovered ? theme::kAccentLight : theme::kAccent, 32, 1.2f);
    } else {
        draw->AddCircleFilled(centre, r, theme::kPanelRaised, 32);
        draw->AddCircle(centre, r, hovered ? theme::kTextGhost : theme::kBorderBright, 32, 1.0f);
    }

    const ImU32 icon = on ? theme::kAccentLight : (hovered ? theme::kTextDim : theme::kTextFaint);
    const float ir   = r * 0.42f;
    const float w    = std::max(1.6f, size * 0.07f);
    draw->PathArcTo(centre, ir, kPi * 1.5f + 0.75f, kPi * 1.5f + 2.0f * kPi - 0.75f, 32);
    draw->PathStroke(icon, w);
    draw->AddLine(ImVec2(centre.x, centre.y - ir * 1.25f), ImVec2(centre.x, centre.y - ir * 0.1f),
                  icon, w);

    return pressed;
}

bool segmented(const char* id, const char* const* labels, int count, int* selected,
               float height)
{
    ImGui::PushID(id);
    ImFont* font = fontOr(g_fonts.caps);
    ImGui::PushFont(font, font->LegacySize);

    if (height <= 0.0f)
        height = ImGui::GetFrameHeight();

    const float spacing = font->LegacySize * 0.12f;
    const float pad     = height * 0.55f;
    const float inset   = 3.0f;

    float total = inset * 2.0f;
    for (int i = 0; i < count; ++i)
        total += spacedTextWidth(labels[i], spacing) + pad * 2.0f + (i ? 2.0f : 0.0f);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##track", ImVec2(total, height));
    const ImVec2 mouse   = ImGui::GetIO().MousePos;
    const bool   clicked = ImGui::IsItemClicked();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    drawInset(draw, origin, ImVec2(origin.x + total, origin.y + height), 7.0f);

    bool changed = false;
    float x = origin.x + inset;
    for (int i = 0; i < count; ++i) {
        const float w = spacedTextWidth(labels[i], spacing) + pad * 2.0f;
        const ImVec2 a(x, origin.y + inset);
        const ImVec2 b(x + w, origin.y + height - inset);
        const bool over = mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;

        if (clicked && over && *selected != i) {
            *selected = i;
            changed   = true;
        }

        const bool on = *selected == i;
        if (on) {
            draw->AddRectFilledMultiColor(a, b, IM_COL32(0x2B, 0x31, 0x3C, 0xFF),
                                          IM_COL32(0x2B, 0x31, 0x3C, 0xFF),
                                          theme::kControl, theme::kControl);
            draw->AddRect(a, b, theme::kHighlight, 5.0f);
        }

        drawSpacedText(draw,
                       ImVec2(x + pad, origin.y + (height - font->LegacySize) * 0.5f),
                       on ? theme::kAccentLight : (over ? theme::kTextDim : theme::kTextFaint),
                       labels[i], spacing);
        x += w + 2.0f;
    }

    ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

bool chipButton(const char* label, bool lit, ImU32 litColour)
{
    ImFont* font = fontOr(g_fonts.caps);
    ImGui::PushFont(font, font->LegacySize);

    const float spacing = font->LegacySize * 0.12f;
    const float height  = ImGui::GetFrameHeight() * 0.86f;
    const float width   = spacedTextWidth(label, spacing) + height * 1.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + height);
    const float  radius = 8.0f;

    if (lit) {
        draw->AddRectFilled(origin, max, theme::withAlpha(litColour, 0.12f), radius);
        draw->AddRect(origin, max, theme::withAlpha(litColour, hovered ? 0.85f : 0.55f), radius);
    } else {
        draw->AddRectFilled(origin, max, theme::kPanelRaised, radius);
        draw->AddRect(origin, max, hovered ? theme::kTextGhost : theme::kBorderBright, radius);
    }

    const ImU32 text = lit ? (litColour == theme::kAccent ? theme::kAccentLight : litColour)
                           : (hovered ? theme::kText : theme::kTextDim);
    drawSpacedText(draw, ImVec2(origin.x + height * 0.5f, origin.y + (height - font->LegacySize) * 0.5f),
                   text, label, spacing);

    ImGui::PopFont();
    return pressed;
}

bool gearButton(const char* id, float size, ImU32 tint, bool lit)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive() || lit;

    ImDrawList* draw  = ImGui::GetWindowDrawList();
    const ImU32 frame = held ? theme::kControl : hovered ? theme::kControl : theme::kPanelRaised;
    const ImVec2 corner(origin.x + size, origin.y + size);
    draw->AddRectFilled(origin, corner, frame, 10.0f);
    draw->AddRect(origin, corner, hovered ? theme::kTextGhost : theme::kBorderBright, 10.0f);

    const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float body  = size * 0.2f;
    const float tooth = body * 1.45f;
    const float hole  = body * 0.45f;

    // Eight teeth: fewer reads as a flower at this size, more turns into a
    // circle. Each is a quad, so it stays convex and fills cleanly.
    constexpr int kTeeth = 8;
    for (int i = 0; i < kTeeth; ++i) {
        const float angle = (2.0f * kPi * static_cast<float>(i)) / kTeeth;
        const float base  = 0.20f; ///< Half-width at the root, in radians.
        const float tip   = 0.13f; ///< Narrower at the tip, so it reads as a cog.

        auto point = [&](float offset, float radius) {
            return ImVec2(centre.x + std::cos(angle + offset) * radius,
                          centre.y + std::sin(angle + offset) * radius);
        };

        draw->AddQuadFilled(point(-base, body * 0.9f), point(base, body * 0.9f),
                            point(tip, tooth), point(-tip, tooth), tint);
    }

    draw->AddCircleFilled(centre, body, tint, 24);
    draw->AddCircleFilled(centre, hole, frame, 16);

    return pressed;
}

void rightLabel(const char* text, ImU32 colour)
{
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(colour));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void infoRow(const char* key, const char* value, ImU32 valueColour)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();

    ImGui::PushFont(fontOr(g_fonts.mono), fontOr(g_fonts.mono)->LegacySize);
    rightLabel(value, valueColour);
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

float spacedTextWidth(const char* text, float spacing)
{
    const size_t length = std::strlen(text);
    if (length == 0)
        return 0.0f;
    return ImGui::CalcTextSize(text).x + spacing * static_cast<float>(length - 1);
}

void drawSpacedText(ImDrawList* draw, ImVec2 position, ImU32 colour, const char* text,
                    float spacing)
{
    char glyph[2] = {0, 0};
    float x = position.x;
    for (const char* c = text; *c; ++c) {
        glyph[0] = *c;
        draw->AddText(ImVec2(x, position.y), colour, glyph);
        x += ImGui::CalcTextSize(glyph).x + spacing;
    }
}

void capsLabel(const char* text, ImU32 colour)
{
    ImFont* font = fontOr(g_fonts.caps);
    ImGui::PushFont(font, font->LegacySize);
    const float spacing = font->LegacySize * 0.16f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = spacedTextWidth(text, spacing);
    ImGui::Dummy(ImVec2(width, font->LegacySize));
    drawSpacedText(ImGui::GetWindowDrawList(), origin, colour, text, spacing);
    ImGui::PopFont();
}

void drawInset(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding)
{
    draw->AddRectFilled(min, max, theme::kInset, rounding);
    // A shadow along the top edge and a lit lip along the bottom are what make
    // a flat rectangle read as a recess.
    draw->AddRectFilledMultiColor(ImVec2(min.x + rounding * 0.5f, min.y + 1.0f),
                                  ImVec2(max.x - rounding * 0.5f, min.y + std::min(8.0f, (max.y - min.y) * 0.4f)),
                                  IM_COL32(0, 0, 0, 0x90), IM_COL32(0, 0, 0, 0x90),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    draw->AddRect(min, max, IM_COL32(0, 0, 0, 0xFF), rounding);
    draw->AddLine(ImVec2(min.x + rounding, max.y + 0.5f), ImVec2(max.x - rounding, max.y + 0.5f),
                  IM_COL32(0xFF, 0xFF, 0xFF, 0x0D), 1.0f);
}

void drawFaceplate(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, ImU32 top,
                   ImU32 bottom)
{
    // Rounded corners first in the bottom colour, then the gradient inset by the
    // radius so its square corners stay inside the rounded outline.
    draw->AddRectFilled(min, max, bottom, rounding);
    draw->AddRectFilled(min, ImVec2(max.x, min.y + rounding * 2.0f), top, rounding,
                        ImDrawFlags_RoundTop);
    draw->AddRectFilledMultiColor(ImVec2(min.x, min.y + rounding), ImVec2(max.x, max.y - rounding),
                                  top, top, bottom, bottom);
    draw->AddRect(min, max, IM_COL32(0x2A, 0x30, 0x3B, 0xFF), rounding);
    draw->AddLine(ImVec2(min.x + rounding, min.y + 1.0f), ImVec2(max.x - rounding, min.y + 1.0f),
                  theme::kHighlight, 1.0f);
}

void drawGlowLine(ImDrawList* draw, const ImVec2* points, int count, ImU32 colour,
                  float thickness)
{
    if (count < 2)
        return;
    draw->AddPolyline(points, count, theme::withAlpha(colour, 0.10f), thickness * 5.0f);
    draw->AddPolyline(points, count, theme::withAlpha(colour, 0.22f), thickness * 2.5f);
    draw->AddPolyline(points, count, colour, thickness);
}

void drawLogo(ImDrawList* draw, ImVec2 origin, float size, bool withArcs)
{
    // The same geometry as tools/make-icon.py, in unit fractions of the tile.
    auto u = [&](float f) { return f * size; };
    const ImVec2 o = origin;

    const ImVec2 tileMin(o.x + u(0.03f), o.y + u(0.03f));
    const ImVec2 tileMax(o.x + u(0.97f), o.y + u(0.97f));
    draw->AddRectFilled(tileMin, tileMax, IM_COL32(0x10, 0x13, 0x18, 0xFF), u(0.225f));
    draw->AddRectFilled(tileMin, ImVec2(tileMax.x, o.y + u(0.5f)), IM_COL32(0x1E, 0x22, 0x2A, 0xFF),
                        u(0.225f), ImDrawFlags_RoundTop);
    draw->AddRect(tileMin, tileMax, IM_COL32(0x3A, 0x41, 0x4E, 0xB0), u(0.225f));

    const ImVec2 c(o.x + u(0.5f), o.y + u(0.375f));

    draw->AddCircleFilled(c, u(0.3f), theme::withAlpha(theme::kAccent, 0.12f), 32);

    if (withArcs) {
        const float a1 = 44.0f * kPi / 180.0f;
        draw->PathArcTo(c, u(0.285f), -a1, a1, 16);
        draw->PathStroke(theme::kAccent, std::max(1.0f, u(0.03f)));
        draw->PathArcTo(c, u(0.285f), kPi - a1, kPi + a1, 16);
        draw->PathStroke(theme::kAccent, std::max(1.0f, u(0.03f)));
    }

    const float yokeR = u(0.205f);
    const ImVec2 yoke(c.x, c.y + u(0.045f));
    draw->PathArcTo(yoke, yokeR, 18.0f * kPi / 180.0f, 162.0f * kPi / 180.0f, 20);
    draw->PathStroke(theme::kAccentDim, std::max(1.0f, u(0.045f)));
    draw->AddRectFilled(ImVec2(c.x - u(0.0225f), yoke.y + yokeR - u(0.02f)),
                        ImVec2(c.x + u(0.0225f), o.y + u(0.855f)), theme::kAccentDim);
    draw->AddRectFilled(ImVec2(c.x - u(0.155f), o.y + u(0.807f)),
                        ImVec2(c.x + u(0.155f), o.y + u(0.855f)), theme::kAccentDim, u(0.024f));

    const ImVec2 capMin(c.x - u(0.115f), c.y - u(0.185f));
    const ImVec2 capMax(c.x + u(0.115f), c.y + u(0.185f));
    draw->AddRectFilled(capMin, capMax, theme::kAccentDim, u(0.115f));
    draw->AddRectFilled(capMin, ImVec2(capMax.x, c.y + u(0.02f)), theme::kAccentLight, u(0.115f),
                        ImDrawFlags_RoundTop);
    draw->AddRectFilled(ImVec2(capMin.x, c.y - u(0.02f)), ImVec2(capMax.x, c.y + u(0.06f)),
                        theme::kAccent);
}

} // namespace rv::gui
