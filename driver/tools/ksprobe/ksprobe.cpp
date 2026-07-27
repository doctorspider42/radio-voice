/*
    ksprobe - asks a KS audio filter the same questions the audio stack asks.

    Written for one specific situation: a driver that loads, registers its
    filters and is then ignored by the audio endpoint builder. From the outside
    that is indistinguishable from a driver with a subtly wrong topology, and
    nothing in the kernel says which it is.

    This opens the filters from user mode and dumps what they actually answer -
    pin count, dataflow, communication, category, and the physical connection
    that links a topology filter to its wave filter. Run it against a working
    virtual cable and against the driver under test, and the difference is the
    bug.

    Build:  build.cmd     (in this directory)
    Usage:  ksprobe.exe [substring]
            with no argument, every audio topology and wave filter is listed.
*/

#include <windows.h>

#include <initguid.h>
#include <setupapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

//-----------------------------------------------------------------------------
// GUIDs
//
// ks.h declares these as extern and expects an import library to define them,
// and which library that is varies between toolchains. The STATIC_ forms are
// not usable as initialisers everywhere either. Spelling the values out removes
// every dependency - they are fixed constants that have not changed since the
// nineties.
//-----------------------------------------------------------------------------

const GUID kCategoryTopology =
    {0xDDA54A40, 0x1E4C, 0x11D1, {0xA0, 0x50, 0x40, 0x57, 0x05, 0xC1, 0x00, 0x00}};
const GUID kCategoryAudio =
    {0x6994AD04, 0x93EF, 0x11D0, {0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID kCategoryRender =
    {0x65E8773E, 0x8F56, 0x11D0, {0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID kCategoryCapture =
    {0x65E8773D, 0x8F56, 0x11D0, {0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID kPropSetPin =
    {0x8C134960, 0x51AD, 0x11CF, {0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00}};
const GUID kNodeSpeaker =
    {0xDFF21CE1, 0xF70F, 0x11D0, {0xB9, 0x17, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID kNodeMicrophone =
    {0xDFF21BE1, 0xF70F, 0x11D0, {0xB9, 0x17, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID kNodeLineConnector =
    {0xDFF21FE3, 0xF70F, 0x11D0, {0xB9, 0x17, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};

// Wave format subtypes, so the format dump reads as names rather than GUIDs.
const GUID kSubtypePcm =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypeFloat =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypeAnalog =
    {0x6DBA3190, 0x67BD, 0x11CF, {0xA0, 0xF7, 0x00, 0x20, 0xAF, 0xD1, 0x56, 0xE4}};

//-----------------------------------------------------------------------------
// Enumeration
//-----------------------------------------------------------------------------

std::vector<std::wstring> interfacePaths(const GUID& category)
{
    std::vector<std::wstring> paths;

    HDEVINFO set = SetupDiGetClassDevsW(&category, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return paths;

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &category, index, &interfaceData);
         ++index) {

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;

        std::vector<BYTE> buffer(needed);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, needed, nullptr,
                                             nullptr)) {
            paths.emplace_back(detail->DevicePath);
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

//-----------------------------------------------------------------------------
// Property helpers
//-----------------------------------------------------------------------------

bool pinProperty(HANDLE filter, ULONG pinId, ULONG propertyId,
                 void* result, ULONG resultSize, ULONG* returned = nullptr)
{
    KSP_PIN request{};
    request.Property.Set   = kPropSetPin;
    request.Property.Id    = propertyId;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId          = pinId;

    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY,
                                    &request, sizeof(request),
                                    result, resultSize, &bytes, nullptr);
    if (returned)
        *returned = bytes;
    return ok != FALSE;
}

const char* dataflowName(KSPIN_DATAFLOW flow)
{
    switch (flow) {
        case KSPIN_DATAFLOW_IN:  return "IN";
        case KSPIN_DATAFLOW_OUT: return "OUT";
        default:                 return "?";
    }
}

const char* communicationName(KSPIN_COMMUNICATION communication)
{
    switch (communication) {
        case KSPIN_COMMUNICATION_NONE:   return "NONE  (bridge)";
        case KSPIN_COMMUNICATION_SINK:   return "SINK  (stream)";
        case KSPIN_COMMUNICATION_SOURCE: return "SOURCE";
        case KSPIN_COMMUNICATION_BOTH:   return "BOTH";
        case KSPIN_COMMUNICATION_BRIDGE: return "BRIDGE";
        default:                         return "?";
    }
}

/// Names the node-type GUIDs that decide whether the audio stack treats a pin
/// as a physical connector - which is what makes an endpoint appear.
const char* categoryName(const GUID& guid)
{
    struct Known { const GUID* guid; const char* name; };
    static const Known known[] = {
        {&kNodeSpeaker,         "KSNODETYPE_SPEAKER"},
        {&kNodeMicrophone,      "KSNODETYPE_MICROPHONE"},
        {&kNodeLineConnector,   "KSNODETYPE_LINE_CONNECTOR"},
        {&kCategoryAudio,       "KSCATEGORY_AUDIO"},
        {&kCategoryRender,      "KSCATEGORY_RENDER"},
        {&kCategoryCapture,     "KSCATEGORY_CAPTURE"},
        {&kSubtypePcm,          "PCM"},
        {&kSubtypeFloat,        "IEEE_FLOAT"},
        {&kSubtypeAnalog,       "ANALOG"},
    };

    for (const auto& entry : known) {
        if (IsEqualGUID(guid, *entry.guid))
            return entry.name;
    }
    return nullptr;
}

void printGuid(const GUID& guid)
{
    if (const char* name = categoryName(guid)) {
        std::printf("%s", name);
        return;
    }
    std::printf("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                guid.Data1, guid.Data2, guid.Data3,
                guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

//-----------------------------------------------------------------------------
// Reporting
//-----------------------------------------------------------------------------

void describeFilter(const std::wstring& path)
{
    std::printf("\n%ws\n", path.c_str());

    HANDLE filter = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (filter == INVALID_HANDLE_VALUE) {
        std::printf("    cannot open: error %lu\n", GetLastError());
        return;
    }

    ULONG pinCount = 0;
    if (!pinProperty(filter, 0, KSPROPERTY_PIN_CTYPES, &pinCount, sizeof(pinCount))) {
        std::printf("    KSPROPERTY_PIN_CTYPES failed: error %lu\n", GetLastError());
        CloseHandle(filter);
        return;
    }

    std::printf("    %lu pin(s)\n", pinCount);

    for (ULONG pin = 0; pin < pinCount; ++pin) {
        KSPIN_DATAFLOW      flow{};
        KSPIN_COMMUNICATION communication{};
        GUID                category{};

        const bool haveFlow =
            pinProperty(filter, pin, KSPROPERTY_PIN_DATAFLOW, &flow, sizeof(flow));
        const bool haveComm =
            pinProperty(filter, pin, KSPROPERTY_PIN_COMMUNICATION,
                        &communication, sizeof(communication));
        const bool haveCategory =
            pinProperty(filter, pin, KSPROPERTY_PIN_CATEGORY, &category, sizeof(category));

        std::printf("      pin %lu: dataflow %-4s  communication %-15s  category ",
                    pin,
                    haveFlow ? dataflowName(flow) : "n/a",
                    haveComm ? communicationName(communication) : "n/a");

        if (haveCategory)
            printGuid(category);
        else
            std::printf("n/a");
        std::printf("\n");

        // Formats. The audio stack has to settle on one before it can build an
        // endpoint, so a pin that offers nothing it can use is as good as a pin
        // that is not there - and it fails silently, with no error anywhere.
        if (haveComm && communication != KSPIN_COMMUNICATION_NONE) {
            ULONG needed = 0;
            pinProperty(filter, pin, KSPROPERTY_PIN_DATARANGES, nullptr, 0, &needed);

            std::vector<BYTE> ranges(needed ? needed : 4096);
            ULONG returned = 0;
            if (pinProperty(filter, pin, KSPROPERTY_PIN_DATARANGES, ranges.data(),
                            static_cast<ULONG>(ranges.size()), &returned) &&
                returned >= sizeof(KSMULTIPLE_ITEM)) {

                auto* multiple = reinterpret_cast<KSMULTIPLE_ITEM*>(ranges.data());
                auto* cursor = reinterpret_cast<BYTE*>(multiple + 1);

                for (ULONG i = 0; i < multiple->Count; ++i) {
                    auto* range = reinterpret_cast<KSDATARANGE*>(cursor);

                    std::printf("               format: ");
                    if (range->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                        auto* audio = reinterpret_cast<KSDATARANGE_AUDIO*>(range);
                        std::printf("%lu-%lu Hz, up to %lu ch, %lu-%lu bits, subtype ",
                                    audio->MinimumSampleFrequency,
                                    audio->MaximumSampleFrequency,
                                    audio->MaximumChannels,
                                    audio->MinimumBitsPerSample,
                                    audio->MaximumBitsPerSample);
                    }
                    printGuid(range->SubFormat);
                    std::printf("\n");

                    cursor += ((range->FormatSize + 7) & ~7ul); // ranges are 8-byte aligned
                }
            }
        }

        // The link from a topology filter to its wave filter. Its absence on a
        // bridge pin is what stops the audio stack finding the streaming half
        // of the device, and therefore what stops an endpoint being built.
        std::vector<BYTE> connection(512);
        ULONG returned = 0;
        if (pinProperty(filter, pin, KSPROPERTY_PIN_PHYSICALCONNECTION,
                        connection.data(), static_cast<ULONG>(connection.size()), &returned) &&
            returned >= sizeof(KSPIN_PHYSICALCONNECTION)) {

            auto* physical = reinterpret_cast<KSPIN_PHYSICALCONNECTION*>(connection.data());
            std::printf("               -> physical connection to pin %lu of %ws\n",
                        physical->Pin, physical->SymbolicLinkName);
        }
    }

    CloseHandle(filter);
}

void probeCategory(const char* title, const GUID& category, const std::wstring& filter)
{
    std::printf("\n================ %s ================\n", title);

    const auto paths = interfacePaths(category);
    if (paths.empty()) {
        std::printf("  (no interfaces)\n");
        return;
    }

    int shown = 0;
    for (const auto& path : paths) {
        if (!filter.empty()) {
            std::wstring lower = path;
            for (auto& c : lower)
                c = static_cast<wchar_t>(towlower(c));
            if (lower.find(filter) == std::wstring::npos)
                continue;
        }
        describeFilter(path);
        ++shown;
    }

    if (shown == 0)
        std::printf("  (nothing matched the filter)\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring filter;
    if (argc > 1) {
        filter = argv[1];
        for (auto& c : filter)
            c = static_cast<wchar_t>(towlower(c));
    }

    std::printf("ksprobe - what the audio stack sees\n");
    if (!filter.empty())
        std::printf("filter: %ws\n", filter.c_str());

    probeCategory("KSCATEGORY_TOPOLOGY", kCategoryTopology, filter);
    probeCategory("KSCATEGORY_AUDIO",    kCategoryAudio,    filter);

    return 0;
}
