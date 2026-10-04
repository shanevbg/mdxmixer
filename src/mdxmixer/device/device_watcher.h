#pragma once
// IMMNotificationClient wrapper. Device arrival/loss comes from these events,
// never a poll (spec).
//
// SIGNAL-ONLY CONTRACT (MDropDX12 fj#401): the Callback runs INSIDE the MMDevice
// notification callback, which is dispatched under the API's client-side
// notification lock. The callback must only signal — PostMessage / SetEvent —
// and must never call MMDevice/COM device APIs, take slow locks, or block.
// Re-entering the MMDevice API from here is the documented deadlock that got
// MDropDX12 killed as a hung app after an audio failover.
#include <functional>

namespace mdxm {

class DeviceWatcher {
public:
    ~DeviceWatcher();
    using Callback = std::function<void()>;   // "device set changed" — debounced by the receiver
    bool Start(Callback onChange);            // registers with the enumerator
    void Stop();

private:
    void* m_client = nullptr;                 // NotificationClient*, opaque here
    void* m_enumerator = nullptr;             // IMMDeviceEnumerator*
};

} // namespace mdxm
