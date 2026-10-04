#pragma once
// Endpoint enumeration and the pure helpers around it.
// Every device binding stores id AND name; MatchBinding is id-first with an
// exact (case-insensitive) name fallback — a re-paired Bluetooth device returns
// under a NEW id with the SAME name (the MDropDX12 lesson).
#include "config/config.h"   // DeviceRef
#include <windows.h>
#include <mmreg.h>           // WAVEFORMATEX (WIN32_LEAN_AND_MEAN drops it from windows.h)
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mdxm {

struct EndpointInfo { std::wstring id, name; bool isRender = false; bool isActive = false;
                      uint32_t rate = 0; };   // shared-mode mix rate, 0 when unknown

// Both flows, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED. COM failures skip
// that device; never throws. Requires COM initialized on the calling thread.
std::vector<EndpointInfo> EnumerateEndpoints();

// Pure. Id match wins; else exact case-insensitive name match (active endpoints
// preferred when several share a name); else nullptr.
const EndpointInfo* MatchBinding(const std::vector<EndpointInfo>& eps, const DeviceRef& want);

// The system default render endpoint's id ("" on failure). Control thread only.
std::wstring DefaultRenderEndpointId();

// The endpoint's shared-mode mix rate (0 when it cannot be read). Control thread
// only — this activates an IAudioClient, so never call it from an audio thread.
// The rollout requires every cable at 48 kHz; this is how that gets checked.
uint32_t EndpointMixRate(const std::wstring& endpointId);

struct StreamFormat { uint32_t rate = 0; uint16_t channels = 0;
                      enum class Sample { F32, I16, Unsupported } sample = Sample::Unsupported; };
// Pure over the struct bytes: float32 and 16-bit PCM (plain or EXTENSIBLE) parse;
// anything else is Unsupported and the caller refuses to start rather than write
// garbage bytes (passthrough-monitor lessons).
StreamFormat ParseMixFormat(const WAVEFORMATEX* wfx);

} // namespace mdxm
