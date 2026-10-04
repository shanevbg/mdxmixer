#pragma once
// What the Bluetooth stack knows about a physical audio device: battery level
// and when it last connected.
//
// Neither lives anywhere near the audio endpoint. Windows keeps them as PnP
// device properties on a THIRD node:
//
//   SWD\MMDEVAPI\{0.0.0.00000000}.{guid}         the audio endpoint
//   BTHENUM\DEV_<mac>\...                        the Bluetooth device
//   BTHENUM\{0000111E-...}\...&<mac>_C00000000   "<name> Hands-Free AG"  <- here
//
// The endpoint and that node are joined by ContainerId, which Windows assigns
// per physical device and both carry — so no MAC address has to be parsed out
// of an instance path. Only devices exposing the hands-free profile report at
// all, which in practice means headsets and earbuds: exactly what a failover
// list is made of.
//
// Control thread only.
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm {

struct BluetoothInfo {
    std::wstring containerId;
    int battery = -1;              // percent, -1 when the node reports none
    uint64_t lastConnectedRaw = 0; // FILETIME exactly as the property stores it
    bool present = false;          // attached right now, vs merely remembered
    // The headset's own Bluetooth address, lower-cased hex. Empty for
    // anything that is not a Bluetooth device.
    //
    // The one identifier here that survives a change of ADAPTER: endpoint ids
    // and ContainerIds are both minted per pairing, so a new dongle re-pairs
    // everything and every alias keyed on them stops matching at once.
    std::wstring btAddress;
};

// Every Bluetooth container the machine knows, keyed by ContainerId.
std::vector<BluetoothInfo> ReadBluetoothInfo();

// The ContainerId of one audio endpoint, for joining against the above.
std::wstring EndpointContainerId(const std::wstring& endpointId);

// The last-write time of an endpoint's own registry key, in UTC FILETIME.
//
// PORTED from MDropDX12's mixer_device_watch.cpp. MMDevice exposes no "last
// connected" property, but Windows keeps a key per endpoint under
// MMDevices\Audio and rewrites it as the device's state changes, so its
// timestamp moves when a device comes and goes.
//
// This exists because the Bluetooth property is not enough on its own.
// DEVPKEY_Bluetooth_LastConnectedTime is per-device and months deep, but
// Windows does not reliably refresh it: measured on this machine, headsets
// used the same day still reported 27 and 28 September, which is worse than
// useless for picking one. The registry stamp is an approximation -- a reboot
// restamps every endpoint at once -- but it is the one that moves.
//
// Returns 0 when there is no such key.
uint64_t EndpointLastSeenUtc(const std::wstring& endpointId, bool isRender);

} // namespace mdxm
