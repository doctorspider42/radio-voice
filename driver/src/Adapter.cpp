/*
    StartDevice: builds the four filters and wires them together.

    PortCls does not discover topology on its own. For each direction the
    adapter has to create a port/miniport pair for the wave filter and another
    for the topology filter, register both as subdevices under the names the INF
    refers to, and then declare the physical connection between them. Miss the
    connection and both filters exist but no endpoint is ever created, which
    presents as a driver that installs cleanly and does nothing.
*/

#include "Common.h"
#include "Diagnostics.h"
#include "MinTopo.h"
#include "MinWaveRT.h"

extern "C" NTSTATUS RvStartDevice(_In_ PDEVICE_OBJECT DeviceObject,
                                  _In_ PIRP          Irp,
                                  _In_ PRESOURCELIST ResourceList);

#pragma code_seg("PAGE")

namespace {

/// Creates a port of `portClassId`, initialises it with `miniport`, and
/// registers it as a subdevice called `name`.
///
/// On success the caller receives a reference to the port's unknown, which it
/// needs to declare the physical connection; that reference is released once
/// the connection is registered.
NTSTATUS InstallSubdevice(_In_ PDEVICE_OBJECT DeviceObject,
                          _In_ PIRP Irp,
                          _In_ PRESOURCELIST ResourceList,
                          _In_ PCWSTR name,
                          _In_ REFGUID portClassId,
                          _In_ PUNKNOWN miniport,
                          _Outptr_ PUNKNOWN* portUnknown)
{
    PAGED_CODE();

    *portUnknown = nullptr;

    PPORT port = nullptr;
    NTSTATUS status = PcNewPort(&port, portClassId);
    if (!NT_SUCCESS(status)) {
        RV_LOG("PcNewPort failed for %S: 0x%08X", name, status);
        return status;
    }

    status = port->Init(DeviceObject, Irp, miniport, nullptr, ResourceList);
    if (!NT_SUCCESS(status)) {
        RV_LOG("port Init failed for %S: 0x%08X", name, status);
        port->Release();
        return status;
    }

    status = PcRegisterSubdevice(DeviceObject, const_cast<PWSTR>(name), port);
    if (!NT_SUCCESS(status)) {
        RV_LOG("PcRegisterSubdevice failed for %S: 0x%08X", name, status);
        port->Release();
        return status;
    }

    // Hand the caller an IUnknown on the port so the physical connection can
    // reference it; ownership of the port itself now sits with PortCls.
    status = port->QueryInterface(IID_IUnknown, reinterpret_cast<PVOID*>(portUnknown));
    port->Release();

    if (!NT_SUCCESS(status))
        RV_LOG("QueryInterface on port %S failed: 0x%08X", name, status);

    return status;
}

/// Builds one complete endpoint: wave filter, topology filter, and the
/// connection between them.
///
/// `waveIsSource` distinguishes the two directions. On the render side audio
/// leaves the wave filter and enters the topology filter; on the capture side
/// it flows the other way, and the connection has to be declared in that
/// direction or the endpoint builder will not follow it.
NTSTATUS InstallEndpoint(_In_ PDEVICE_OBJECT DeviceObject,
                         _In_ PIRP Irp,
                         _In_ PRESOURCELIST ResourceList,
                         _In_ RV_DIRECTION direction,
                         _In_ PCWSTR waveName,
                         _In_ PCWSTR topologyName)
{
    PAGED_CODE();

    const bool render = (direction == RvDirectionRender);

    NTSTATUS status = STATUS_SUCCESS;
    PUNKNOWN waveUnknown = nullptr;
    PUNKNOWN topologyUnknown = nullptr;

    auto* waveMiniport = new (NonPagedPoolNx, RV_POOL_TAG) MiniportWaveRT(nullptr, direction);
    if (!waveMiniport)
        return STATUS_INSUFFICIENT_RESOURCES;
    waveMiniport->AddRef();

    auto* topologyMiniport =
        new (NonPagedPoolNx, RV_POOL_TAG) MiniportTopology(nullptr, direction);
    if (!topologyMiniport) {
        waveMiniport->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    topologyMiniport->AddRef();

    status = InstallSubdevice(DeviceObject, Irp, ResourceList, waveName,
                              CLSID_PortWaveRT, PUNKNOWN(PMINIPORTWAVERT(waveMiniport)),
                              &waveUnknown);
    rvdiag::RecordStatus(render ? L"WaveRenderSubdevice" : L"WaveCaptureSubdevice", status);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = InstallSubdevice(DeviceObject, Irp, ResourceList, topologyName,
                              CLSID_PortTopology,
                              PUNKNOWN(PMINIPORTTOPOLOGY(topologyMiniport)),
                              &topologyUnknown);
    rvdiag::RecordStatus(render ? L"TopoRenderSubdevice" : L"TopoCaptureSubdevice", status);
    if (!NT_SUCCESS(status))
        goto cleanup;

    if (direction == RvDirectionRender) {
        // wave[source] -> topology[wave in]
        status = PcRegisterPhysicalConnection(DeviceObject,
                                              waveUnknown, RV_WAVE_RENDER_SOURCE,
                                              topologyUnknown, RV_TOPO_RENDER_WAVE_IN);
    } else {
        // topology[wave out] -> wave[sink]
        status = PcRegisterPhysicalConnection(DeviceObject,
                                              topologyUnknown, RV_TOPO_CAPTURE_WAVE_OUT,
                                              waveUnknown, RV_WAVE_CAPTURE_SINK);
    }

    rvdiag::RecordStatus(render ? L"PhysConnRender" : L"PhysConnCapture", status);

    if (!NT_SUCCESS(status))
        RV_LOG("PcRegisterPhysicalConnection failed for %S: 0x%08X", waveName, status);

cleanup:
    // The ports hold their own references now; these were only needed to build
    // the connection. The miniports are likewise owned by their ports.
    if (waveUnknown)
        waveUnknown->Release();
    if (topologyUnknown)
        topologyUnknown->Release();

    waveMiniport->Release();
    topologyMiniport->Release();

    return status;
}

} // namespace

extern "C" NTSTATUS RvStartDevice(_In_ PDEVICE_OBJECT DeviceObject,
                                  _In_ PIRP          Irp,
                                  _In_ PRESOURCELIST ResourceList)
{
    PAGED_CODE();

    RV_LOG("StartDevice");
    rvdiag::Record(L"StartDeviceEntered", 1);

    if (!DeviceObject || !Irp)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS status = InstallEndpoint(DeviceObject, Irp, ResourceList,
                                      RvDirectionRender,
                                      RV_WAVE_RENDER_NAME, RV_TOPO_RENDER_NAME);
    if (!NT_SUCCESS(status)) {
        RV_LOG("render endpoint failed: 0x%08X", status);
        return status;
    }

    status = InstallEndpoint(DeviceObject, Irp, ResourceList,
                             RvDirectionCapture,
                             RV_WAVE_CAPTURE_NAME, RV_TOPO_CAPTURE_NAME);
    if (!NT_SUCCESS(status)) {
        RV_LOG("capture endpoint failed: 0x%08X", status);
        return status;
    }

    RV_LOG("both endpoints registered");
    rvdiag::Record(L"StartDeviceSucceeded", 1);
    return STATUS_SUCCESS;
}

#pragma code_seg()
