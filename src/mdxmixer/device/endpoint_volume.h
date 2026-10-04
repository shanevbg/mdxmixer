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
    // The level Windows is PUTTING on this endpoint right now, 0..1.
    //
    // Not the volume setting -- what is actually flowing. It is the one
    // reading that separates "nothing is being sent here" from "something is
    // being sent and you cannot hear it", which on 2026-10-03 was the
    // difference between a broken mixer and a flapping Bluetooth link, and
    // nothing on the machine showed it.
    float peak = 0.0f;
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

bool SetEndpointVolume(const std::wstring& endpointId, float vol01);
bool SetEndpointMute(const std::wstring& endpointId, bool mute);

} // namespace mdxm
