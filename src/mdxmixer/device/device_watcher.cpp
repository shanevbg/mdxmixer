#include "device_watcher.h"
#include "app/thread_guard.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <atomic>

namespace mdxm {

namespace {

class NotificationClient : public IMMNotificationClient {
public:
    explicit NotificationClient(DeviceWatcher::Callback cb) : m_cb(std::move(cb)) {}

    // Every event collapses to "the device set changed"; the receiver re-enumerates
    // on its own thread. Nothing here touches the MMDevice API (fj#401).
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { Signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

private:
    void Signal() {
        // A REAL SEH guard, not only a try/catch. These callbacks arrive on an
        // audio-stack thread, and an exception that escapes one is turned into
        // a FATAL 0xC000041D by Windows -- catch(...) under /EHsc does not
        // catch an access violation, so a fault here would take the process
        // down from inside AudioSes.dll with nothing in our own log. Seen on
        // this machine on 2026-10-03.
        if (!m_cb) return;
        RunGuarded([](void* p) { (*(DeviceWatcher::Callback*)p)(); }, &m_cb);
    }
    DeviceWatcher::Callback m_cb;
    std::atomic<ULONG> m_refs{1};
};

} // namespace

DeviceWatcher::~DeviceWatcher() { Stop(); }

bool DeviceWatcher::Start(Callback onChange) {
    Stop();
    IMMDeviceEnumerator* enumr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
        return false;
    NotificationClient* client = new NotificationClient(std::move(onChange));
    if (FAILED(enumr->RegisterEndpointNotificationCallback(client))) {
        client->Release();
        enumr->Release();
        return false;
    }
    m_client = client;
    m_enumerator = enumr;
    return true;
}

void DeviceWatcher::Stop() {
    if (m_enumerator && m_client) {
        auto* enumr = (IMMDeviceEnumerator*)m_enumerator;
        auto* client = (NotificationClient*)m_client;
        enumr->UnregisterEndpointNotificationCallback(client);
        client->Release();
        enumr->Release();
    }
    m_client = nullptr;
    m_enumerator = nullptr;
}

} // namespace mdxm
