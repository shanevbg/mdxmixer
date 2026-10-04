#include "routing/default_endpoint.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <objbase.h>

namespace mdxm {

namespace {

// IPolicyConfig — undocumented, and the only way to move the default endpoint.
// Declared here because the SDK ships no header for it; the CLSID and IID are
// the Windows 7+ values every audio switcher uses. THE VTABLE ORDER MATTERS:
// SetDefaultEndpoint must sit at exactly this slot or the call lands on a
// different method. Copied from MDropDX12, where it is known to work on this
// machine's Windows build.
const CLSID CLSID_CPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const IID IID_IPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, INT64*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct Work {
    std::wstring endpointId;
    bool ok = false;
    std::wstring err;
};

// The actual write, on a thread of its own and in the MULTITHREADED apartment.
//
// This is the whole reason there is a thread here, and it was measured the
// hard way: run on the UI thread, this crashed mdxmixer in AudioSes.dll with
// 0xC000041D (a fatal exception inside a user callback), on 2026-10-03 at
// 12:48:10.
//
// Changing the default endpoint makes Windows call every registered
// IMMNotificationClient back -- including ours. The UI thread is an STA, and
// an STA PUMPS MESSAGES while it waits inside a COM call, so the window
// procedure re-entered in the middle of SetDefaultEndpoint, ran device-change
// work there, and whatever it threw unwound out through the audio stack rather
// than through us. An MTA thread pumps nothing, so the call is atomic from the
// UI's point of view and nothing can re-enter.
//
// It also means the UI thread never blocks on an audio-stack call that another
// thread may be holding, which is the same rule the notification callbacks
// already follow (fj#401).
DWORD WINAPI WriteDefaultThread(LPVOID param) {
    Work* w = (Work*)param;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IPolicyConfig* policy = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
                                IID_IPolicyConfig, (void**)&policy)) || !policy) {
        w->err = L"IPolicyConfig is not available on this Windows build";
    } else {
        // All three roles -- see the header for why, and for what it costs.
        bool ok = SUCCEEDED(policy->SetDefaultEndpoint(w->endpointId.c_str(), eConsole));
        ok = SUCCEEDED(policy->SetDefaultEndpoint(w->endpointId.c_str(), eMultimedia)) && ok;
        ok = SUCCEEDED(policy->SetDefaultEndpoint(w->endpointId.c_str(), eCommunications)) && ok;
        policy->Release();
        w->ok = ok;
        if (!ok) w->err = L"Windows refused the default-endpoint change";
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return 0;
}

} // namespace

bool SetDefaultRenderEndpoint(const std::wstring& endpointId, std::wstring* err) {
    auto fail = [err](const wchar_t* why) { if (err) *err = why; return false; };
    if (endpointId.empty()) return fail(L"no endpoint given");

    Work w;
    w.endpointId = endpointId;
    HANDLE th = CreateThread(nullptr, 0, WriteDefaultThread, &w, 0, nullptr);
    if (!th) return fail(L"could not start the default-endpoint thread");
    // Bounded: a wedged audio stack must not take the caller with it. Five
    // seconds is far beyond the few milliseconds the call takes when the stack
    // is healthy, and is the same shape of bounded wait the engine uses.
    DWORD waited = WaitForSingleObject(th, 5000);
    CloseHandle(th);
    if (waited != WAIT_OBJECT_0)
        return fail(L"the default-endpoint change did not come back in 5 s");
    if (!w.ok) return fail(w.err.empty() ? L"default-endpoint change failed" : w.err.c_str());
    return true;
}

} // namespace mdxm
