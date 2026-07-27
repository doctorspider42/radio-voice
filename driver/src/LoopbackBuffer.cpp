#include "LoopbackBuffer.h"

LoopbackBuffer g_loopback;

#pragma code_seg("PAGE")

NTSTATUS LoopbackBuffer::Initialize()
{
    PAGED_CODE();

    KeInitializeSpinLock(&m_lock);

    m_data          = nullptr;
    m_writePos      = 0;
    m_readPos       = 0;
    m_filled        = 0;
    m_renderRunning = FALSE;

    m_capacity = RV_LOOPBACK_BYTES;

    // Non-paged: both ends touch this from DPCs, where a page fault is fatal.
    m_data = static_cast<PUCHAR>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, m_capacity, RV_POOL_TAG));
    if (!m_data) {
        m_capacity = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_writePos = m_readPos = m_filled = 0;
    m_renderRunning = FALSE;

    RV_LOG("loopback ring allocated: %lu bytes (%d ms)", m_capacity, RV_LOOPBACK_MS);
    return STATUS_SUCCESS;
}

void LoopbackBuffer::Cleanup()
{
    PAGED_CODE();

    if (m_data) {
        ExFreePool(m_data);
        m_data = nullptr;
    }
    m_capacity = m_filled = m_writePos = m_readPos = 0;
}

#pragma code_seg()

void LoopbackBuffer::Reset()
{
    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_lock, &handle);

    m_writePos = m_readPos = m_filled = 0;
    if (m_data)
        RtlZeroMemory(m_data, m_capacity);

    KeReleaseInStackQueuedSpinLock(&handle);
}

ULONG LoopbackBuffer::Filled()
{
    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_lock, &handle);
    const ULONG filled = m_filled;
    KeReleaseInStackQueuedSpinLock(&handle);
    return filled;
}

void LoopbackBuffer::Write(const void* data, ULONG bytes)
{
    if (!m_data || bytes == 0)
        return;

    const PUCHAR source = static_cast<const PUCHAR>(const_cast<void*>(data));

    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_lock, &handle);

    // A write larger than the whole ring can only be satisfied by its tail.
    ULONG offset = 0;
    ULONG toWrite = bytes;
    if (toWrite > m_capacity) {
        offset  = toWrite - m_capacity;
        toWrite = m_capacity;
    }

    // Make room by discarding the oldest audio rather than dropping the newest:
    // the newest is what the listener is about to hear.
    const ULONG free = m_capacity - m_filled;
    if (toWrite > free) {
        const ULONG discard = toWrite - free;
        m_readPos = (m_readPos + discard) % m_capacity;
        m_filled -= discard;
    }

    const ULONG firstChunk = min(toWrite, m_capacity - m_writePos);
    RtlCopyMemory(m_data + m_writePos, source + offset, firstChunk);
    if (toWrite > firstChunk)
        RtlCopyMemory(m_data, source + offset + firstChunk, toWrite - firstChunk);

    m_writePos = (m_writePos + toWrite) % m_capacity;
    m_filled  += toWrite;

    KeReleaseInStackQueuedSpinLock(&handle);
}

void LoopbackBuffer::Read(void* data, ULONG bytes)
{
    if (bytes == 0)
        return;

    const PUCHAR destination = static_cast<PUCHAR>(data);

    if (!m_data) {
        RtlZeroMemory(destination, bytes);
        return;
    }

    KLOCK_QUEUE_HANDLE handle;
    KeAcquireInStackQueuedSpinLock(&m_lock, &handle);

    const ULONG available = min(bytes, m_filled);

    if (available > 0) {
        const ULONG firstChunk = min(available, m_capacity - m_readPos);
        RtlCopyMemory(destination, m_data + m_readPos, firstChunk);
        if (available > firstChunk)
            RtlCopyMemory(destination + firstChunk, m_data, available - firstChunk);

        m_readPos = (m_readPos + available) % m_capacity;
        m_filled -= available;
    }

    KeReleaseInStackQueuedSpinLock(&handle);

    // Padding happens outside the lock: it can be the whole buffer, and holding
    // a spin lock across a large memset would raise the interrupt latency of
    // the entire system.
    if (available < bytes)
        RtlZeroMemory(destination + available, bytes - available);
}
