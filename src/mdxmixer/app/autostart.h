#pragma once
// Autostart at login: HKCU Run key, value "mdxmixer", quoted exe path.
// Written ONLY when the user ticks the toggle — never silently (spec).

namespace mdxm {

bool SetAutostart(bool on);
bool GetAutostart();

} // namespace mdxm
