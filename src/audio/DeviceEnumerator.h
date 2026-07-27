#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include "audio/DeviceInfo.h"
#include "core/Types.h"

namespace rv::audio {

/// Aggregates the device lists of every backend into one table.
///
/// Enumeration is cheap (a few milliseconds), so rather than subscribing to
/// endpoint notifications the UI simply calls `refreshIfStale` on a timer. That
/// avoids an `IMMNotificationClient` whose callbacks arrive on arbitrary
/// threads and would need their own synchronisation for no practical gain.
class DeviceEnumerator {
public:
    /// Re-enumerates every backend. Requires COM on the calling thread.
    void refresh();

    /// Re-enumerates only if more than `maxAgeMs` have passed.
    void refreshIfStale(int maxAgeMs = 2000);

    std::vector<DeviceInfo> inputs(BackendType backend) const;
    std::vector<DeviceInfo> outputs(BackendType backend) const;

    /// All devices across all backends, in enumeration order.
    std::vector<DeviceInfo> all() const;

    std::optional<DeviceInfo> find(const DeviceId& id, BackendType backend) const;

    /// Output endpoints that look like a virtual cable - the ones worth
    /// suggesting as the destination, because selecting one is what makes the
    /// processed signal available to other applications.
    std::vector<DeviceInfo> virtualCableOutputs() const;

    bool anyVirtualCableInstalled() const;

    /// Sample rates the endpoint accepts in WASAPI exclusive mode. Probing is
    /// several device activations, so it is deliberately on demand rather than
    /// part of `refresh`.
    static std::vector<int> probeExclusiveRates(const DeviceId& id, int channels);

private:
    void enumerateWasapi(std::vector<DeviceInfo>& out);
    void enumerateDirectSound(std::vector<DeviceInfo>& out);
    void enumerateAsio(std::vector<DeviceInfo>& out);

    mutable std::mutex      mutex_;
    std::vector<DeviceInfo> devices_;
    u64                     lastRefreshMs_ = 0;
};

} // namespace rv::audio
