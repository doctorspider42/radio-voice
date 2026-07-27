/*
    RadioVoice virtual audio driver - shared declarations.

    The driver registers one PortCls adapter exposing two endpoints:

        "RadioVoice Output"      - a render endpoint the application plays into
        "RadioVoice Microphone"  - a capture endpoint other applications select

    Everything written to the render endpoint is copied into a shared ring and
    read back out of the capture endpoint. That is the whole function of the
    driver: it is a cable, not a device.

    There is no hardware behind it, so there is no DMA engine to advance the
    WaveRT position register. A periodic timer does that job instead, moving
    the position at exactly the rate the declared format implies.
*/

#ifndef RADIOVOICE_COMMON_H
#define RADIOVOICE_COMMON_H

// Include order matters here and is not negotiable.
//
// ksmedia.h describes wave formats in terms of WAVEFORMATEX, which lives in
// mmreg.h; portcls.h in turn declares interfaces taking KSDATAFORMAT_WAVEFORMATEX
// from ksmedia.h. Pull them in the other way round and the errors surface deep
// inside the WDK headers, pointing at the SDK rather than at this file.
//
// NOBITMAP keeps mmreg.h from dragging in the GDI bitmap definitions, which do
// not belong in a kernel driver and clash with wingdi.h if anything else pulls
// that in later.
#include <ntddk.h>
#include <windef.h>
#include <winerror.h>

#define NOBITMAP
#include <mmreg.h>
#undef NOBITMAP

#include <ks.h>
#include <ksmedia.h>
#include <portcls.h>
#include <stdunk.h>
#include <ntstrsafe.h>

//=============================================================================
// Identity
//=============================================================================

#define RV_POOL_TAG 'oidR' // "Rdio", reversed as pool tags conventionally are

#define RV_DRIVER_NAME L"RadioVoiceAudio"

//=============================================================================
// Audio format
//
// Two layers, and keeping them apart matters.
//
// The *wire* format is what a pin advertises and what the client's buffer
// holds. Several are offered - 16 and 24 bit PCM as well as 32 bit float -
// because the audio engine negotiates a format before it will build an
// endpoint at all, and a pin offering a single point with no PCM among it
// gives it nothing to agree to. It does not report an error; it silently
// declines to create the endpoint, which is indistinguishable from the driver
// not being there.
//
// The *internal* format is what crosses the loopback ring: always 32 bit
// signed integer. Fixing it means the two endpoints can negotiate
// independently without the ring having to care.
//
// Integer, not float, and that is not a detail. Kernel code on x64 may only
// touch floating point or SSE registers between KeSaveExtendedProcessorState
// and KeRestoreExtendedProcessorState - using them bare corrupts the floating
// point state of whichever user-mode thread happened to be interrupted. With
// a 32 bit integer hub every conversion is a shift, so the question never
// arises and nothing has to be saved on a path that runs every few
// milliseconds.
//
// 32 bits also means the widest advertised wire format (24 bit) converts in
// and back out without losing anything.
//=============================================================================

#define RV_SAMPLE_RATE      48000
#define RV_CHANNELS         2

// Internal (ring) format: 32-bit signed PCM.
#define RV_INTERNAL_BYTES_PER_SAMPLE 4
#define RV_INTERNAL_FRAME_SIZE       (RV_CHANNELS * RV_INTERNAL_BYTES_PER_SAMPLE)
#define RV_INTERNAL_BYTES_PER_SECOND (RV_SAMPLE_RATE * RV_INTERNAL_FRAME_SIZE)

//=============================================================================
// Buffering
//=============================================================================

// Bounds the OS may pick a cyclic buffer within, in milliseconds.
#define RV_MIN_BUFFER_MS 3
#define RV_MAX_BUFFER_MS 500

// Interval at which the position register advances and notification events
// fire when the client did not ask for notifications. 10 ms matches the
// default Windows shared-mode period.
#define RV_DEFAULT_PERIOD_MS 10

// Depth of the ring joining render to capture. Large enough to absorb the two
// endpoints' timers drifting apart before either is opened in lockstep, small
// enough that the added delay is inaudible.
#define RV_LOOPBACK_MS 40
#define RV_LOOPBACK_BYTES ((RV_INTERNAL_BYTES_PER_SECOND / 1000) * RV_LOOPBACK_MS)

//=============================================================================
// Pin and node indices
//
// Both the wave and topology filters use these; the physical connection
// between the two filters is built from them in Adapter.cpp.
//=============================================================================

// Wave filter, render side.
enum {
    RV_WAVE_RENDER_SINK   = 0, // from the client
    RV_WAVE_RENDER_SOURCE = 1, // bridge pin to the topology filter
    RV_WAVE_RENDER_PIN_COUNT
};

// Wave filter, capture side.
enum {
    RV_WAVE_CAPTURE_SINK   = 0, // bridge pin from the topology filter
    RV_WAVE_CAPTURE_SOURCE = 1, // to the client
    RV_WAVE_CAPTURE_PIN_COUNT
};

// Topology filter, render side.
enum {
    RV_TOPO_RENDER_WAVE_IN = 0, // bridge pin from the wave filter
    RV_TOPO_RENDER_LINE_OUT = 1,
    RV_TOPO_RENDER_PIN_COUNT
};

// Topology filter, capture side.
enum {
    RV_TOPO_CAPTURE_MIC_IN   = 0,
    RV_TOPO_CAPTURE_WAVE_OUT = 1, // bridge pin to the wave filter
    RV_TOPO_CAPTURE_PIN_COUNT
};

//=============================================================================
// Subdevice names
//
// These strings tie together three things that must agree exactly or the
// endpoints will not be created: the names registered by PcRegisterSubdevice
// in Adapter.cpp, the reference strings in the INF's AddInterface directives,
// and the KSNAME_ entries in the INF.
//=============================================================================

#define RV_WAVE_RENDER_NAME    L"WaveRender"
#define RV_WAVE_CAPTURE_NAME   L"WaveCapture"
#define RV_TOPO_RENDER_NAME    L"TopologyRender"
#define RV_TOPO_CAPTURE_NAME   L"TopologyCapture"

//=============================================================================
// Diagnostics
//=============================================================================

#if DBG
#define RV_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "RadioVoice: " fmt "\n", ##__VA_ARGS__)
#else
#define RV_LOG(fmt, ...) ((void)0)
#endif

#define RV_RETURN_IF_FAILED(expr)          \
    do {                                   \
        NTSTATUS _s = (expr);              \
        if (!NT_SUCCESS(_s)) return _s;    \
    } while (0)

//=============================================================================
// Direction of an endpoint, used to parameterise the shared miniport code.
//=============================================================================

typedef enum _RV_DIRECTION {
    RvDirectionRender,
    RvDirectionCapture
} RV_DIRECTION;

//=============================================================================
// Adapter-wide context, reachable from the miniports.
//=============================================================================

typedef struct _RV_ADAPTER_CONTEXT {
    PDEVICE_OBJECT DeviceObject;
} RV_ADAPTER_CONTEXT, *PRV_ADAPTER_CONTEXT;

// Declared in Adapter.cpp; the miniport factories live there too.
extern "C" {

NTSTATUS CreateMiniportWaveRT(PUNKNOWN* Unknown, REFCLSID, PUNKNOWN OuterUnknown,
                              POOL_TYPE PoolType, PUNKNOWN PortUnknown,
                              PVOID Context, PVOID* Reserved);

NTSTATUS CreateMiniportTopology(PUNKNOWN* Unknown, REFCLSID, PUNKNOWN OuterUnknown,
                                POOL_TYPE PoolType, PUNKNOWN PortUnknown,
                                PVOID Context, PVOID* Reserved);

} // extern "C"

// Note on allocation: portcls.h already defines the pool-aware operator new
// and the matching deletes that `new (NonPagedPoolNx, RV_POOL_TAG) T(...)`
// resolves to. Defining them here as well is a redefinition error, not an
// override - so the driver simply uses the ones the port class library
// provides.

#endif // RADIOVOICE_COMMON_H
