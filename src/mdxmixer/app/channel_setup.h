#pragma once
// Turning an audio endpoint into a mixer channel.
//
// A channel is what the Personal and Streaming faders act on: one source, mixed
// into the personal mix and the streaming mix at independent levels. The source
// is usually a RENDER endpoint, tapped by WASAPI loopback — that is how a
// channel mirrors "SteelSeries Sonar - Gaming" without a virtual cable between
// them, and it is why Sonar's four outputs can be adopted with one click.
//
// Pure string work, so the parts that are easy to get wrong — a stable channel
// id, and refusing the binding that would howl — are tested.
#include "config/config.h"
#include <string>
#include <vector>

namespace mdxm {

// A lowercase slug of the name, uniqued against the channels already there.
// Ids end up in the config, in the IPC protocol and in MDropDX12's requests, so
// they stay ASCII and stable rather than following a rename.
std::wstring MakeChannelId(const std::vector<ChannelConfig>& existing,
                           const std::wstring& name);

// "SteelSeries Sonar - Gaming (SteelSeries Sonar Virtual Audio Device)" ->
// "Gaming". Empty for anything that is not one of Sonar's virtual outputs, so
// the one-click setup cannot adopt an unrelated device.
std::wstring SonarChannelName(const std::wstring& endpointName);

// Tapping the endpoint the mix is being played to feeds the mix back into
// itself. On this machine that is the easy mistake to make: the system default
// output IS "SteelSeries Sonar - Gaming", so adopting it as a channel while the
// personal output follows the default would howl. Loud. Refused, not warned.
bool WouldFeedBack(const std::wstring& sourceEndpointId,
                   const std::wstring& personalRenderId);

} // namespace mdxm
