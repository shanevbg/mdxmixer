#pragma once
// Turning SteelSeries Sonar off and on, the way its own Settings toggle does.
//
// Why mdxmixer needs this: when a Bluetooth headset disconnects SUDDENLY,
// Sonar hangs. Shane's recovery is "disable Sonar if it is running, restart
// the audio services, then re-enable Sonar" — three steps, the first of which
// had no method behind it but a mouse. This is that method.
//
// ── How it was found ────────────────────────────────────────────────────
//
// GG's local API is undocumented. /subApps reports every sub-app, and sonar
// is the only one carrying "toggleViaSettings": true — the flag behind the
// "Activate or deactivate Sonar" switch. The route itself is not discoverable
// from the API: everything but /subApps answers 404. It came out of the GG
// client's own bundle instead, which calls
//
//     post("/subApps/status", { name, isEnabled, additionalArguments })
//
// and that was confirmed against the live server on 2026-10-03: a bogus name
// answers 400, so the server validates and the shape is right; setting a
// sub-app to the state it is already in answers 200; and asking to enable
// Sonar while Sonar is already running answers 500, which is why SetEnabled
// checks the current state first rather than passing that 500 on as a
// failure.
//
// Control thread only — it does network I/O with short timeouts.
#include <string>

namespace mdxm {

enum class SonarState {
    Unknown,        // GG is not running, or did not answer
    Enabled,
    Disabled,
    NotInstalled,   // no coreProps.json: GG is not on this machine
};

const wchar_t* SonarStateName(SonarState s);

// What GG says right now. `err` carries the reason for Unknown.
SonarState QuerySonar(std::wstring* err);

// Flip it. Returns true if Sonar ENDS UP in the requested state, including
// when it was already there — asking for a state it already holds is not a
// failure, and for "enable" the server would answer 500 to it.
//
// Sonar's virtual endpoints disappear while it is disabled, so anything
// playing into them goes somewhere else; that is the point of the call, but
// it is not a quiet one.
bool SetSonarEnabled(bool enabled, std::wstring* err);

} // namespace mdxm
