#include "audio_policy_config.h"
#include <windows.h>
#include <mmdeviceapi.h>      // EDataFlow / ERole
#include <hstring.h>
#include <winstring.h>
#include <roapi.h>

#pragma comment(lib, "runtimeobject.lib")

namespace mdxm {

namespace {

constexpr wchar_t kMmdevapiToken[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kRenderSuffix[]  = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"; // DEVINTERFACE_AUDIO_RENDER

// The vtable EarTrumpet documents for Windows.Media.Internal.AudioPolicyConfig.
// Only the last three slots are called; the padding slots keep the layout exact
// and each carries its real name.
struct __declspec(novtable) IAudioPolicyConfigFactoryVtbl : IUnknown {
    virtual HRESULT __stdcall Unused1() = 0;   // add_CtxVolumeChange
    virtual HRESULT __stdcall Unused2() = 0;   // remove_CtxVolumeChanged
    virtual HRESULT __stdcall Unused3() = 0;   // add_RingerVibrateStateChanged
    virtual HRESULT __stdcall Unused4() = 0;   // remove_RingerVibrateStateChange
    virtual HRESULT __stdcall Unused5() = 0;   // SetVolumeGroupGainForId
    virtual HRESULT __stdcall Unused6() = 0;   // GetVolumeGroupGainForId
    virtual HRESULT __stdcall Unused7() = 0;   // GetActiveVolumeGroupForEndpointId
    virtual HRESULT __stdcall Unused8() = 0;   // GetVolumeGroupsForEndpoint
    virtual HRESULT __stdcall Unused9() = 0;   // GetCurrentVolumeContext
    virtual HRESULT __stdcall Unused10() = 0;  // SetVolumeGroupMuteForId
    virtual HRESULT __stdcall Unused11() = 0;  // GetVolumeGroupMuteForId
    virtual HRESULT __stdcall Unused12() = 0;  // SetRingerVibrateState
    virtual HRESULT __stdcall Unused13() = 0;  // GetRingerVibrateState
    virtual HRESULT __stdcall Unused14() = 0;  // SetPreferredChatApplication
    virtual HRESULT __stdcall Unused15() = 0;  // ResetPreferredChatApplication
    virtual HRESULT __stdcall Unused16() = 0;  // GetPreferredChatApplication
    virtual HRESULT __stdcall Unused17() = 0;  // GetCurrentChatApplications
    virtual HRESULT __stdcall Unused18() = 0;  // add_ChatContextChanged
    virtual HRESULT __stdcall Unused19() = 0;  // remove_ChatContextChanged
    virtual HRESULT __stdcall SetPersistedDefaultAudioEndpoint(UINT processId, EDataFlow flow,
                                                               ERole role, HSTRING deviceId) = 0;
    virtual HRESULT __stdcall GetPersistedDefaultAudioEndpoint(UINT processId, EDataFlow flow,
                                                               ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT __stdcall ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// {ab3d4648-e242-459f-b02f-541c70306324} — Windows 11 21H2+
constexpr GUID kIidWin11 = { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };
// {2a59116d-6c4f-45e0-a74f-707e3fef9258} — Windows 10
constexpr GUID kIidWin10 = { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };

struct HStr {   // tiny HSTRING RAII
    HSTRING h = nullptr;
    explicit HStr(const std::wstring& s) { WindowsCreateString(s.c_str(), (UINT32)s.size(), &h); }
    HStr() = default;
    ~HStr() { if (h) WindowsDeleteString(h); }
};

std::wstring FmtHr(const wchar_t* what, HRESULT hr) {
    wchar_t buf[64];
    swprintf(buf, 64, L" (hr=0x%08X)", (unsigned)hr);
    return std::wstring(what) + buf;
}

} // namespace

std::wstring MakePolicyDeviceId(const std::wstring& mmDeviceId) {
    return std::wstring(kMmdevapiToken) + mmDeviceId + kRenderSuffix;
}

std::wstring ParsePolicyDeviceId(const std::wstring& policyId) {
    const std::wstring token = kMmdevapiToken;
    const std::wstring suffix = kRenderSuffix;
    if (policyId.size() > token.size() + suffix.size() &&
        policyId.compare(0, token.size(), token) == 0 &&
        policyId.compare(policyId.size() - suffix.size(), suffix.size(), suffix) == 0)
        return policyId.substr(token.size(), policyId.size() - token.size() - suffix.size());
    return policyId;   // unrecognized passes through
}

AudioPolicyConfig::~AudioPolicyConfig() {
    if (m_factory) ((IAudioPolicyConfigFactoryVtbl*)m_factory)->Release();
    m_factory = nullptr;
    if (m_comInited) CoUninitialize();
    m_comInited = false;
}

bool AudioPolicyConfig::Init(std::wstring* err) {
    if (m_factory) return true;
    // Join an apartment if the calling thread has none. RPC_E_CHANGED_MODE means
    // COM is already up in the other model — fine, proceed and do not uninit it.
    if (!m_comInited) {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInited = SUCCEEDED(hrCo);
    }
    HStr className(L"Windows.Media.Internal.AudioPolicyConfig");
    IAudioPolicyConfigFactoryVtbl* factory = nullptr;
    HRESULT hr = RoGetActivationFactory(className.h, kIidWin11, (void**)&factory);
    if (SUCCEEDED(hr)) {
        m_iidUsed = L"win11";
    } else {
        hr = RoGetActivationFactory(className.h, kIidWin10, (void**)&factory);
        if (SUCCEEDED(hr)) m_iidUsed = L"win10";
    }
    if (FAILED(hr)) {
        if (err) *err = FmtHr(L"AudioPolicyConfig factory unavailable", hr);
        return false;
    }
    m_factory = factory;
    return true;
}

bool AudioPolicyConfig::SetPersistedDefaultRender(unsigned long pid, const std::wstring& endpointId,
                                                 std::wstring* err) {
    if (!m_factory) { if (err) *err = L"policy API not available"; return false; }
    if (endpointId.empty()) { if (err) *err = L"empty endpoint id"; return false; }
    auto* f = (IAudioPolicyConfigFactoryVtbl*)m_factory;
    HStr dev(MakePolicyDeviceId(endpointId));
    HRESULT hr = f->SetPersistedDefaultAudioEndpoint((UINT)pid, eRender, eConsole, dev.h);
    if (SUCCEEDED(hr)) hr = f->SetPersistedDefaultAudioEndpoint((UINT)pid, eRender, eMultimedia, dev.h);
    if (FAILED(hr)) { if (err) *err = FmtHr(L"SetPersistedDefaultAudioEndpoint failed", hr); return false; }
    return true;
}

bool AudioPolicyConfig::ClearPersistedDefaultRender(unsigned long pid, std::wstring* err) {
    if (!m_factory) { if (err) *err = L"policy API not available"; return false; }
    auto* f = (IAudioPolicyConfigFactoryVtbl*)m_factory;
    HRESULT hr = f->SetPersistedDefaultAudioEndpoint((UINT)pid, eRender, eConsole, nullptr);
    if (SUCCEEDED(hr)) hr = f->SetPersistedDefaultAudioEndpoint((UINT)pid, eRender, eMultimedia, nullptr);
    if (FAILED(hr)) { if (err) *err = FmtHr(L"clear persisted endpoint failed", hr); return false; }
    return true;
}

bool AudioPolicyConfig::GetPersistedDefaultRender(unsigned long pid, std::wstring* endpointId) {
    if (!m_factory || !endpointId) return false;
    auto* f = (IAudioPolicyConfigFactoryVtbl*)m_factory;
    HSTRING dev = nullptr;
    if (FAILED(f->GetPersistedDefaultAudioEndpoint((UINT)pid, eRender, eConsole, &dev)) || !dev)
        return false;
    UINT32 len = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(dev, &len);
    *endpointId = ParsePolicyDeviceId(std::wstring(raw, len));
    WindowsDeleteString(dev);
    return !endpointId->empty();
}

} // namespace mdxm
