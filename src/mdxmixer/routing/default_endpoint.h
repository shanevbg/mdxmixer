#pragma once
// Moving the Windows default output.
//
// PORTED from MDropDX12 (mixer_provider_endpoint.cpp at 29c4806d), where the
// vtable layout and the three-role caveat were established. mdxmixer needs it
// for the failure measured on this machine on 2026-10-03: the default output
// was a SteelSeries Sonar virtual endpoint whose engine had survived two
// AUDIODG kills and was mixing nothing, so every sound went into it and none
// came out — with services Running, every endpoint Active, and no error
// anywhere. Getting out of that is one write, and until now mdxmixer could not
// make it.
//
// Control thread only: it stands up COM and activates interfaces, so it must
// never run on an audio thread or inside a device notification (fj#401).
#include <string>

namespace mdxm {

// Point Windows' default render endpoint at this device.
//
// Writes all three roles — Console, Multimedia and Communications. An app that
// asks for a specific role would otherwise keep the old device and the switch
// would look like it half-worked.
//
// That has a cost worth stating plainly, measured in mdx12 by moving a media
// player between Sonar channels while asserting nothing else changed: Windows
// keeps the three roles separately, a user may well have them pointed at
// different devices (Sonar routes apps that way), and writing all three
// FLATTENS that arrangement. The previous per-role assignment is not recorded
// anywhere, so it cannot be put back from here. There is no such thing as a
// no-op default-endpoint write — never call this to "check whether it works".
bool SetDefaultRenderEndpoint(const std::wstring& endpointId, std::wstring* err);

} // namespace mdxm
