/*
    Filter, pin and connection descriptors.

    Four filters are described: a wave and a topology filter for each direction.
    Each pair is joined by a physical connection registered in Adapter.cpp, and
    the pin indices on both sides of that connection are the RV_WAVE_* and
    RV_TOPO_* constants in Common.h.

    The topology filters carry no nodes - no volume, no mute, no meters. A cable
    has nothing to adjust, and every node added here would be a control surface
    the user could change without it affecting anything. Windows provides a
    software volume for the endpoint regardless.
*/

#ifndef RADIOVOICE_DESCRIPTORS_H
#define RADIOVOICE_DESCRIPTORS_H

#include "Common.h"

extern const PCFILTER_DESCRIPTOR g_waveRenderFilterDescriptor;
extern const PCFILTER_DESCRIPTOR g_waveCaptureFilterDescriptor;
extern const PCFILTER_DESCRIPTOR g_topologyRenderFilterDescriptor;
extern const PCFILTER_DESCRIPTOR g_topologyCaptureFilterDescriptor;

#endif // RADIOVOICE_DESCRIPTORS_H
