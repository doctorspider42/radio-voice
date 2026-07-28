#pragma once

/// Launching RadioVoice when the user signs in.
///
/// A processor sitting between the microphone and everything that listens to it
/// is only doing its job if it is already running by the time the first call
/// starts, so this is the natural companion to the tray icon.
///
/// The entry lives under HKEY_CURRENT_USER, which is what makes it writable
/// without elevation and what keeps it per-user on a shared machine. The
/// installer writes the same value, so whichever of the two the user reaches
/// for, there is only ever one entry to reason about.
namespace rv::autostart {

/// Whether the entry exists and points at this executable. A stale entry left
/// by a copy that has since moved reads as off, because that is what it is.
bool enabled();

/// Adds or removes the entry. Returns false when the registry refused, which is
/// worth reporting rather than silently leaving the checkbox where the user
/// did not put it.
bool setEnabled(bool on);

} // namespace rv::autostart
