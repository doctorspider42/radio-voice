#include "Diagnostics.h"

namespace rvdiag {
namespace {

constexpr PCWSTR kKeyPath = L"RadioVoiceAudio\\Diagnostics";

/// Named counters. A fixed table rather than anything dynamic: this code runs
/// on paths where an allocation failure would be far worse than a lost counter.
constexpr ULONG kMaxCounters = 24;

struct Counter {
    WCHAR name[48];
    ULONG value;
};

Counter g_counters[kMaxCounters];
ULONG   g_counterCount = 0;

/// Guards the counter table. GetDescription can be called from more than one
/// thread while an enumeration is in flight.
KSPIN_LOCK g_lock;
BOOLEAN    g_initialised = FALSE;

} // namespace

#pragma code_seg("PAGE")

void Initialize()
{
    PAGED_CODE();

    KeInitializeSpinLock(&g_lock);
    RtlZeroMemory(g_counters, sizeof(g_counters));
    g_counterCount = 0;

    // RtlWriteRegistryValue will not create the key itself.
    const NTSTATUS status = RtlCreateRegistryKey(RTL_REGISTRY_SERVICES,
                                                 const_cast<PWSTR>(kKeyPath));
    g_initialised = NT_SUCCESS(status);

    if (!g_initialised)
        RV_LOG("diagnostics key could not be created: 0x%08X", status);
}

void Record(PCWSTR name, ULONG value)
{
    PAGED_CODE();

    if (!g_initialised)
        return;

    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, const_cast<PWSTR>(kKeyPath),
                          const_cast<PWSTR>(name), REG_DWORD, &value, sizeof(value));
}

void RecordStatus(PCWSTR name, NTSTATUS status)
{
    PAGED_CODE();

    Record(name, static_cast<ULONG>(status));

    // A second, human-readable value: 1 for success, 0 for failure. Reading
    // 0xC00000BB out of a registry dump and knowing what it means is a skill
    // nobody should need for this.
    WCHAR okName[64];
    RtlStringCchCopyW(okName, ARRAYSIZE(okName), name);
    RtlStringCchCatW(okName, ARRAYSIZE(okName), L"_ok");
    Record(okName, NT_SUCCESS(status) ? 1u : 0u);
}

#pragma code_seg()

void Count(PCWSTR name)
{
    if (!g_initialised)
        return;

    ULONG updated = 0;
    BOOLEAN found = FALSE;

    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&g_lock, &handle);

    for (ULONG i = 0; i < g_counterCount; ++i) {
        SIZE_T length = 0;
        if (NT_SUCCESS(RtlStringCchLengthW(g_counters[i].name, ARRAYSIZE(g_counters[i].name),
                                           &length)) &&
            _wcsnicmp(g_counters[i].name, name, length + 1) == 0) {
            updated = ++g_counters[i].value;
            found = TRUE;
            break;
        }
    }

    if (!found && g_counterCount < kMaxCounters) {
        RtlStringCchCopyW(g_counters[g_counterCount].name,
                          ARRAYSIZE(g_counters[g_counterCount].name), name);
        g_counters[g_counterCount].value = 1;
        updated = 1;
        ++g_counterCount;
        found = TRUE;
    }

    KeReleaseInStackQueuedSpinLock(&handle);

    // The registry write happens outside the lock, and only at PASSIVE_LEVEL -
    // every caller of this is a paged miniport method, but the check keeps a
    // future caller from turning a diagnostic into a bugcheck.
    if (found && KeGetCurrentIrql() == PASSIVE_LEVEL)
        Record(name, updated);
}

} // namespace rvdiag
