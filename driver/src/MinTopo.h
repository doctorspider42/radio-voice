/*
    Topology miniport.

    The topology filter is what makes an endpoint appear in the Sound control
    panel at all: the audio endpoint builder walks the topology looking for a
    pin whose category says "speaker" or "microphone", and creates an endpoint
    for it. The wave filter alone would be invisible.

    This one is as thin as a topology filter can be - two pins, one connection,
    no nodes. See Descriptors.cpp for why there are no volume or mute nodes.
*/

#ifndef RADIOVOICE_MINTOPO_H
#define RADIOVOICE_MINTOPO_H

#include "Common.h"

class MiniportTopology : public IMiniportTopology, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();

    MiniportTopology(PUNKNOWN outerUnknown, RV_DIRECTION direction);
    ~MiniportTopology();

    IMP_IMiniportTopology;

private:
    RV_DIRECTION  m_direction;
    PPORTTOPOLOGY m_port = nullptr;
};

#endif // RADIOVOICE_MINTOPO_H
