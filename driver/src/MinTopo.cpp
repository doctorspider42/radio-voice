#include "MinTopo.h"

#include "Descriptors.h"

#pragma code_seg("PAGE")

MiniportTopology::MiniportTopology(PUNKNOWN outerUnknown, RV_DIRECTION direction)
    : CUnknown(outerUnknown)
    , m_direction(direction)
{
    PAGED_CODE();
}

MiniportTopology::~MiniportTopology()
{
    PAGED_CODE();
    RV_LOG("topology miniport destroyed (%s)",
           m_direction == RvDirectionRender ? "render" : "capture");
}

STDMETHODIMP_(NTSTATUS)
MiniportTopology::NonDelegatingQueryInterface(_In_ REFIID Interface,
                                              _COM_Outptr_ PVOID* Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology)) {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportTopology::Init(_In_opt_ PUNKNOWN /*UnknownAdapter*/,
                       _In_opt_ PRESOURCELIST /*ResourceList*/,
                       _In_ PPORTTOPOLOGY Port)
{
    PAGED_CODE();

    m_port = Port;
    RV_LOG("topology miniport initialised (%s)",
           m_direction == RvDirectionRender ? "render" : "capture");
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* Description)
{
    PAGED_CODE();

    if (!Description)
        return STATUS_INVALID_PARAMETER;

    *Description = (m_direction == RvDirectionRender)
                       ? const_cast<PPCFILTER_DESCRIPTOR>(&g_topologyRenderFilterDescriptor)
                       : const_cast<PPCFILTER_DESCRIPTOR>(&g_topologyCaptureFilterDescriptor);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
MiniportTopology::DataRangeIntersection(_In_ ULONG /*PinId*/,
                                        _In_ PKSDATARANGE /*DataRange*/,
                                        _In_ PKSDATARANGE /*MatchingDataRange*/,
                                        _In_ ULONG /*OutputBufferLength*/,
                                        _Out_writes_bytes_opt_(OutputBufferLength) PVOID /*ResultantFormat*/,
                                        _Out_ PULONG /*ResultantFormatLength*/)
{
    PAGED_CODE();

    // Every pin on this filter is a bridge pin carrying the analog placeholder
    // range, so there is no format to negotiate.
    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg()
