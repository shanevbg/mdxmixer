#pragma once
// The spec's one-file isolation for the undocumented IAudioPolicyConfigFactory —
// the API Windows Settings' "App volume and device preferences" page itself uses,
// shipped on for years by EarTrumpet and SoundSwitch. A Windows update can move
// it; the failure mode is "assignment stops working", surfaced in the UI — never
// a crash, never broken audio for already-routed apps.
//
// The underlying API is keyed by PROCESS ID (Windows persists per-app from the
// pid); mapping an exe path to its live pids is the app layer's job, via
// EnumerateSessions.
#include <string>

namespace mdxm {

// MMDevice endpoint id <-> the \\?\SWD#MMDEVAPI#...#{DEVINTERFACE_AUDIO_RENDER}
// moniker this API speaks. Pure string transforms (render flow only in v1).
std::wstring MakePolicyDeviceId(const std::wstring& mmDeviceId);
std::wstring ParsePolicyDeviceId(const std::wstring& policyId);   // unrecognized passes through

class AudioPolicyConfig {
public:
    ~AudioPolicyConfig();
    bool Init(std::wstring* err);                 // resolves the factory, tries Win11 then Win10 IID
    bool IsAvailable() const { return m_factory != nullptr; }
    const wchar_t* IidUsed() const { return m_iidUsed; }   // L"win11" / L"win10" / L""

    // Persisted default render endpoint (eConsole + eMultimedia) for the app
    // owning `pid`. Empty endpointId is invalid here — use Clear.
    bool SetPersistedDefaultRender(unsigned long pid, const std::wstring& endpointId, std::wstring* err);
    bool ClearPersistedDefaultRender(unsigned long pid, std::wstring* err);
    bool GetPersistedDefaultRender(unsigned long pid, std::wstring* endpointId);

private:
    void* m_factory = nullptr;                    // IAudioPolicyConfigFactory*, opaque here
    const wchar_t* m_iidUsed = L"";
    // Init stands COM up itself and keeps it up for as long as it holds the
    // factory: the first caller is the UI thread at startup, which has no
    // apartment yet, and RoGetActivationFactory without one fails with
    // CO_E_NOTINITIALIZED — permanently disabling per-app routing.
    bool m_comInited = false;
};

} // namespace mdxm
