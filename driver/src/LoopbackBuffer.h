/*
    The ring that joins the render endpoint to the capture endpoint.

    Both ends are driven from timer DPCs at DISPATCH_LEVEL, so access is
    serialised with a spin lock rather than a mutex. The two timers are
    independent and will not stay in phase, which is why this is a ring with
    slack rather than a straight buffer swap.

    Policy at the edges is asymmetric on purpose:

      * A writer that finds the ring full drops the *oldest* audio. Blocking is
        not an option in a DPC, and discarding stale audio keeps the delay from
        growing without bound when the capture side is not being drained.

      * A reader that finds the ring short fills the remainder with silence.
        Silence is the correct output when nothing is playing into the cable,
        which is also the normal state whenever the application is not running.
*/

#ifndef RADIOVOICE_LOOPBACK_BUFFER_H
#define RADIOVOICE_LOOPBACK_BUFFER_H

#include "Common.h"

class LoopbackBuffer {
public:
    NTSTATUS Initialize();
    void     Cleanup();

    /// Drops everything buffered. Called when either side starts, so a new
    /// stream does not begin by playing whatever the previous one left behind.
    void Reset();

    /// Render side. Never blocks; overruns discard the oldest audio.
    void Write(_In_reads_bytes_(bytes) const void* data, ULONG bytes);

    /// Capture side. Always writes exactly `bytes`, padding with silence.
    void Read(_Out_writes_bytes_(bytes) void* data, ULONG bytes);

    /// Tracks whether a render stream is running, so the capture side can tell
    /// "nothing is playing" from "the writer fell behind" for diagnostics.
    void SetRenderRunning(BOOLEAN running) { m_renderRunning = running; }
    BOOLEAN IsRenderRunning() const { return m_renderRunning; }

    ULONG Filled();

private:
    // Deliberately no default member initialisers, and therefore no
    // constructor. This object is a global, and kernel-mode images have no CRT
    // to run static initialisers - the linker says as much with LNK4210. Every
    // field is set by Initialize(), which DriverEntry calls before anything
    // else can reach the ring.
    KSPIN_LOCK m_lock;
    PUCHAR     m_data;
    ULONG      m_capacity;
    ULONG      m_writePos;
    ULONG      m_readPos;
    ULONG      m_filled;
    BOOLEAN    m_renderRunning;
};

/// There is exactly one cable, so the ring is a single adapter-wide instance
/// rather than something threaded through every miniport interface.
extern LoopbackBuffer g_loopback;

#endif // RADIOVOICE_LOOPBACK_BUFFER_H
