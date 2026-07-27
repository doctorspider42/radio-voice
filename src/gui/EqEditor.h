#pragma once

#include <imgui.h>

#include "core/Params.h"
#include "dsp/SpectrumAnalyzer.h"

namespace rv::gui {

/// Interactive equaliser display.
///
/// Combines three layers that are usually separate controls: the live input
/// spectrum, the resulting frequency response, and the band handles themselves.
/// Seeing the response drawn over the spectrum is what makes an EQ decision
/// obvious rather than a guess.
///
/// Interaction:
///   * drag a band handle vertically  - gain
///   * mouse wheel over a band handle - Q
///   * double-click a band handle     - flat
///   * drag the cut markers along the bottom - high-pass / low-pass frequency
///
/// Returns true on frames where a parameter changed, so the caller can bump the
/// coefficient revision and mark the configuration dirty.
bool eqEditor(const char* id, Params& params, dsp::SpectrumAnalyzer& spectrum,
              float sampleRate, ImVec2 size);

} // namespace rv::gui
