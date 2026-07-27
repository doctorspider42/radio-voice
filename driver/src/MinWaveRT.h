/*
    WaveRT miniport and stream.

    One class serves both endpoints, parameterised by direction. The render and
    capture halves differ only in which way bytes move between the client's
    WaveRT buffer and the loopback ring; everything else - buffer allocation,
    timing, position reporting, notifications - is identical.

    The interface method declarations come from the IMP_* macros in portcls.h
    rather than being spelled out here. Those macros are emitted by the same
    header that declares the interfaces, so the signatures cannot drift out of
    sync with the WDK the driver is built against.
*/

#ifndef RADIOVOICE_MINWAVERT_H
#define RADIOVOICE_MINWAVERT_H

#include "Common.h"

class MiniportWaveRTStream;

//=============================================================================
// MiniportWaveRT
//=============================================================================

class MiniportWaveRT : public IMiniportWaveRT, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();

    MiniportWaveRT(PUNKNOWN outerUnknown, RV_DIRECTION direction);
    ~MiniportWaveRT();

    IMP_IMiniportWaveRT;

    RV_DIRECTION Direction() const { return m_direction; }

    /// Called by a stream as it is created and destroyed. Only the render side
    /// acts on this - it is how the exclusive-writer rule is enforced. On the
    /// capture side it simply records the most recent stream.
    void StreamCreated(MiniportWaveRTStream* stream);
    void StreamDestroyed(MiniportWaveRTStream* stream);

private:
    RV_DIRECTION           m_direction;
    PPORTWAVERT            m_port   = nullptr;
    PADAPTER_OBJECT        m_adapter = nullptr;
    MiniportWaveRTStream*  m_stream = nullptr;
};

//=============================================================================
// MiniportWaveRTStream
//
// The client is handed a plain block of memory and reads or writes it directly
// - that is the entire point of WaveRT. With no hardware to move the data, a
// high-resolution timer stands in for the DMA engine: on every tick it copies
// the bytes that "would have" been transferred since the previous tick and
// advances the reported position.
//
// The position is derived from the interrupt-time clock rather than by adding
// a fixed amount per tick. Timer callbacks are not perfectly punctual, and a
// position that accumulates timer error would drift away from real time and
// eventually be audible as a glitch every time the OS resynchronised to it.
//=============================================================================

class MiniportWaveRTStream : public IMiniportWaveRTStreamNotification,
                             public CUnknown {
public:
    DECLARE_STD_UNKNOWN();

    MiniportWaveRTStream(PUNKNOWN outerUnknown);
    ~MiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;

    NTSTATUS Init(MiniportWaveRT* miniport, PPORTWAVERTSTREAM portStream,
                  ULONG pin, BOOLEAN capture, PKSDATAFORMAT dataFormat);

private:
    /// Allocates the shared buffer and its MDL. Shared by the plain and the
    /// with-notification allocation paths, which differ only in whether the OS
    /// also asked to be woken up.
    NTSTATUS AllocateBuffer(ULONG requestedSize, ULONG notificationCount,
                            PMDL* mdl, ULONG* actualSize,
                            ULONG* offsetFromFirstPage,
                            MEMORY_CACHING_TYPE* cacheType);
    void ReleaseBuffer();

    NTSTATUS StartTimer();
    void     StopTimer();

    /// Timer body: moves audio and advances the position.
    void OnTick();
    static EXT_CALLBACK TimerCallback;

    /// Signals whatever notification events the OS registered.
    void SignalNotifications();

    /// Reads a negotiated format into the wire-format members below.
    /// Returns false for anything outside what the data ranges advertise.
    BOOLEAN ParseFormat(PKSDATAFORMAT format);

    /// Wire format of this stream - what the client's buffer actually holds.
    /// 16 or 24 bit PCM, not known until the format is negotiated. The loopback
    /// ring is always 32-bit PCM regardless, and these describe what has to be
    /// converted to and from it.
    ULONG m_wireBytesPerSample = 2;
    ULONG m_wireFrameBytes     = RV_CHANNELS * 2;
    ULONG m_wireBytesPerSecond = RV_SAMPLE_RATE * RV_CHANNELS * 2;

    /// Conversion scratch, in the internal 32-bit format. Allocated with the
    /// audio buffer so the timer callback never allocates.
    PVOID m_scratch     = nullptr;
    ULONG m_scratchSize = 0;

    MiniportWaveRT* m_miniport = nullptr;
    PPORTWAVERTSTREAM m_portStream = nullptr;

    BOOLEAN m_capture = FALSE;
    ULONG   m_pin     = 0;
    KSSTATE m_state   = KSSTATE_STOP;

    // Shared WaveRT buffer.
    PMDL   m_mdl        = nullptr;
    PVOID  m_buffer     = nullptr;
    ULONG  m_bufferSize = 0;

    // Position, in bytes since the stream started running.
    ULONGLONG m_position       = 0;
    ULONGLONG m_startTime100ns = 0;
    ULONGLONG m_lastTime100ns  = 0;

    // Timing.
    PEX_TIMER m_timer          = nullptr;
    ULONG     m_notifyCount    = 0;
    LONGLONG  m_period100ns    = 0;

    // Notification events registered by the OS. Two is what the audio engine
    // uses in practice; the array avoids an allocation on a hot path.
    static constexpr ULONG kMaxNotificationEvents = 4;
    PKEVENT m_notificationEvents[kMaxNotificationEvents] = {};
    ULONG   m_notificationEventCount = 0;
    KSPIN_LOCK m_notificationLock;

    /// Guards the buffer and position against the timer running concurrently
    /// with a state change on the OS's thread.
    KSPIN_LOCK m_stateLock;
};

#endif // RADIOVOICE_MINWAVERT_H
