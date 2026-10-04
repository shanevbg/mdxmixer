#pragma once
// Telling one device from another.
//
// Windows names five identical pairs of earbuds WF-1000XM5-1 through -5, will
// not let them be renamed persistently, and hands them a new endpoint id every
// time they re-pair. So the name is ours to keep, and it has to survive the id
// changing underneath it.
//
// Everything here is pure string and time handling, so the awkward parts —
// re-association after a re-pair, and the local-vs-UTC trap in the Bluetooth
// last-connected property — are covered by tests rather than by hoping.
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm {

// Our name for a device, with everything needed to recognise it again.
//
// The endpoint id is NOT a stable handle: the same earbuds get a new one with
// no re-pairing at all, depending on which side connects first. ContainerId is
// assigned per PHYSICAL device and survives that, so it is the real anchor.
// The Windows name is the last resort, for endpoints that have no container.
struct DeviceAlias {
    std::wstring id;
    // The headset's own Bluetooth address, and the STRONGEST anchor of the
    // four: it belongs to the device, not to the pairing.
    //
    // ContainerId survives a re-pair of the same adapter but not a change OF
    // adapter -- a new dongle mints a new container for every headset, and
    // eighteen careful names stop matching in one afternoon. The address does
    // not move, so a name keyed on it survives that, and several pairings of
    // one physical headset collapse onto a single name instead of needing one
    // each.
    std::wstring btAddress;
    std::wstring containerId;
    std::wstring windowsName;
    std::wstring alias;
    // How the device is filed in the mixer list. Kept on the same entry as the
    // name because they are recognised by the same anchor: pinning a headset
    // has to survive the re-pair that changes its endpoint id, exactly as its
    // name does.
    bool hidden = false;   // left out of the list until "show hidden"
    bool pinned = false;   // sorted above everything else
};

// The filing flags alone, for a device we may never have been told about.
struct DeviceView { bool hidden = false; bool pinned = false; };

// Windows hands every device with no real container the same placeholder GUID,
// {00000000-0000-0000-FFFF-FFFFFFFFFFFF}. Six SteelSeries Sonar virtual
// endpoints carrying it are not one physical device: treating it as an anchor
// files them all together and, worse, makes renaming one of them rename all
// six. Treated as no container at all.
bool IsNullContainer(const std::wstring& containerId);

// Sets or clears the name (empty clears). Matches an existing entry the way
// DisplayName does, so renaming a device that has moved id updates that entry
// rather than adding a second one. Clearing the name does not discard a device
// that is still pinned or hidden — those outlive the name.
void SetAlias(std::vector<DeviceAlias>& aliases, const std::wstring& id,
              const std::wstring& containerId, const std::wstring& windowsName,
              const std::wstring& alias, const std::wstring& btAddress = {});

// The alias if we have one, else the Windows name.
//
// Match order is Bluetooth address, then container, then endpoint id, then
// Windows name — strongest anchor first, because each one below survives less
// than the one above it: the address outlives a change of adapter, the
// container outlives a re-pair, the id outlives nothing, and the name is a
// last resort because two devices CAN carry the same one. On a match by
// anything other than id, the entry re-homes onto the current id.
std::wstring DisplayName(std::vector<DeviceAlias>& aliases, const std::wstring& id,
                         const std::wstring& containerId, const std::wstring& windowsName,
                         const std::wstring& btAddress = {});

// The alias alone, read-only, for a device that is NOT present.
//
// DisplayName re-homes the entry it matches onto the current endpoint id,
// which is right when a real device is in front of it and wrong when there is
// not one: a failover allowlist lists devices that may be switched off for
// months, and re-homing those onto an id that no longer exists would corrupt
// the anchor that still works.
//
// Returns the Windows name when nothing matches, so a caller can print the
// result either way.
std::wstring AliasFor(const std::vector<DeviceAlias>& aliases, const std::wstring& id,
                      const std::wstring& containerId, const std::wstring& windowsName,
                      const std::wstring& btAddress = {});

// Pin and hide, recognised by the same anchor as the name. Default flags on
// both sides — a device nobody has filed is visible and unpinned — and setting
// both back to default drops an entry that carries no name either.
DeviceView LookupView(std::vector<DeviceAlias>& aliases, const std::wstring& id,
                      const std::wstring& containerId, const std::wstring& windowsName,
                      const std::wstring& btAddress = {});
void SetView(std::vector<DeviceAlias>& aliases, const std::wstring& id,
             const std::wstring& containerId, const std::wstring& windowsName,
             DeviceView view, const std::wstring& btAddress = {});

// The part of a Windows device name that identifies it, for a column narrow
// enough to leave room for the rest of the row.
//
// "SteelSeries Sonar - Chat (SteelSeries Sonar Virtual Audio Device)" is 63
// characters of which 4 distinguish it, and six of them differ only in those
// 4 -- so in any readable column they are identical. MDropDX12 trims the same
// way and keeps the whole name on a tooltip; this returns the short form and
// the caller supplies the tooltip.
//
//   SteelSeries Sonar - Chat (...Virtual Audio Device)  ->  [SS] Chat
//   CABLE Input (VB-Audio Virtual Cable)                ->  [VB] CABLE Input
//   Headphones (11- WF-1000XM5)                         ->  unchanged
//
// A headset keeps its full name: there the generic word is the PREFIX and the
// identifying part is inside the brackets, so trimming the brackets would
// throw away the only thing telling five of them apart.
std::wstring ShortDeviceName(const std::wstring& windowsName);

// An endpoint published by a mixer PROVIDER rather than by hardware.
//
// PORTED, rule and reasoning, from MDropDX12: "They truncate to exactly the
// provider channel names they shadow, sit at 1.000 and ignore a write, so the
// row that LOOKS like the one you want is a decoy while the real channel sits
// below the fold." Now that mdxmixer carries Sonar's own channels, its six
// virtual endpoints are exactly that -- an [SS] Aux row at 100% that does
// nothing, directly above an Aux channel at 4% that does.
//
// Matched by the provider's own name prefix, NOT by a null ContainerId. mdx12
// learned that the hard way (its forgejo#51): NVIDIA Broadcast and the GS
// Wavetable Synth carry the same placeholder container and their volumes
// WORK, so a container test hides real devices along with the decoys.
bool IsProviderOwnedEndpoint(const std::wstring& windowsName);

// Empty when there is no reading. A real zero is worth showing, so -1 rather
// than 0 means "unknown".
std::wstring FormatBattery(int percent);

// Windows publishes one headset twice: "Headphones (X)" is A2DP stereo, and
// "Headset (X ... Hands-Free)" turns the microphone on and drops to mono
// narrowband. Worth spelling out in any list that offers both.
bool IsHandsFreeName(const std::wstring& windowsName);

// DEVPKEY_Bluetooth_LastConnectedTime is stored in LOCAL time. Converting it
// with FileTimeToLocalFileTime subtracts the offset a second time, which is
// how two headsets once appeared exactly one timezone apart. Returns 0 for 0.
uint64_t LocalFileTimeToUtc(uint64_t localFileTime);

// "23 Sep 15:30" — the hour is kept, not just the day: several of the XM5
// pairings were last seen on the same date and are told apart only by time.
// Empty when the stack has no record.
//
// `present` wins outright. Windows does not reliably refresh
// LastConnectedTime: an XM6 measured on this machine sat at present=1 with a
// full battery while still reporting a stamp from the previous year. A device
// that is connected now was, trivially, seen now.
std::wstring FormatLastSeen(uint64_t utcFileTime, bool present);

} // namespace mdxm
