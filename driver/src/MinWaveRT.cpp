#include "MinWaveRT.h"

#include "Descriptors.h"
#include "LoopbackBuffer.h"

//=============================================================================
// MiniportWaveRT
//=============================================================================

#pragma code_seg("PAGE")

MiniportWaveRT::MiniportWaveRT(PUNKNOWN outerUnknown, RV_DIRECTION direction)
    : CUnknown(outerUnknown)
    , m_direction(direction)
{
    PAGED_CODE();
}

MiniportWaveRT::~MiniportWaveRT()
{
    PAGED_CODE();
    RV_LOG("wave miniport destroyed (%s)",
           m_direction == RvDirectionRender ? "render" : "capture");
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRT::NonDelegatingQueryInterface(_In_ REFIID Interface,
                                            _COM_Outptr_ PVOID* Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT)) {
        *Object = PVOID(PMINIPORTWAVERT(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRT::Init(_In_opt_ PUNKNOWN /*UnknownAdapter*/,
                     _In_opt_ PRESOURCELIST /*ResourceList*/,
                     _In_ PPORTWAVERT Port)
{
    PAGED_CODE();

    m_port = Port;
    RV_LOG("wave miniport initialised (%s)",
           m_direction == RvDirectionRender ? "render" : "capture");
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* Description)
{
    PAGED_CODE();

    if (!Description)
        return STATUS_INVALID_PARAMETER;

    *Description = (m_direction == RvDirectionRender)
                       ? const_cast<PPCFILTER_DESCRIPTOR>(&g_waveRenderFilterDescriptor)
                       : const_cast<PPCFILTER_DESCRIPTOR>(&g_waveCaptureFilterDescriptor);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRT::DataRangeIntersection(_In_ ULONG /*PinId*/,
                                      _In_ PKSDATARANGE /*DataRange*/,
                                      _In_ PKSDATARANGE /*MatchingDataRange*/,
                                      _In_ ULONG /*OutputBufferLength*/,
                                      _Out_writes_bytes_opt_(OutputBufferLength) PVOID /*ResultantFormat*/,
                                      _Out_ PULONG /*ResultantFormatLength*/)
{
    PAGED_CODE();

    // Only one format is advertised, and it is described by a plain
    // KSDATARANGE_AUDIO. PortCls' built-in intersection handler produces
    // exactly the right answer from that, and returning NOT_IMPLEMENTED is the
    // documented way to ask for it. A hand-written handler here could only
    // reproduce it, with more room to get it wrong.
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    PAGED_CODE();

    if (!DeviceDescription)
        return STATUS_INVALID_PARAMETER;

    // There is no real bus master behind this, but PortCls still builds a DMA
    // adapter object from these fields. The values describe an unconstrained
    // scatter/gather master, which is the closest honest description of memory
    // that the CPU simply copies.
    RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DeviceDescription->Version           = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription->Master            = TRUE;
    DeviceDescription->ScatterGather     = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType     = PCIBus;
    DeviceDescription->MaximumLength     = 0xFFFFFFFF;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRT::NewStream(_Out_ PMINIPORTWAVERTSTREAM* Stream,
                          _In_ PPORTWAVERTSTREAM PortStream,
                          _In_ ULONG Pin,
                          _In_ BOOLEAN Capture,
                          _In_ PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();

    if (!Stream || !PortStream || !DataFormat)
        return STATUS_INVALID_PARAMETER;

    // Only the render side is exclusive, matching what its pin descriptor
    // declares: two writers would interleave two programmes into one ring.
    // Capture is not - several applications opening the same microphone at once
    // is ordinary, and its descriptor allows it, so refusing here would
    // contradict the descriptor and fail the second application for no reason.
    if (m_direction == RvDirectionRender && m_stream) {
        RV_LOG("second render stream refused; the cable already has a writer");
        return STATUS_DEVICE_BUSY;
    }

    auto* stream = new (NonPagedPoolNx, RV_POOL_TAG) MiniportWaveRTStream(nullptr);
    if (!stream)
        return STATUS_INSUFFICIENT_RESOURCES;

    stream->AddRef();

    NTSTATUS status = stream->Init(this, PortStream, Pin, Capture, DataFormat);
    if (!NT_SUCCESS(status)) {
        stream->Release();
        return status;
    }

    *Stream = PMINIPORTWAVERTSTREAM(stream);
    return STATUS_SUCCESS;
}

void MiniportWaveRT::StreamCreated(MiniportWaveRTStream* stream)
{
    PAGED_CODE();
    m_stream = stream;
}

void MiniportWaveRT::StreamDestroyed(MiniportWaveRTStream* stream)
{
    PAGED_CODE();
    if (m_stream == stream)
        m_stream = nullptr;
}

//=============================================================================
// MiniportWaveRTStream
//=============================================================================

MiniportWaveRTStream::MiniportWaveRTStream(PUNKNOWN outerUnknown)
    : CUnknown(outerUnknown)
{
    PAGED_CODE();
    KeInitializeSpinLock(&m_stateLock);
    KeInitializeSpinLock(&m_notificationLock);
}

MiniportWaveRTStream::~MiniportWaveRTStream()
{
    PAGED_CODE();

    StopTimer();
    ReleaseBuffer();

    if (m_miniport) {
        m_miniport->StreamDestroyed(this);
        m_miniport = nullptr;
    }

    RV_LOG("stream destroyed");
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::NonDelegatingQueryInterface(_In_ REFIID Interface,
                                                  _COM_Outptr_ PVOID* Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

BOOLEAN MiniportWaveRTStream::IsSupportedFormat(PKSDATAFORMAT format)
{
    PAGED_CODE();

    if (!format || format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX))
        return FALSE;

    const auto* waveFormat = &reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(format)->WaveFormatEx;

    if (waveFormat->nChannels != RV_CHANNELS ||
        waveFormat->nSamplesPerSec != RV_SAMPLE_RATE ||
        waveFormat->wBitsPerSample != RV_BITS_PER_SAMPLE) {
        return FALSE;
    }

    // Both the plain float tag and the extensible wrapper around it are
    // accepted; clients use either, and they describe the same bytes.
    if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return TRUE;

    if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(waveFormat);
        return IsEqualGUIDAligned(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                   ? TRUE : FALSE;
    }

    return FALSE;
}

NTSTATUS MiniportWaveRTStream::Init(MiniportWaveRT* miniport,
                                    PPORTWAVERTSTREAM portStream,
                                    ULONG pin, BOOLEAN capture,
                                    PKSDATAFORMAT dataFormat)
{
    PAGED_CODE();

    if (!IsSupportedFormat(dataFormat)) {
        RV_LOG("stream refused: unsupported format");
        return STATUS_NOT_SUPPORTED;
    }

    m_miniport   = miniport;
    m_portStream = portStream;
    m_pin        = pin;
    m_capture    = capture;
    m_state      = KSSTATE_STOP;

    miniport->StreamCreated(this);

    RV_LOG("stream created: pin %lu, %s", pin, capture ? "capture" : "render");
    return STATUS_SUCCESS;
}

//-----------------------------------------------------------------------------
// Buffer
//-----------------------------------------------------------------------------

NTSTATUS MiniportWaveRTStream::AllocateBuffer(ULONG requestedSize,
                                              ULONG notificationCount,
                                              PMDL* mdl, ULONG* actualSize,
                                              ULONG* offsetFromFirstPage,
                                              MEMORY_CACHING_TYPE* cacheType)
{
    PAGED_CODE();

    if (!mdl || !actualSize || !offsetFromFirstPage || !cacheType)
        return STATUS_INVALID_PARAMETER;

    if (m_buffer)
        return STATUS_DEVICE_BUSY;

    const ULONG minSize = (RV_BYTES_PER_SECOND / 1000) * RV_MIN_BUFFER_MS;
    const ULONG maxSize = (RV_BYTES_PER_SECOND / 1000) * RV_MAX_BUFFER_MS;

    ULONG size = requestedSize;
    if (size < minSize) size = minSize;
    if (size > maxSize) size = maxSize;

    // Whole frames, and a whole number of notification periods so the timer
    // does not have to deal with a short final period every cycle.
    const ULONG periods = notificationCount ? notificationCount : 1;
    const ULONG granularity = RV_FRAME_SIZE * periods;
    size = (size / granularity) * granularity;
    if (size == 0)
        size = granularity;

    // Rounded to whole pages so the mapping the OS builds for user mode starts
    // at offset zero; a page-aligned buffer also keeps the copies in the timer
    // callback off split cache lines.
    const ULONG allocationSize = ROUND_TO_PAGES(size);

    m_buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocationSize, RV_POOL_TAG);
    if (!m_buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    m_mdl = IoAllocateMdl(m_buffer, allocationSize, FALSE, FALSE, nullptr);
    if (!m_mdl) {
        ExFreePool(m_buffer);
        m_buffer = nullptr;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(m_mdl);

    m_bufferSize  = size;
    m_notifyCount = notificationCount;

    // The period is what the declared format says the buffer's worth of audio
    // takes to play, divided by the number of notifications the OS asked for.
    const ULONG bytesPerPeriod = size / periods;
    m_period100ns = (LONGLONG)bytesPerPeriod * 10000000LL / RV_BYTES_PER_SECOND;
    if (m_period100ns <= 0)
        m_period100ns = (LONGLONG)RV_DEFAULT_PERIOD_MS * 10000LL;

    *mdl                 = m_mdl;
    *actualSize          = size;
    *offsetFromFirstPage = 0;
    *cacheType           = MmCached;

    RV_LOG("buffer allocated: %lu bytes, %lu notifications, period %lld00ns",
           size, notificationCount, m_period100ns);

    return STATUS_SUCCESS;
}

void MiniportWaveRTStream::ReleaseBuffer()
{
    PAGED_CODE();

    if (m_mdl) {
        IoFreeMdl(m_mdl);
        m_mdl = nullptr;
    }
    if (m_buffer) {
        ExFreePool(m_buffer);
        m_buffer = nullptr;
    }
    m_bufferSize = 0;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::AllocateAudioBuffer(_In_ ULONG RequestedSize,
                                          _Out_ PMDL* AudioBufferMdl,
                                          _Out_ ULONG* ActualSize,
                                          _Out_ ULONG* OffsetFromFirstPage,
                                          _Out_ MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    return AllocateBuffer(RequestedSize, 0, AudioBufferMdl, ActualSize,
                          OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID)
MiniportWaveRTStream::FreeAudioBuffer(_In_opt_ PMDL /*AudioBufferMdl*/,
                                      _In_ ULONG /*BufferSize*/)
{
    PAGED_CODE();
    StopTimer();
    ReleaseBuffer();
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::AllocateBufferWithNotification(_In_ ULONG NotificationCount,
                                                     _In_ ULONG RequestedSize,
                                                     _Out_ PMDL* AudioBufferMdl,
                                                     _Out_ ULONG* ActualSize,
                                                     _Out_ ULONG* OffsetFromFirstPage,
                                                     _Out_ MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    return AllocateBuffer(RequestedSize, NotificationCount, AudioBufferMdl,
                          ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID)
MiniportWaveRTStream::FreeBufferWithNotification(_In_opt_ PMDL /*AudioBufferMdl*/,
                                                 _In_ ULONG /*BufferSize*/)
{
    PAGED_CODE();
    StopTimer();
    ReleaseBuffer();
}

//-----------------------------------------------------------------------------
// Notifications
//-----------------------------------------------------------------------------

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PAGED_CODE();

    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_notificationLock, &handle);

    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    if (m_notificationEventCount < kMaxNotificationEvents) {
        m_notificationEvents[m_notificationEventCount++] = NotificationEvent;
        status = STATUS_SUCCESS;
    }

    KeReleaseInStackQueuedSpinLock(&handle);
    return status;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PAGED_CODE();

    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_notificationLock, &handle);

    NTSTATUS status = STATUS_NOT_FOUND;
    for (ULONG i = 0; i < m_notificationEventCount; ++i) {
        if (m_notificationEvents[i] == NotificationEvent) {
            m_notificationEvents[i] = m_notificationEvents[--m_notificationEventCount];
            m_notificationEvents[m_notificationEventCount] = nullptr;
            status = STATUS_SUCCESS;
            break;
        }
    }

    KeReleaseInStackQueuedSpinLock(&handle);
    return status;
}

void MiniportWaveRTStream::SignalNotifications()
{
    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_notificationLock, &handle);

    for (ULONG i = 0; i < m_notificationEventCount; ++i) {
        if (m_notificationEvents[i])
            KeSetEvent(m_notificationEvents[i], 0, FALSE);
    }

    KeReleaseInStackQueuedSpinLock(&handle);
}

//-----------------------------------------------------------------------------
// Position and timing
//-----------------------------------------------------------------------------

#pragma code_seg()

void MiniportWaveRTStream::TimerCallback(PEX_TIMER /*Timer*/, PVOID Context)
{
    auto* stream = static_cast<MiniportWaveRTStream*>(Context);
    if (stream)
        stream->OnTick();
}

void MiniportWaveRTStream::OnTick()
{
    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_stateLock, &handle);

    if (m_state != KSSTATE_RUN || !m_buffer || m_bufferSize == 0) {
        KeReleaseInStackQueuedSpinLock(&handle);
        return;
    }

    // Position follows the clock, not the number of times this callback has
    // run. Timer callbacks are late by varying amounts under load, and a
    // position built by accumulating per-tick constants would slowly diverge
    // from real time - which the OS would eventually correct with an audible
    // jump.
    const ULONGLONG now     = KeQueryInterruptTime();
    const ULONGLONG elapsed = now - m_startTime100ns;
    const ULONGLONG target  = elapsed * RV_BYTES_PER_SECOND / 10000000ULL;

    ULONGLONG delta = (target > m_position) ? (target - m_position) : 0;
    if (delta == 0) {
        KeReleaseInStackQueuedSpinLock(&handle);
        return;
    }

    // Falling more than a full buffer behind means the system stalled. There is
    // no way to recover the missing audio, so the buffer is traversed once and
    // the position jumps to where the clock says it should be.
    if (delta > m_bufferSize)
        delta = m_bufferSize;

    ULONG offset = (ULONG)(m_position % m_bufferSize);
    ULONG remaining = (ULONG)delta;

    while (remaining > 0) {
        const ULONG chunk = min(remaining, m_bufferSize - offset);
        PUCHAR at = static_cast<PUCHAR>(m_buffer) + offset;

        if (m_capture)
            g_loopback.Read(at, chunk);   // cable -> client
        else
            g_loopback.Write(at, chunk);  // client -> cable

        offset = (offset + chunk) % m_bufferSize;
        remaining -= chunk;
    }

    m_position      = target;
    m_lastTime100ns = now;

    KeReleaseInStackQueuedSpinLock(&handle);

    SignalNotifications();
}

#pragma code_seg("PAGE")

NTSTATUS MiniportWaveRTStream::StartTimer()
{
    PAGED_CODE();

    if (m_timer)
        return STATUS_SUCCESS;

    // A high-resolution timer is required, not a convenience: KeSetTimerEx
    // works in whole milliseconds, and a buffer of a few milliseconds split
    // into two notifications needs a sub-millisecond period.
    m_timer = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
    if (!m_timer)
        return STATUS_INSUFFICIENT_RESOURCES;

    m_startTime100ns = KeQueryInterruptTime();
    m_lastTime100ns  = m_startTime100ns;
    m_position       = 0;

    // Negative due time means relative; the period repeats it.
    ExSetTimer(m_timer, -m_period100ns, m_period100ns, nullptr);

    RV_LOG("timer started, period %lld00ns", m_period100ns);
    return STATUS_SUCCESS;
}

void MiniportWaveRTStream::StopTimer()
{
    PAGED_CODE();

    if (!m_timer)
        return;

    // Cancel = TRUE, Wait = TRUE: a callback that is already running must be
    // allowed to finish before the object it holds a pointer to goes away.
    // ExDeleteTimer does both, so a separate ExCancelTimer would add nothing.
    ExDeleteTimer(m_timer, TRUE, TRUE, nullptr);
    m_timer = nullptr;

    RV_LOG("timer stopped");
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    PAGED_CODE();

    RV_LOG("SetState %d (%s)", State, m_capture ? "capture" : "render");

    switch (State) {
        case KSSTATE_STOP: {
            StopTimer();

            KLOCK_QUEUE_HANDLE handle;
            KeAcquireInStackQueuedSpinLock(&m_stateLock, &handle);
            m_state    = KSSTATE_STOP;
            m_position = 0;
            KeReleaseInStackQueuedSpinLock(&handle);

            if (!m_capture)
                g_loopback.SetRenderRunning(FALSE);
            break;
        }

        case KSSTATE_ACQUIRE:
        case KSSTATE_PAUSE: {
            StopTimer();

            KLOCK_QUEUE_HANDLE handle;
            KeAcquireInStackQueuedSpinLock(&m_stateLock, &handle);
            m_state = State;
            KeReleaseInStackQueuedSpinLock(&handle);

            if (!m_capture)
                g_loopback.SetRenderRunning(FALSE);
            break;
        }

        case KSSTATE_RUN: {
            // Whichever side starts first clears the ring, so a new stream does
            // not open by replaying whatever the previous one left in it.
            g_loopback.Reset();

            {
                KLOCK_QUEUE_HANDLE handle;
                KeAcquireInStackQueuedSpinLock(&m_stateLock, &handle);
                m_state    = KSSTATE_RUN;
                m_position = 0;
                if (m_buffer)
                    RtlZeroMemory(m_buffer, m_bufferSize);
                KeReleaseInStackQueuedSpinLock(&handle);
            }

            if (!m_capture)
                g_loopback.SetRenderRunning(TRUE);

            RV_RETURN_IF_FAILED(StartTimer());
            break;
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    return IsSupportedFormat(DataFormat) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    if (!Position)
        return STATUS_INVALID_PARAMETER;

    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_stateLock, &handle);

    // Linear byte counts since the stream started running, which is what
    // KSPROPERTY_AUDIO_POSITION is defined to carry.
    Position->PlayOffset  = m_position;
    Position->WriteOffset = m_position;

    KeReleaseInStackQueuedSpinLock(&handle);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::GetPositionRegister(_Out_ PKSRTAUDIO_HWREGISTER /*Register*/)
{
    PAGED_CODE();
    // No hardware register exists to map. Returning NOT_IMPLEMENTED is the
    // documented signal for the port driver to fall back to polling
    // GetPosition, which is what a software cable can actually answer.
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS)
MiniportWaveRTStream::GetClockRegister(_Out_ PKSRTAUDIO_HWREGISTER /*Register*/)
{
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(VOID)
MiniportWaveRTStream::GetHWLatency(_Out_ PKSRTAUDIO_HWLATENCY Latency)
{
    PAGED_CODE();

    if (!Latency)
        return;

    // No codec, no chipset, no FIFO - the only delay is the loopback ring, and
    // that is reported through the endpoint's period rather than here.
    Latency->ChipsetDelay = 0;
    Latency->CodecDelay   = 0;
    Latency->FifoSize     = 0;
}

#pragma code_seg()
