/*
    Driver entry points.

    PortCls owns almost all of the PnP and power plumbing for an audio adapter;
    this file only hands it the two callbacks it needs and takes care of the one
    resource that lives for the whole driver rather than per device - the
    loopback ring.
*/

#include "Common.h"
#include "Diagnostics.h"
#include "LoopbackBuffer.h"

extern "C" {

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     RvDriverUnload;

NTSTATUS RvAddDevice(_In_ PDRIVER_OBJECT DriverObject,
                     _In_ PDEVICE_OBJECT PhysicalDeviceObject);

NTSTATUS RvStartDevice(_In_ PDEVICE_OBJECT DeviceObject,
                       _In_ PIRP          Irp,
                       _In_ PRESOURCELIST ResourceList);

} // extern "C"

// PortCls installs its own unload routine; ours has to run first and then
// chain, or the port class library never gets to tear itself down.
static PDRIVER_UNLOAD g_portClsUnload = nullptr;

#pragma code_seg("INIT")

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject,
                                _In_ PUNICODE_STRING RegistryPath)
{
    RV_LOG("DriverEntry");

    rvdiag::Initialize();
    rvdiag::Record(L"DriverEntry", 1);

    // The ring is allocated up front rather than on first stream: allocation
    // failure at this point is reportable, whereas failing inside a stream
    // start would surface to the user as an unexplained silent device.
    NTSTATUS status = g_loopback.Initialize();
    if (!NT_SUCCESS(status)) {
        RV_LOG("loopback ring allocation failed: 0x%08X", status);
        return status;
    }

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, RvAddDevice);
    if (!NT_SUCCESS(status)) {
        RV_LOG("PcInitializeAdapterDriver failed: 0x%08X", status);
        g_loopback.Cleanup();
        return status;
    }

    g_portClsUnload         = DriverObject->DriverUnload;
    DriverObject->DriverUnload = RvDriverUnload;

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")

extern "C" void RvDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    RV_LOG("DriverUnload");

    if (g_portClsUnload)
        g_portClsUnload(DriverObject);

    g_loopback.Cleanup();
}

extern "C" NTSTATUS RvAddDevice(_In_ PDRIVER_OBJECT DriverObject,
                                _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PAGED_CODE();
    RV_LOG("AddDevice");

    // Four subdevices: a wave and a topology filter for each direction.
    constexpr ULONG kMaxSubdevices = 4;

    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, RvStartDevice,
                              kMaxSubdevices, 0);
}

#pragma code_seg()
