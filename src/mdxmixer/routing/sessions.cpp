#include "sessions.h"
#include "device/endpoints.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audiopolicy.h>
#include <objbase.h>

namespace mdxm {

namespace {
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

std::wstring ProcessImagePath(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) out.assign(buf, len);
    CloseHandle(h);
    return out;
}
} // namespace

std::vector<SessionInfo> EnumerateSessions() {
    std::vector<SessionInfo> out;
    try {
        ComScope com;
        IMMDeviceEnumerator* enumr = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
            return out;
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                std::wstring epId, epName;
                LPWSTR id = nullptr;
                if (SUCCEEDED(dev->GetId(&id)) && id) { epId = id; CoTaskMemFree(id); }
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        epName = pv.pwszVal;
                    PropVariantClear(&pv);
                    props->Release();
                }
                IAudioSessionManager2* mgr = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) {
                    IAudioSessionEnumerator* sessions = nullptr;
                    if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions))) {
                        int n = 0;
                        sessions->GetCount(&n);
                        for (int s = 0; s < n; ++s) {
                            IAudioSessionControl* ctl = nullptr;
                            if (FAILED(sessions->GetSession(s, &ctl))) continue;
                            IAudioSessionControl2* ctl2 = nullptr;
                            if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctl2))) {
                                DWORD pid = 0;
                                if (SUCCEEDED(ctl2->GetProcessId(&pid)) && pid != 0 &&
                                    ctl2->IsSystemSoundsSession() != S_OK) {
                                    SessionInfo info;
                                    info.pid = pid;
                                    info.exePath = ProcessImagePath(pid);
                                    info.endpointId = epId;
                                    info.endpointName = epName;
                                    AudioSessionState state = AudioSessionStateInactive;
                                    ctl->GetState(&state);
                                    info.active = (state == AudioSessionStateActive);
                                    if (!info.exePath.empty()) out.push_back(std::move(info));
                                }
                                ctl2->Release();
                            }
                            ctl->Release();
                        }
                        sessions->Release();
                    }
                    mgr->Release();
                }
                dev->Release();
            }
            coll->Release();
        }
        enumr->Release();
    } catch (...) {}   // never throws
    return out;
}

} // namespace mdxm
