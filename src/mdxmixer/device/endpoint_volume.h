#pragma once
// The Windows volume of each audio endpoint — what the system tray slider and
// the Sound control panel move — read and written through IAudioEndpointVolume.
//
// This is a separate axis from the mixer's own channel faders: a channel fader
// scales what mdxmixer sums, an endpoint volume is the device's own level. A
// mixer that cannot reach the second one leaves the user going to the Windows
// slider anyway, which is the thing this program exists to replace.
//
// Control thread only. Every call activates COM interfaces, so none of this may
// run on an audio thread or inside a device notification (MDropDX12 fj#401).
#include "dsp/peak_hold.h"   // kPeakUnknown — no Windows types, see mixer_control.h
#include <string>
#include <vector>

namespace mdxm {

struct DeviceLevel {
    std::wstring id, name;    // name is what Windows calls it
    std::wstring displayName; // our alias when one is set, else name
    // Per PHYSICAL device, and stable when the endpoint id is not — the same
    // earbuds get a new id with no re-pairing, depending which side connects
    // first. Empty for endpoints with no container (most non-Bluetooth ones).
    std::wstring containerId;
    bool isRender = false;
    bool isDefault = false;   // the system default for its flow
    float vol = 0.0f;         // 0..1, the endpoint's own scalar volume
    bool mute = false;
    // Identity, for telling five identically-named headsets apart.
    int battery = -1;             // percent, -1 when the device reports none
    // PAIRED, not connected. The Bluetooth device node answers this, and it
    // says yes for a headset that is paired and switched off in a drawer --
    // every one of Shane's six dormant pairings reports present.
    //
    // The comment here used to read "physically connected right now", and
    // believing it is how a "Show disconnected" box came to read (0) with six
    // disconnected headsets pinned above everything else. Anything asking
    // "can I use this device" wants `active`; this is only good for telling a
    // device Windows still knows about from one it has forgotten.
    bool present = true;
    // The endpoint is ACTIVE, i.e. Windows can open a stream on it. False for
    // one that is listed but unplugged -- a headset that is paired and
    // switched off. Such an endpoint has no volume, no meter and no session,
    // and is listed for its identity: a device that is not in the list can be
    // neither renamed nor dated, which is most of the five XM5s most of the
    // time.
    bool active = true;
    bool volumeKnown = false;     // vol/mute were actually read
    // Sorted to the top like a pinned device, but not BY the user — this is
    // membership of a failover allowlist, which mdxmixer holds at the top
    // while the device is connected (MDropDX12's pinFailoverDevices). Kept
    // apart from `pinned` so the "[pinned]" tag and the Unpin menu item go on
    // saying what the user did, and only that.
    bool autoPinned = false;
    uint64_t lastConnectedUtc = 0;
    // The headset's own Bluetooth address, lower-cased hex. Empty for
    // anything not on Bluetooth.
    //
    // The most stable anchor there is for a name: endpoint ids and
    // ContainerIds are both minted per PAIRING, so changing adapters re-pairs
    // everything and every alias keyed on them stops matching at once -- "I
    // might have to change adapters at some point and then have to redo all
    // the names even though the actual id of the device is the same".
    std::wstring btAddress;
    bool isHandsFree = false;     // the mono narrowband twin of a headset
    // The level Windows is PUTTING on this endpoint right now, 0..1, or
    // kPeakUnknown (-1) when it cannot be read.
    //
    // Not the volume setting -- what is actually flowing. It is the one
    // reading that separates "nothing is being sent here" from "something is
    // being sent and you cannot hear it", which on 2026-10-03 was the
    // difference between a broken mixer and a flapping Bluetooth link, and
    // nothing on the machine showed it.
    //
    // -1 AND 0.0 ARE DIFFERENT ANSWERS, and the difference is the point.
    // An endpoint Windows has parked as UNPLUGGED has no meter to activate,
    // and one whose IAudioMeterInformation refuses has none either; both must
    // say "cannot know" rather than "silent", because silent is a claim and a
    // client sorting by what is making sound would rank them as confidently
    // quiet. Same rule this struct already applies to `battery`.
    float peak = kPeakUnknown;
    // The same reading with NO hold on it: what the meter said at the instant
    // of the sweep.
    //
    // `peak` above is overwritten by the controller with a 1.5 s peak-hold,
    // which is right for the wire -- a spike has to survive mdxmixer to
    // MDropDX12 to a phone. It is wrong for a meter somebody is watching, which
    // should fall promptly and smoothly (dsp/meter_ballistics.h). Both numbers
    // cost one read, so both are published and each surface picks.
    float peakNow = kPeakUnknown;
    // How the user filed it: see DeviceView in device_identity.h. Filled in by
    // the controller from the saved list, not read from Windows.
    bool hidden = false;
    bool pinned = false;
};

// Active AND unplugged endpoints, render first then capture. An unplugged one
// carries its name, container, battery and last-seen but no level: see
// DeviceLevel::active. Never throws; a device that refuses to report is
// skipped.
std::vector<DeviceLevel> ListEndpointVolumes();

// The same sweep, told which endpoints need their IDENTITY ONLY — no volume,
// no meter, so no COM activation and no reads.
//
// An endpoint the user has hidden in the mixer has no row to draw, so its
// level and its peak are collected for nobody. On this machine that is not a
// rounding error: installing Voicemeeter and VB-Matrix took the endpoint count
// to 53, about 45 of them ACTIVE, and sixteen of those are VBMatrix In/Out
// rows that exist only because the driver creates eight of each.
//
// Hidden is the APP's idea, not Windows' (DeviceView in device_identity.h), and
// resolving it needs the name and container this sweep produces — so the caller
// passes the ids that resolved to hidden LAST time. A newly hidden device is
// swept in full once more and then goes quiet, which is the right way round:
// the cost of being one sweep late is a stale reading nobody is looking at.
std::vector<DeviceLevel> ListEndpointVolumes(const std::vector<std::wstring>& identityOnlyIds);

// ── The per-endpoint COM cache ───────────────────────────────────────────────
//
// IAudioEndpointVolume and IAudioMeterInformation are activated once per
// endpoint and kept. "com activations are very costly" — and this sweep ran
// four times a second on the UI thread, rebuilding about ninety of them each
// time, which measured 98-161 ms out of every 250 ms tick.
//
// The cached interfaces outlive the IMMDevice they came from: an activated
// endpoint object holds its own reference to the device and does not depend on
// the enumerator's wrapper. What they do NOT outlive is the endpoint itself, so:
//
//   * InvalidateEndpointCache() drops everything, and is what a device
//     arrival/removal/state change must call. It takes no COM calls and no
//     locks of its own beyond the cache's, so it is safe from the debounced
//     device-change handler — but NOT from inside an IMMNotificationClient
//     callback, which may only signal (MDropDX12 fj#401).
//   * a read that fails invalidates its own entry and re-activates once, which
//     covers an endpoint that died between sweeps without a notification.
//
// Single-threaded by design: the cache belongs to the thread that filled it
// (the UI thread, an STA for the life of the process). A call from any other
// thread bypasses it entirely rather than hand an apartment-bound pointer
// across threads, so the CLI paths and the engine are unaffected.
void InvalidateEndpointCache();

// Release the cached interfaces now. MUST be called while COM is still
// initialised on the owning thread — releasing after CoUninitialize is a
// use-after-teardown, which is why this is explicit rather than a static
// destructor.
void ReleaseEndpointCache();

// Where a sweep spent its time. Filled in by every ListEndpointVolumes call;
// `--sweepprof` prints it, and the first sweep of a process is the honest
// "before" measurement because nothing is cached yet.
struct SweepProfile {
    double totalMs = 0;
    double btInfoMs = 0;         // ReadBluetoothInfo, once per sweep
    double enumMs = 0;           // enumerator, defaults, EnumAudioEndpoints
    double nameMs = 0;           // OpenPropertyStore + FriendlyName
    double activateMs = 0;       // Activate(IAudioEndpointVolume/MeterInformation)
    double readMs = 0;           // GetMasterVolumeLevelScalar/GetMute/GetPeakValue
    double containerMs = 0;      // EndpointContainerId   (registry)
    double lastSeenMs = 0;       // EndpointLastSeenUtc   (registry)
    int endpoints = 0;
    int active = 0;
    int identityOnly = 0;        // skipped because the user hid them
    int cacheHits = 0;           // endpoints whose interfaces were reused
    int cacheMisses = 0;         // endpoints that had to be activated
    int cacheDropped = 0;        // entries thrown out after a failed read
};
SweepProfile LastSweepProfile();

bool SetEndpointVolume(const std::wstring& endpointId, float vol01);
bool SetEndpointMute(const std::wstring& endpointId, bool mute);

} // namespace mdxm
