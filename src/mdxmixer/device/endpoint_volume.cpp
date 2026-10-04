#include "endpoint_volume.h"
#include "app/thread_guard.h"
#include "app/log.h"
#include "device/endpoints.h"
#include "device/device_identity.h"
#include "device/device_info.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>
#include <algorithm>

namespace mdxm {

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// Opens the endpoint volume interface for one device id. Caller releases.
IAudioEndpointVolume* OpenVolume(const std::wstring& endpointId) {
    if (endpointId.empty()) return nullptr;
    IMMDeviceEnumerator* enumr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)) || !enumr)
        return nullptr;
    IMMDevice* dev = nullptr;
    HRESULT hr = enumr->GetDevice(endpointId.c_str(), &dev);
    enumr->Release();
    if (FAILED(hr) || !dev) return nullptr;
    IAudioEndpointVolume* vol = nullptr;
    hr = dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol);
    dev->Release();
    return SUCCEEDED(hr) ? vol : nullptr;
}

std::wstring DefaultIdFor(IMMDeviceEnumerator* enumr, EDataFlow flow) {
    std::wstring out;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(flow, eConsole, &dev)) && dev) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) { out = id; CoTaskMemFree(id); }
        dev->Release();
    }
    return out;
}

} // namespace

// The whole sweep, behind a REAL SEH guard.
//
// This is called four times a second from the UI thread and activates COM
// interfaces on every endpoint -- IAudioEndpointVolume, and since the peak
// meters, IAudioMeterInformation as well. On this machine the Bluetooth
// headsets connect and drop every half minute, so some of those endpoints are
// dying WHILE they are being read, and AudioSes.dll faults inside the call.
//
// Measured on 2026-10-03: mdxmixer died 86 seconds after a clean start, in
// AudioSes.dll at offset 0xf057f -- 0xC0000005 then 0xC000041D -- which is the
// same fault site as the morning's crashes. The earlier guards covered
// Engine::Start and the device notifications; this path had none, and it is
// the one that runs constantly.
//
// A fault here costs the frame's readings, not the process: the caller gets
// whatever was collected before it, and tries again in 250 ms.
static void ListEndpointVolumesBody(void* ctx);

std::vector<DeviceLevel> ListEndpointVolumes() {
    std::vector<DeviceLevel> out;
    if (!RunGuarded(&ListEndpointVolumesBody, &out))
        Log(1, L"endpoint sweep: the audio stack faulted; keeping %zu reading(s)", out.size());
    return out;
}

static void ListEndpointVolumesBody(void* ctx) {
    std::vector<DeviceLevel>& out = *(std::vector<DeviceLevel>*)ctx;
    try {
        ComScope com;
        // Battery and last-connected live on a Bluetooth PnP node, joined to
        // the endpoint by ContainerId. Read once for the whole sweep.
        const std::vector<BluetoothInfo> bt = ReadBluetoothInfo();
        IMMDeviceEnumerator* enumr = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&enumr)) || !enumr)
            return;
        const std::wstring defRender = DefaultIdFor(enumr, eRender);
        const std::wstring defCapture = DefaultIdFor(enumr, eCapture);

        for (EDataFlow flow : { eRender, eCapture }) {
            IMMDeviceCollection* coll = nullptr;
            // ACTIVE **and** UNPLUGGED, deliberately.
            //
            // Listing only the active ones made a disconnected headset
            // invisible, and five identical pairs of earbuds are disconnected
            // most of the time. That is why "I can't tell when the last time a
            // bluetooth headset was connected" and "I can't rename headsets so
            // all the headsets have random numbers" are the same bug: a device
            // that is not in the list cannot show its last-seen time and
            // cannot be given a name. MDropDX12's failover list shows every
            // device Windows knows about for exactly this reason -- "the
            // useful entries are exactly the ones switched off".
            //
            // DISABLED and NOTPRESENT are left out: those are devices the user
            // has turned off in Windows or that have been removed, not ones
            // waiting to be reconnected.
            const DWORD kStates = DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED;
            if (FAILED(enumr->EnumAudioEndpoints(flow, kStates, &coll)) || !coll)
                continue;
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev)) || !dev) continue;
                DeviceLevel lvl;
                lvl.isRender = (flow == eRender);
                LPWSTR id = nullptr;
                if (SUCCEEDED(dev->GetId(&id)) && id) { lvl.id = id; CoTaskMemFree(id); }
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        lvl.name = pv.pwszVal;
                    PropVariantClear(&pv);
                    props->Release();
                }
                // Is it actually there? An unplugged endpoint cannot be
                // activated for volume or metering, and asking is both
                // pointless and a fault risk.
                DWORD devState = DEVICE_STATE_ACTIVE;
                dev->GetState(&devState);
                lvl.active = (devState == DEVICE_STATE_ACTIVE);

                // Only an ACTIVE endpoint has a volume or a meter. An
                // unplugged one is listed for its identity alone: its name,
                // its battery and when it was last seen.
                if (lvl.active) {
                    IAudioEndpointVolume* vol = nullptr;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                                nullptr, (void**)&vol)) && vol) {
                        float scalar = 0.0f;
                        if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&scalar))) lvl.vol = scalar;
                        BOOL muted = FALSE;
                        if (SUCCEEDED(vol->GetMute(&muted))) lvl.mute = (muted != FALSE);
                        vol->Release();
                        lvl.volumeKnown = true;
                    }
                    // What is actually flowing, alongside what the level is
                    // set to: the reading that says whether audio is reaching
                    // the device at all.
                    IAudioMeterInformation* meter = nullptr;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioMeterInformation),
                                                CLSCTX_ALL, nullptr, (void**)&meter)) && meter) {
                        float peak = 0.0f;
                        if (SUCCEEDED(meter->GetPeakValue(&peak))) lvl.peak = peak;
                        meter->Release();
                    }
                }

                // Identity, for every endpoint whatever its state. This is
                // the half that was trapped inside the volume branch, which
                // is why a disconnected headset could be neither named nor
                // dated.
                lvl.isDefault = (!lvl.id.empty() &&
                                 lvl.id == (lvl.isRender ? defRender : defCapture));
                lvl.isHandsFree = IsHandsFreeName(lvl.name);
                lvl.displayName = lvl.name;   // the app layer applies aliases
                std::wstring container = EndpointContainerId(lvl.id);
                lvl.containerId = container;
                lvl.present = lvl.active;
                if (!container.empty()) {
                    for (const auto& b : bt)
                        if (b.containerId == container) {
                            lvl.battery = b.battery;
                            lvl.btAddress = b.btAddress;
                            // The Bluetooth node can only ADD presence, never
                            // take it away.
                            //
                            // It used to be assigned outright, and that is the
                            // bug Shane hit: "way down the list I finally
                            // figured out the device that supposedly hasn't
                            // connected since sept 27 was actually the
                            // currently connected device" -- a row carrying a
                            // live peak meter while its status column read a
                            // week-old date.
                            //
                            // The two sources are not equals. lvl.active comes
                            // from the endpoint's own DEVICE_STATE_ACTIVE: the
                            // endpoint exists, Windows will open a stream on
                            // it, and this process is reading peaks off it.
                            // b.present comes from the BTHENUM device node for
                            // the radio's service, which goes not-present on
                            // its own schedule -- a connected A2DP headset is
                            // routinely reported absent there while its
                            // endpoint plays. Letting that overwrite an ACTIVE
                            // endpoint means the one device in the list you
                            // can actually hear is the one labelled gone.
                            //
                            // So presence is the OR: the endpoint's own state
                            // wins when it says yes, and the Bluetooth node is
                            // consulted only for an endpoint Windows has
                            // parked as UNPLUGGED, which is the case it is
                            // genuinely better at.
                            lvl.present = lvl.active || b.present;
                            lvl.lastConnectedUtc = LocalFileTimeToUtc(b.lastConnectedRaw);
                            break;
                        }
                }
                // The LATER of the two stamps.
                //
                // The Bluetooth property is per-device and months deep but
                // Windows does not reliably refresh it -- measured here,
                // headsets used the same day still reported 27 and 28
                // September, which makes it useless for choosing one. The
                // endpoint's registry key is restamped as a device comes and
                // goes, so it moves; its weakness is that a reboot restamps
                // every endpoint at once. Taking the later of the two keeps
                // each one's strength.
                const uint64_t regSeen = EndpointLastSeenUtc(lvl.id, lvl.isRender);
                if (regSeen > lvl.lastConnectedUtc) lvl.lastConnectedUtc = regSeen;
                if (!lvl.id.empty()) out.push_back(std::move(lvl));
                dev->Release();
            }
            coll->Release();
        }
        enumr->Release();
    } catch (...) {}

}

bool SetEndpointVolume(const std::wstring& endpointId, float vol01) {
    try {
        ComScope com;
        IAudioEndpointVolume* vol = OpenVolume(endpointId);
        if (!vol) return false;
        // Nullptr event context: this change is ours, and there is no callback
        // registered that would need to tell its own writes from someone else's.
        HRESULT hr = vol->SetMasterVolumeLevelScalar(std::clamp(vol01, 0.0f, 1.0f), nullptr);
        vol->Release();
        return SUCCEEDED(hr);
    } catch (...) { return false; }
}

bool SetEndpointMute(const std::wstring& endpointId, bool mute) {
    try {
        ComScope com;
        IAudioEndpointVolume* vol = OpenVolume(endpointId);
        if (!vol) return false;
        HRESULT hr = vol->SetMute(mute ? TRUE : FALSE, nullptr);
        vol->Release();
        return SUCCEEDED(hr);
    } catch (...) { return false; }
}

} // namespace mdxm
