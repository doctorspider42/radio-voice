// This translation unit is the one that gives the KS GUIDs actual storage.
//
// The pin descriptors take the address of KSCATEGORY_AUDIO, KSNODETYPE_SPEAKER
// and friends, which the KS headers only declare unless INITGUID is defined
// first. Exactly one file in the driver may do this; doing it in two would
// produce duplicate symbols at link time, and in none would leave every
// category reference unresolved.
#define INITGUID
#include <initguid.h>

#include "Descriptors.h"

// SIZEOF_ARRAY comes from the WDK headers.

// Descriptors are read-only tables consulted while handling KS property
// requests, which can arrive at PASSIVE_LEVEL only - so they may be paged.
#pragma data_seg("PAGECONST")
#define RV_PAGED_CONST __declspec(allocate("PAGECONST"))

//=============================================================================
// Data ranges
//=============================================================================

// Advertised stream formats.
//
// PCM comes first and deliberately spans a range of depths rather than naming a
// single one. The audio engine negotiates a format before it will build an
// endpoint, and a pin offering one point - particularly a float-only point -
// leaves it nothing to agree to. It then declines silently, which looks exactly
// like a driver that never loaded.
RV_PAGED_CONST static const KSDATARANGE_AUDIO g_pcmStreamRange = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    RV_CHANNELS,    // MaximumChannels
    16,             // MinimumBitsPerSample
    24,             // MaximumBitsPerSample
    RV_SAMPLE_RATE, // MinimumSampleFrequency
    RV_SAMPLE_RATE  // MaximumSampleFrequency
};

// PCM only, no float range. A float wire format would force the driver to
// convert in floating point, which kernel code on x64 may not do without
// saving the interrupted thread's FP state first - see Common.h. Nothing is
// lost: the audio engine converts for any client that wants float.
RV_PAGED_CONST static const PKSDATARANGE g_streamRanges[] = {
    PKSDATARANGE(&g_pcmStreamRange)
};

// Bridge pins carry no digital data - they represent the wire between the wave
// filter and the topology filter, so their range is the analog placeholder.
RV_PAGED_CONST static const KSDATARANGE g_bridgeRange = {
    sizeof(KSDATARANGE),
    0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

RV_PAGED_CONST static const PKSDATARANGE g_bridgeRanges[] = {
    PKSDATARANGE(&g_bridgeRange)
};

//=============================================================================
// Wave filter - render
//
// client --> [0] sink ......... source [1] --> topology filter
//=============================================================================

RV_PAGED_CONST static const PCPIN_DESCRIPTOR g_waveRenderPins[RV_WAVE_RENDER_PIN_COUNT] = {
    // [0] Streaming sink. One instance: a cable with two writers would
    // interleave two programmes into the same ring.
    {
        1, 1, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_streamRanges), g_streamRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    // [1] Bridge to the topology filter.
    {
        0, 0, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_bridgeRanges), g_bridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    }
};

RV_PAGED_CONST static const PCCONNECTION_DESCRIPTOR g_waveRenderConnections[] = {
    { PCFILTER_NODE, RV_WAVE_RENDER_SINK, PCFILTER_NODE, RV_WAVE_RENDER_SOURCE }
};

RV_PAGED_CONST static const GUID g_waveRenderCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

const PCFILTER_DESCRIPTOR g_waveRenderFilterDescriptor = {
    0,                                      // Version
    nullptr,                                // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_waveRenderPins),
    g_waveRenderPins,
    sizeof(PCNODE_DESCRIPTOR),
    0, nullptr,                             // no nodes
    SIZEOF_ARRAY(g_waveRenderConnections),
    g_waveRenderConnections,
    SIZEOF_ARRAY(g_waveRenderCategories),
    g_waveRenderCategories
};

//=============================================================================
// Wave filter - capture
//
// topology filter --> [0] sink ......... source [1] --> client
//=============================================================================

RV_PAGED_CONST static const PCPIN_DESCRIPTOR g_waveCapturePins[RV_WAVE_CAPTURE_PIN_COUNT] = {
    // [0] Bridge from the topology filter.
    {
        0, 0, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_bridgeRanges), g_bridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    // [1] Streaming source.
    //
    // Several applications legitimately open the same microphone at once, so
    // unlike the render side this allows more than one instance. Each gets its
    // own stream object, and each reads the ring independently.
    {
        MAXULONG, MAXULONG, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_streamRanges), g_streamRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    }
};

RV_PAGED_CONST static const PCCONNECTION_DESCRIPTOR g_waveCaptureConnections[] = {
    { PCFILTER_NODE, RV_WAVE_CAPTURE_SINK, PCFILTER_NODE, RV_WAVE_CAPTURE_SOURCE }
};

RV_PAGED_CONST static const GUID g_waveCaptureCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

const PCFILTER_DESCRIPTOR g_waveCaptureFilterDescriptor = {
    0,
    nullptr,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_waveCapturePins),
    g_waveCapturePins,
    sizeof(PCNODE_DESCRIPTOR),
    0, nullptr,
    SIZEOF_ARRAY(g_waveCaptureConnections),
    g_waveCaptureConnections,
    SIZEOF_ARRAY(g_waveCaptureCategories),
    g_waveCaptureCategories
};

//=============================================================================
// Topology filter - render
//
// wave filter --> [0] wave in ......... line out [1] --> "speaker"
//
// The KSNODETYPE on the outward pin is what tells the audio endpoint builder
// what kind of device this is; it is why the endpoint appears under Playback
// rather than Recording.
//=============================================================================

RV_PAGED_CONST static const PCPIN_DESCRIPTOR g_topologyRenderPins[RV_TOPO_RENDER_PIN_COUNT] = {
    // [0] From the wave filter.
    {
        0, 0, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_bridgeRanges), g_bridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    // [1] The endpoint itself.
    {
        0, 0, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_bridgeRanges), g_bridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            nullptr,
            0
        }
    }
};

RV_PAGED_CONST static const PCCONNECTION_DESCRIPTOR g_topologyRenderConnections[] = {
    { PCFILTER_NODE, RV_TOPO_RENDER_WAVE_IN, PCFILTER_NODE, RV_TOPO_RENDER_LINE_OUT }
};

RV_PAGED_CONST static const GUID g_topologyCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_TOPOLOGY)
};

const PCFILTER_DESCRIPTOR g_topologyRenderFilterDescriptor = {
    0,
    nullptr,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_topologyRenderPins),
    g_topologyRenderPins,
    sizeof(PCNODE_DESCRIPTOR),
    0, nullptr,
    SIZEOF_ARRAY(g_topologyRenderConnections),
    g_topologyRenderConnections,
    SIZEOF_ARRAY(g_topologyCategories),
    g_topologyCategories
};

//=============================================================================
// Topology filter - capture
//
// "microphone" --> [0] mic in ......... wave out [1] --> wave filter
//=============================================================================

RV_PAGED_CONST static const PCPIN_DESCRIPTOR g_topologyCapturePins[RV_TOPO_CAPTURE_PIN_COUNT] = {
    // [0] The endpoint itself.
    {
        0, 0, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_bridgeRanges), g_bridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            nullptr,
            0
        }
    },
    // [1] To the wave filter.
    {
        0, 0, 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            SIZEOF_ARRAY(g_bridgeRanges), g_bridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    }
};

RV_PAGED_CONST static const PCCONNECTION_DESCRIPTOR g_topologyCaptureConnections[] = {
    { PCFILTER_NODE, RV_TOPO_CAPTURE_MIC_IN, PCFILTER_NODE, RV_TOPO_CAPTURE_WAVE_OUT }
};

const PCFILTER_DESCRIPTOR g_topologyCaptureFilterDescriptor = {
    0,
    nullptr,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(g_topologyCapturePins),
    g_topologyCapturePins,
    sizeof(PCNODE_DESCRIPTOR),
    0, nullptr,
    SIZEOF_ARRAY(g_topologyCaptureConnections),
    g_topologyCaptureConnections,
    SIZEOF_ARRAY(g_topologyCategories),
    g_topologyCategories
};

#pragma data_seg()
