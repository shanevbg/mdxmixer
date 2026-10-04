#include "endpoints.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

namespace mdxm {

namespace {
// Both enumeration helpers self-initialize COM so any control thread can call
// them. On an STA thread CoInitializeEx(MTA) returns RPC_E_CHANGED_MODE — COM is
// already up, so proceed; only balance the init we actually made.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
} // namespace

std::vector<EndpointInfo> EnumerateEndpoints() {
    std::vector<EndpointInfo> out;
    ComScope com;
    IMMDeviceEnumerator* enumr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
        return out;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(enumr->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &coll))) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev))) continue;
            EndpointInfo info;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) {
                info.id = id;
                CoTaskMemFree(id);
            }
            DWORD state = 0;
            if (SUCCEEDED(dev->GetState(&state)))
                info.isActive = (state == DEVICE_STATE_ACTIVE);
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    info.name = pv.pwszVal;
                PropVariantClear(&pv);
                props->Release();
            }
            IMMEndpoint* ep = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IMMEndpoint), (void**)&ep))) {
                EDataFlow flow = eRender;
                if (SUCCEEDED(ep->GetDataFlow(&flow)))
                    info.isRender = (flow == eRender);
                ep->Release();
            }
            dev->Release();
            if (!info.id.empty()) out.push_back(std::move(info));
        }
        coll->Release();
    }
    enumr->Release();
    return out;
}

const EndpointInfo* MatchBinding(const std::vector<EndpointInfo>& eps, const DeviceRef& want) {
    if (!want.id.empty())
        for (const auto& e : eps)
            if (e.id == want.id) return &e;
    if (want.name.empty()) return nullptr;
    const EndpointInfo* inactive = nullptr;
    for (const auto& e : eps) {
        if (_wcsicmp(e.name.c_str(), want.name.c_str()) != 0) continue;
        if (e.isActive) return &e;             // prefer active among same-named endpoints
        if (!inactive) inactive = &e;
    }
    return inactive;
}

uint32_t EndpointMixRate(const std::wstring& endpointId) {
    ComScope com;
    uint32_t rate = 0;
    IMMDeviceEnumerator* enumr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
        return 0;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(enumr->GetDevice(endpointId.c_str(), &dev))) {
        IAudioClient* client = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
            WAVEFORMATEX* wfx = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&wfx)) && wfx) {
                rate = wfx->nSamplesPerSec;
                CoTaskMemFree(wfx);
            }
            client->Release();
        }
        dev->Release();
    }
    enumr->Release();
    return rate;
}

std::wstring DefaultRenderEndpointId() {
    std::wstring out;
    ComScope com;
    IMMDeviceEnumerator* enumr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
        return out;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            out = id;
            CoTaskMemFree(id);
        }
        dev->Release();
    }
    enumr->Release();
    return out;
}

StreamFormat ParseMixFormat(const WAVEFORMATEX* wfx) {
    StreamFormat sf;
    if (!wfx) return sf;
    sf.rate = wfx->nSamplesPerSec;
    sf.channels = wfx->nChannels;
    switch (wfx->wFormatTag) {
    case WAVE_FORMAT_IEEE_FLOAT:
        if (wfx->wBitsPerSample == 32) sf.sample = StreamFormat::Sample::F32;
        break;
    case WAVE_FORMAT_PCM:
        if (wfx->wBitsPerSample == 16) sf.sample = StreamFormat::Sample::I16;
        break;
    case WAVE_FORMAT_EXTENSIBLE:
        if (wfx->cbSize >= 22) {
            const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)wfx;
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wfx->wBitsPerSample == 32)
                sf.sample = StreamFormat::Sample::F32;
            else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 16)
                sf.sample = StreamFormat::Sample::I16;
        }
        break;
    default:
        break;   // Unsupported: caller refuses the stream and reports
    }
    return sf;
}

} // namespace mdxm
