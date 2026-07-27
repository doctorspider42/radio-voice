/*
    Self-diagnostics that need no kernel debugger.

    A driver that loads cleanly and then does nothing useful is the hardest
    kind to investigate: there is no crash, no error code, and DbgPrintEx needs
    a debugger or DebugView on the other end. So the driver records what it did
    into its own service key, where any PowerShell prompt can read it.

    The interesting question here is not whether our own start-up succeeded -
    the device reports OK, so it did - but whether anything ever came back to
    *ask* us about our topology. Counting the calls answers that: no calls means
    nothing is looking at this driver, some calls means something looked and
    then decided against it. Those two point in completely opposite directions.

    Everything lands under:
        HKLM\SYSTEM\CurrentControlSet\Services\RadioVoiceAudio\Diagnostics
*/

#ifndef RADIOVOICE_DIAGNOSTICS_H
#define RADIOVOICE_DIAGNOSTICS_H

#include "Common.h"

namespace rvdiag {

/// Creates the key and clears the counters. Called from DriverEntry.
void Initialize();

/// Records a value, overwriting whatever was there.
void Record(_In_ PCWSTR name, _In_ ULONG value);

/// Records an NTSTATUS, and separately whether it was a success - reading a
/// signed status out of the registry by eye is needlessly error-prone.
void RecordStatus(_In_ PCWSTR name, _In_ NTSTATUS status);

/// Bumps a named counter. Counter storage is a small fixed table, so there is
/// no allocation on any path this touches.
///
/// Safe to call above PASSIVE_LEVEL: the in-memory counter is always updated,
/// and the registry write is skipped when the IRQL forbids it. Anything counted
/// from a DPC therefore only becomes visible once something calls Flush.
void Count(_In_ PCWSTR name);

/// Adds to a named counter. Same IRQL rules as Count - this exists for the
/// streaming path, where the interesting quantity is how many bytes moved
/// rather than how many times a function ran.
void Add(_In_ PCWSTR name, _In_ ULONG amount);

/// Writes every counter out. Must be called at PASSIVE_LEVEL. This is what
/// makes the DISPATCH_LEVEL counters readable; the streaming path calls it on
/// each state change, so the numbers settle as soon as a stream stops.
void Flush();

} // namespace rvdiag

#endif // RADIOVOICE_DIAGNOSTICS_H
