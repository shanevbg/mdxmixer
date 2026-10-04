#pragma once
// The one funnel. Owns config, engine, pipe, watcher, policy API and the window;
// implements IMixerControl for both the protocol layer and the UI. Every write
// path is: clamp -> engine call -> config mutate -> BroadcastState(). All control
// runs on the UI thread — pipe requests are marshaled there via MainWindow::kIpcMsg.
#include "config/config.h"
#include "device/device_watcher.h"
#include "engine/engine.h"
#include "ipc/mixer_control.h"
#include "ipc/pipe_server.h"
#include "routing/audio_policy_config.h"
#include "routing/sonar_api.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include "ui/ui_context.h"
#include <map>
#include <windows.h>

namespace mdxm {

// Everything the config has to say about a freshly-enumerated device list:
// aliases, the pin and hide flags, the failover pins, and the sort. Sorted on
// the way out, so every caller sees the same list in the same order.
//
// Free, and shared, because there are two callers and they were drifting. The
// window got its list through AppController and showed Shane's names; the
// --levels CLI called ListEndpointVolumes directly and showed Windows' names,
// so checking the one against the other meant mapping "Headphones (9-
// WF-1000XM5)" onto "WMX5 White" by hand — on the machine with five
// identically-named pairings, which is the reason the aliases exist at all.
//
// `cfg` is non-const because an alias re-homes onto the endpoint's current id
// when it has to match by Windows name; that is how a re-paired headset keeps
// its name, and it is a write.
// `personalRouteId` is the endpoint mdxmixer's own mix is PLAYING TO right
// now. It is hoisted to the top like a pinned device, because it is the
// literal answer to "where is the headset I am listening through" and it must
// not depend on that headset happening to be on a failover allowlist — plug
// in a pair that was never added there and the allowlist hoists nothing.
void ApplyDeviceView(MixerConfig& cfg, std::vector<DeviceLevel>& levels,
                     const std::wstring& personalRouteId = std::wstring());

class AppController : public IMixerControl {
public:
    bool Start(HINSTANCE hInstance);
    void Stop();

    // IMixerControl (UI thread only; the pipe marshals onto it)
    std::vector<ChannelState> GetChannels() override;
    std::vector<std::pair<std::wstring, std::wstring>> GetRoutes() override;
    std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> GetDevices() override;
    bool SetVolume(const std::wstring& ch, Mix m, float vol) override;
    bool SetMute(const std::wstring& ch, Mix m, bool mute) override;
    bool SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q) override;
    bool EnableEq(const std::wstring& ch, bool on) override;
    bool AssignApp(const std::wstring& exePath, const std::wstring& chOrDash) override;
    bool SetPersonalRoute(const std::wstring& endpointId) override;
    bool SetDefaultOutput(const std::wstring& endpointId, std::wstring* err) override;
    bool RouteAppToEndpoint(const std::wstring& exePath, const std::wstring& endpointId,
                            std::wstring* err) override;
    DiagState GetDiag() override;
    bool ShowUi() override;
    bool ExitApp() override;
    bool CaptureUi(const std::wstring& path, const std::wstring& window) override;
    std::vector<DeviceLevel> GetDeviceLevels() override;
    bool SetFeedEnabled(bool on, std::wstring* err) override;
    bool FeedEnabled() override;
    uint32_t FeedRate() override;
    FailoverConfig GetFailover() override;
    bool SetFailoverArmed(bool armed) override;
    bool SetFailoverTiming(int stabilitySec, int dwellSec, int minGapSec) override;
    bool SetFailoverAllow(const std::vector<DeviceRef>& allow) override;
    bool SetDeviceVolume(const std::wstring& endpointId, float vol01) override;
    bool SetDeviceMute(const std::wstring& endpointId, bool mute) override;
    bool SetDeviceAlias(const std::wstring& endpointId, const std::wstring& containerId,
                        const std::wstring& windowsName, const std::wstring& alias) override;
    bool SetDeviceView(const std::wstring& endpointId, const std::wstring& containerId,
                       const std::wstring& windowsName, bool hidden, bool pinned) override;
    bool ShowHotkeysUi() override;
    std::wstring GetHotkeyStatus(const std::wstring& bindingId) override;
    std::vector<HotkeyBinding> GetHotkeys() override;
    bool SetHotkeys(const std::vector<HotkeyBinding>& bindings) override;
    int  GetVolumeStep() override;
    bool SetVolumeStep(int percent) override;
    bool TriggerHotkey(const std::wstring& bindingId) override;
    std::vector<std::pair<std::wstring, std::wstring>> GetHotkeyTargets() override;

    bool AddChannel(const std::wstring& name, const std::wstring& sourceEndpointId,
                    std::wstring* err) override;
    bool RemoveChannel(const std::wstring& channelId) override;

private:
    void BroadcastState();
    // MDXM_PEAK to subscribed clients, from the 250 ms tick. Returns at once
    // when nobody is subscribed; see the definition.
    void PushPeaks();
    void RestartEngine();
    // Apply every stored app assignment that is not already in force. Control
    // thread only; see the definition.
    void ReconcileRouting(const wchar_t* why);
    std::wstring ExeDir();
    // Replaces each level's instantaneous peak with a held one. See the
    // definition; called from GetDeviceLevels and nowhere else.
    void HoldDevicePeaks(std::vector<DeviceLevel>& levels);

    ConfigStore m_store;
    Engine m_engine;
    PipeServer m_pipe;
    DeviceWatcher m_watcher;
    AudioPolicyConfig m_policy;
    // Sonar's own channels — Aux, Media, Game, Chat, Mic, Master — each with
    // the same Personal/Streaming pair as a native channel. Not an engine
    // channel: Sonar's virtual endpoints ignore a Windows volume write, so
    // these faders only move by asking Sonar.
    SonarChannels m_sonar;
    MainWindow m_window;
    UiContext m_uiCtx;
    ThemeState m_theme;
    bool m_firstRun = false;

    // ── Peak metering state ─────────────────────────────────────────────
    //
    // The endpoint sweep reads an INSTANTANEOUS peak, four times a second at
    // best, and a transient between two of those reads is simply not there.
    // That is useless for the thing the number is for, so the hold lives here,
    // across sweeps, keyed by endpoint id. Milliseconds are the tick unit on
    // this side; the mix thread counts frames (dsp/peak_hold.h).
    //
    // A map rather than a field on DeviceLevel because DeviceLevel is rebuilt
    // from scratch by every sweep: there is nowhere in it for a value to
    // survive. Entries for endpoints that stop appearing are dropped, so an
    // unplugged headset does not keep a peak alive for the session.
    struct DevicePeak { PeakHold hold; unsigned lastSweepMs = 0; };
    std::map<std::wstring, DevicePeak> m_devPeaks;
    unsigned m_lastSweepMs = 0;

    // Sonar's channel peaks, joined onto its virtual endpoints by the last
    // sweep. Cached rather than looked up on demand because GetChannels must
    // not run a COM sweep of its own -- MDXM_STATE asks for channels and for
    // device levels in the same reply, and enumerating all twenty-eight
    // endpoints twice to answer one request is a cost with nothing to show for
    // it. Stale entries expire: see the use site.
    std::map<std::wstring, float> m_sonarPeaks;
    unsigned m_sonarPeaksMs = 0;

    // The last swept list, kept so the 250 ms peak push does not have to run a
    // sweep of its own when the mixer tab has just run one.
    //
    // Without it, a subscribed client and a visible window together mean EIGHT
    // sweeps a second rather than four, each activating COM on every endpoint
    // — and that path is the one that faulted inside AudioSes.dll while
    // Bluetooth headsets came and went (see ListEndpointVolumes). It is
    // guarded, but doubling the rate of the one call known to fault, to
    // produce a list that already exists, is not a trade worth making.
    std::vector<DeviceLevel> m_lastLevels;

    // Is MixPull still being called? See Engine::MixFrames. Watched for
    // MOVEMENT rather than compared against a clock the audio thread sets,
    // because the audio thread must not call a clock.
    uint64_t m_lastMixFrames = 0;
    unsigned m_mixMovedMs = 0;
    bool m_mixSeen = false;
};

} // namespace mdxm
