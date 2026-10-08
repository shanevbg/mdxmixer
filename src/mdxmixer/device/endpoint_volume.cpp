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
#include <atomic>
#include <map>
#include <set>

namespace mdxm {

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// ── Timing ───────────────────────────────────────────────────────────────
//
// A lap timer. Eight QueryPerformanceCounter calls per endpoint is about four
// microseconds across the whole sweep, which is four thousandths of what the
// sweep was costing, so this is measured in production rather than behind a
// build switch -- the thing that went unnoticed for a month was the COST of
// this sweep, and an instrument nobody can read at runtime would not have
// caught it.
double QpcFreq() {
    static double f = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v.QuadPart ? (double)v.QuadPart : 1.0;
    }();
    return f;
}

struct Lap {
    static long long Tick() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
    long long t = Tick();
    // Milliseconds since the last call, and restart.
    double Ms() {
        const long long n = Tick();
        const double ms = (double)(n - t) * 1000.0 / QpcFreq();
        t = n;
        return ms;
    }
};

// ── The per-endpoint cache ───────────────────────────────────────────────
//
// See endpoint_volume.h for the contract and for the measurement that made it
// necessary. What lives here is what does not change between sweeps:
//
//   * the activated interfaces, which are the expensive part;
//   * the Windows friendly name and the registry facts, re-read on a slow
//     cadence rather than four times a second.
//
// What is NOT cached is every reading -- volume, mute and peak are asked for
// afresh on every sweep, because they are the entire point of sweeping.
struct CachedEndpoint {
    IAudioEndpointVolume* vol = nullptr;
    IAudioMeterInformation* meter = nullptr;
    std::wstring name;                 // PKEY_Device_FriendlyName
    std::wstring containerId;          // registry, keyed per endpoint id
    bool containerKnown = false;
    uint64_t lastSeenUtc = 0;
    unsigned slowStampMs = 0;          // when name/lastSeen were last read, 0 = never
    unsigned seenStamp = 0;            // sweep serial this entry last appeared in
};

// Names and last-seen dates are re-read this often instead of every sweep.
//
// Both can change while the program runs -- a device renamed in Windows Sound
// settings, a headset reconnecting and restamping its registry key -- and
// neither is a reading anyone watches move. Five seconds is below the point
// where a stale name could be acted on and is a twentieth of the work. The
// MMDevice API does signal renames through OnPropertyValueChanged, but
// DeviceWatcher deliberately drops those (they fire constantly), so this
// cadence is what keeps a rename from needing a restart.
constexpr unsigned kSlowRefreshMs = 5000;

// The Bluetooth battery table gets its own, much longer cadence.
//
// It is the single most expensive thing in a sweep -- 26 ms of an 86 ms cold
// sweep, measured with --sweepprof -- because it walks the PnP device tree,
// and it answers a question that changes once an hour: how charged is a
// headset. Re-reading it every five seconds alongside the endpoint list would
// put a 26 ms hitch on the UI thread twelve times a minute for nothing.
//
// A device CONNECTING is not on this clock: that is a notification, and it
// invalidates the whole cache, battery table included.
constexpr unsigned kBatteryRefreshMs = 30000;

void ReleaseCached(CachedEndpoint& e) {
    if (e.vol) { e.vol->Release(); e.vol = nullptr; }
    if (e.meter) { e.meter->Release(); e.meter = nullptr; }
}

// DELIBERATELY WITHOUT A DESTRUCTOR THAT RELEASES.
//
// The owning thread is an STA that main() tears down with CoUninitialize, and
// static destructors run after that; releasing an interface whose apartment is
// gone is a use-after-teardown. ReleaseEndpointCache() is the supported way
// out, and a process that exits without calling it leaks the interfaces to
// process teardown instead -- which costs nothing and cannot crash.
struct CachedDev {
    IMMDevice* dev = nullptr;
    bool isRender = false;
};

struct EndpointCacheState {
    std::map<std::wstring, CachedEndpoint> byId;
    DWORD owner = 0;                    // the thread that filled it
    std::atomic<bool> dirty{false};     // set from elsewhere, honoured by the owner
    unsigned serial = 0;                // sweep counter, for pruning

    // ── the slow set: everything in a sweep that is not a READING ─────────
    //
    // The enumerator, the endpoint list itself, which endpoints are the
    // system defaults, and the Bluetooth battery table. None of them changes
    // four times a second, and three of them were being rebuilt at that rate.
    //
    // Measured on 2026-10-05 with --sweepprof, after the interfaces were
    // cached and before this was: of a 56 ms sweep, ReadBluetoothInfo was 28
    // and the enumerator plus EnumAudioEndpoints was 12. Half the remaining
    // cost was re-asking Windows which devices exist and how charged they are,
    // on a UI thread, four times a second.
    //
    // Refreshed when a device notification says the set moved, and otherwise
    // every kSlowRefreshMs so a missed notification cannot freeze the list for
    // the life of the process.
    IMMDeviceEnumerator* enumr = nullptr;
    std::vector<CachedDev> devices;     // render first, then capture
    std::wstring defRender, defCapture;
    std::vector<BluetoothInfo> bt;
    unsigned setStampMs = 0;            // 0 = never gathered
    unsigned btStampMs = 0;             // the battery table's own, slower clock
};

void ReleaseAll(EndpointCacheState& s) {
    for (auto& kv : s.byId) ReleaseCached(kv.second);
    s.byId.clear();
    for (auto& d : s.devices) if (d.dev) d.dev->Release();
    s.devices.clear();
    if (s.enumr) { s.enumr->Release(); s.enumr = nullptr; }
    s.defRender.clear();
    s.defCapture.clear();
    s.bt.clear();
    s.setStampMs = 0;
    s.btStampMs = 0;
}

EndpointCacheState& CacheState() {
    static EndpointCacheState s;
    return s;
}

SweepProfile g_lastProfile;

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

namespace {
// RunGuarded carries one void*, and the sweep now needs three things.
struct SweepCtx {
    std::vector<DeviceLevel>* out;
    const std::set<std::wstring>* identityOnly;
    SweepProfile* prof;
};
} // namespace

std::vector<DeviceLevel> ListEndpointVolumes() {
    return ListEndpointVolumes(std::vector<std::wstring>{});
}

std::vector<DeviceLevel> ListEndpointVolumes(const std::vector<std::wstring>& identityOnlyIds) {
    std::vector<DeviceLevel> out;
    const std::set<std::wstring> skip(identityOnlyIds.begin(), identityOnlyIds.end());
    SweepProfile prof;
    Lap whole;
    SweepCtx ctx{ &out, &skip, &prof };
    if (!RunGuarded(&ListEndpointVolumesBody, &ctx))
        Log(1, L"endpoint sweep: the audio stack faulted; keeping %zu reading(s)", out.size());
    prof.totalMs = whole.Ms();
    g_lastProfile = prof;
    return out;
}

SweepProfile LastSweepProfile() { return g_lastProfile; }

void InvalidateEndpointCache() {
    EndpointCacheState& cs = CacheState();
    // The owner may release its own interfaces immediately; anyone else marks
    // the cache dirty and leaves the release to the thread that made them.
    // Releasing an apartment-bound pointer from the wrong thread is exactly
    // the class of fault the thread guard exists to avoid.
    if (cs.owner != 0 && cs.owner == GetCurrentThreadId()) {
        ReleaseAll(cs);
        cs.dirty.store(false, std::memory_order_relaxed);
        return;
    }
    cs.dirty.store(true, std::memory_order_relaxed);
}

void ReleaseEndpointCache() {
    EndpointCacheState& cs = CacheState();
    if (cs.owner != 0 && cs.owner != GetCurrentThreadId()) {
        // Not ours to release. Say so rather than doing it anyway: a wrong
        // release here is a crash at shutdown, which is the hardest kind to
        // read in a dump.
        Log(1, L"endpoint cache: release asked for on thread %lu, owned by %lu; leaving it",
            GetCurrentThreadId(), cs.owner);
        cs.dirty.store(true, std::memory_order_relaxed);
        return;
    }
    ReleaseAll(cs);
    cs.owner = 0;
    cs.dirty.store(false, std::memory_order_relaxed);
}

static void ListEndpointVolumesBody(void* ctx) {
    SweepCtx& sweep = *(SweepCtx*)ctx;
    std::vector<DeviceLevel>& out = *sweep.out;
    SweepProfile& prof = *sweep.prof;
    try {
        ComScope com;
        Lap lap;

        // The cache belongs to the first thread that sweeps, which in the
        // running program is the UI thread -- an STA for the life of the
        // process (main.cpp). A sweep from anywhere else (the --levels CLI,
        // anything the engine ever does) runs uncached against a local entry,
        // so no apartment-bound pointer ever crosses a thread.
        EndpointCacheState local;   // a sweep from another thread caches nothing
        const DWORD me = GetCurrentThreadId();
        if (CacheState().owner == 0) CacheState().owner = me;
        const bool useCache = (CacheState().owner == me);
        EndpointCacheState& S = useCache ? CacheState() : local;
        if (useCache && S.dirty.exchange(false, std::memory_order_relaxed))
            ReleaseAll(S);
        const unsigned serial = ++S.serial;
        const unsigned nowMs = (unsigned)GetTickCount();

        // Gather the slow set when it is stale, missing, or has just been
        // invalidated. See EndpointCacheState.
        const bool gatherSet = (S.setStampMs == 0) ||
                               (nowMs - S.setStampMs) >= kSlowRefreshMs ||
                               S.devices.empty();
        if (gatherSet) {
            // Battery and last-connected live on a Bluetooth PnP node, joined
            // to the endpoint by ContainerId. On its own, much slower clock:
            // see kBatteryRefreshMs.
            if (S.btStampMs == 0 || (nowMs - S.btStampMs) >= kBatteryRefreshMs ||
                S.bt.empty()) {
                S.bt = ReadBluetoothInfo();
                S.btStampMs = nowMs ? nowMs : 1;
                prof.btInfoMs += lap.Ms();
            }

            for (auto& d : S.devices) if (d.dev) d.dev->Release();
            S.devices.clear();
            if (!S.enumr &&
                (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                         __uuidof(IMMDeviceEnumerator), (void**)&S.enumr)) ||
                 !S.enumr)) {
                S.enumr = nullptr;
                return;
            }
            S.defRender = DefaultIdFor(S.enumr, eRender);
            S.defCapture = DefaultIdFor(S.enumr, eCapture);

            for (EDataFlow flow : { eRender, eCapture }) {
                IMMDeviceCollection* coll = nullptr;
                // ACTIVE **and** UNPLUGGED, deliberately.
                //
                // Listing only the active ones made a disconnected headset
                // invisible, and five identical pairs of earbuds are
                // disconnected most of the time. That is why "I can't tell
                // when the last time a bluetooth headset was connected" and "I
                // can't rename headsets so all the headsets have random
                // numbers" are the same bug: a device that is not in the list
                // cannot show its last-seen time and cannot be given a name.
                // MDropDX12's failover list shows every device Windows knows
                // about for exactly this reason -- "the useful entries are
                // exactly the ones switched off".
                //
                // DISABLED and NOTPRESENT are left out: those are devices the
                // user has turned off in Windows or that have been removed,
                // not ones waiting to be reconnected. That half of "if they
                // can't see a device or it's disabled can you skip that" was
                // always true; the other half is identityOnly below.
                const DWORD kStates = DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED;
                if (FAILED(S.enumr->EnumAudioEndpoints(flow, kStates, &coll)) || !coll)
                    continue;
                UINT count = 0;
                coll->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* dev = nullptr;
                    if (SUCCEEDED(coll->Item(i, &dev)) && dev)
                        S.devices.push_back({ dev, flow == eRender });
                }
                coll->Release();
            }
            S.setStampMs = nowMs ? nowMs : 1;
            prof.enumMs += lap.Ms();
        }

        const std::vector<BluetoothInfo>& bt = S.bt;
        const std::wstring& defRender = S.defRender;
        const std::wstring& defCapture = S.defCapture;

        {
            for (const CachedDev& cd : S.devices) {
                IMMDevice* dev = cd.dev;
                if (!dev) continue;
                DeviceLevel lvl;
                lvl.isRender = cd.isRender;
                LPWSTR id = nullptr;
                if (SUCCEEDED(dev->GetId(&id)) && id) { lvl.id = id; CoTaskMemFree(id); }
                ++prof.endpoints;

                CachedEndpoint& ce = S.byId[lvl.id];
                if (ce.seenStamp != 0) ++prof.cacheHits; else ++prof.cacheMisses;
                ce.seenStamp = serial;
                const bool slow = (ce.slowStampMs == 0) ||
                                  (nowMs - ce.slowStampMs) >= kSlowRefreshMs ||
                                  ce.name.empty();

                if (slow) {
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                            ce.name = pv.pwszVal;
                        PropVariantClear(&pv);
                        props->Release();
                    }
                    prof.nameMs += lap.Ms();
                }
                lvl.name = ce.name;

                // Is it actually there? An unplugged endpoint cannot be
                // activated for volume or metering, and asking is both
                // pointless and a fault risk.
                //
                // Asked EVERY sweep, never cached: this is the one fact about
                // an endpoint that is expected to change underneath us, and it
                // is what decides whether the rest of the loop may touch the
                // device at all.
                DWORD devState = DEVICE_STATE_ACTIVE;
                dev->GetState(&devState);
                lvl.active = (devState == DEVICE_STATE_ACTIVE);
                if (lvl.active) ++prof.active;

                // Hidden: identity only, no activation and no reads. The user
                // took this row off the surface, so its level and its peak are
                // being collected for nobody -- and with Voicemeeter and
                // VB-Matrix installed, 24 of this machine's 53 endpoints are
                // virtual rows nobody looks at.
                //
                // Cached interfaces are dropped rather than parked: holding a
                // meter open on an endpoint nobody is watching asks the audio
                // service to keep doing work too. Un-hiding one costs a single
                // activation on the next sweep.
                const bool identityOnly =
                    sweep.identityOnly && sweep.identityOnly->count(lvl.id) != 0;
                if (identityOnly) {
                    ++prof.identityOnly;
                    if (ce.vol || ce.meter) ReleaseCached(ce);
                }

                // Only an ACTIVE endpoint has a volume or a meter. An
                // unplugged one is listed for its identity alone: its name,
                // its battery and when it was last seen.
                if (lvl.active && !identityOnly) {
                    // A cached interface that refuses a read is pointing at an
                    // endpoint that has gone away without a notification --
                    // which on this machine happens every time a headset drops
                    // mid-sweep. Drop it and activate once more, so the
                    // reading is right in THIS sweep rather than the next one.
                    for (int attempt = 0; attempt < 2; ++attempt) {
                        if (!ce.vol) {
                            IAudioEndpointVolume* v = nullptr;
                            if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                                        nullptr, (void**)&v)) && v)
                                ce.vol = v;
                            prof.activateMs += lap.Ms();
                        }
                        if (!ce.vol) break;
                        float scalar = 0.0f;
                        BOOL muted = FALSE;
                        const HRESULT hv = ce.vol->GetMasterVolumeLevelScalar(&scalar);
                        const HRESULT hm = ce.vol->GetMute(&muted);
                        prof.readMs += lap.Ms();
                        if (SUCCEEDED(hv)) lvl.vol = scalar;
                        if (SUCCEEDED(hm)) lvl.mute = (muted != FALSE);
                        lvl.volumeKnown = true;
                        if (SUCCEEDED(hv) && SUCCEEDED(hm)) break;
                        // Failed: throw THIS interface away and go round once.
                        // Only this one -- a working meter on the same device
                        // has done nothing wrong.
                        lvl.volumeKnown = false;
                        ce.vol->Release();
                        ce.vol = nullptr;
                        ++prof.cacheDropped;
                    }
                    // What is actually flowing, alongside what the level is
                    // set to: the reading that says whether audio is reaching
                    // the device at all.
                    //
                    // lvl.peak is assigned ONLY on success, so a meter that
                    // will not activate or will not answer leaves kPeakUnknown
                    // standing rather than reporting silence. An endpoint that
                    // never reaches this branch at all -- an UNPLUGGED one --
                    // keeps it for the same reason.
                    //
                    // This reads honestly through a Sonar VIRTUAL endpoint,
                    // which is not obvious and was measured on 2026-10-04 with
                    // RarmaRadio playing to Sonar Aux: Aux 0.4457 and Stream
                    // 0.3050/0.3608 across successive reads, both varying with
                    // the music, while Media and Gaming -- which have nothing
                    // routed to them -- read a flat 0.0000. So the Sonar write
                    // path lies (SetMasterVolumeLevelScalar returns success and
                    // holds at 1.0) and the READ path does not. That asymmetry
                    // is what lets a Sonar channel carry a real peak.
                    for (int attempt = 0; attempt < 2; ++attempt) {
                        if (!ce.meter) {
                            IAudioMeterInformation* m = nullptr;
                            if (SUCCEEDED(dev->Activate(__uuidof(IAudioMeterInformation),
                                                        CLSCTX_ALL, nullptr, (void**)&m)) && m)
                                ce.meter = m;
                            prof.activateMs += lap.Ms();
                        }
                        if (!ce.meter) break;
                        float peak = 0.0f;
                        const HRESULT hp = ce.meter->GetPeakValue(&peak);
                        prof.readMs += lap.Ms();
                        if (SUCCEEDED(hp)) {
                            lvl.peak = peak;
                            // The UNHELD reading, kept beside the one the hold
                            // will overwrite. A surface that wants a meter to
                            // fall at its own rate needs the raw number; the
                            // wire keeps the 1.5 s hold (peak_hold.h).
                            lvl.peakNow = peak;
                            break;
                        }
                        ce.meter->Release();
                        ce.meter = nullptr;
                        ++prof.cacheDropped;
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
                // ContainerId is minted per PAIRING and read out of the
                // registry, so for a given endpoint id it never changes. Read
                // once and kept for as long as the endpoint is in the list.
                if (!ce.containerKnown) {
                    ce.containerId = EndpointContainerId(lvl.id);
                    ce.containerKnown = true;
                    prof.containerMs += lap.Ms();
                }
                std::wstring container = ce.containerId;
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
                //
                // Another registry read, on the same slow cadence as the name:
                // it moves as a device comes and goes, but it is displayed as a
                // DATE, so re-reading it four times a second buys nothing.
                if (slow) {
                    ce.lastSeenUtc = EndpointLastSeenUtc(lvl.id, lvl.isRender);
                    ce.slowStampMs = nowMs ? nowMs : 1;   // 0 means "never read"
                    prof.lastSeenMs += lap.Ms();
                }
                if (ce.lastSeenUtc > lvl.lastConnectedUtc) lvl.lastConnectedUtc = ce.lastSeenUtc;
                if (!lvl.id.empty()) out.push_back(std::move(lvl));
                // dev is owned by S.devices, not by this iteration.
            }
        }

        // Entries for endpoints that stopped appearing. Dropped rather than
        // kept, so the map does not grow for the life of a process on a
        // machine that re-pairs headsets daily and mints an endpoint id each
        // time -- and so a dead endpoint's interfaces are released rather than
        // held open against a device that is gone.
        for (auto it = S.byId.begin(); it != S.byId.end(); ) {
            if (it->second.seenStamp != serial) {
                ReleaseCached(it->second);
                it = S.byId.erase(it);
            } else {
                ++it;
            }
        }
        // A sweep that does not own the cache owns everything it just made.
        if (!useCache) ReleaseAll(S);
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
