#pragma once
// The order the device list is read in.
//
// Windows hands endpoints over in whatever order it enumerated them, which on
// this machine means five identically-named pairs of earbuds, their hands-free
// twins, and a row of virtual Sonar endpoints interleaved at random and
// reshuffled whenever something reconnects. A list you have to re-read from the
// top every time is not a mixer, so the order is defined here instead — and
// being pure, it is tested rather than eyeballed.
#include "device/endpoint_volume.h"
#include <vector>

namespace mdxm {

// Sorts in place, by these keys in order:
//   1. pinned devices first — the manual override, for the pair actually in use
//   2. outputs before inputs
//   3. the system default for its flow, first within that flow
//   4. connected before disconnected
//   5. the two endpoints of one physical device stay adjacent (ContainerId),
//      stereo above its hands-free twin
//   6. display name, then endpoint id, so the order never depends on what
//      Windows happened to enumerate first
//
// Keys 3 and 4 are decided per PHYSICAL device, not per endpoint: a headset
// whose stereo half is the default and whose hands-free half is not would
// otherwise be split apart by the very rule meant to bring it to the top.
void SortDevices(std::vector<DeviceLevel>& devices);

} // namespace mdxm
