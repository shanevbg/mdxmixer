#pragma once
// mdxmixer.json — one exe-relative file, the spec's Configuration section.
// Values are clamped at load time (volumes to [0,1], EQ bands to kBands):
// bad config never reaches the engine unclamped.
#include "json_utils.h"
#include "device/device_identity.h"   // DeviceAlias
#include "app/hotkeys.h"                 // HotkeyBinding
#include <functional>
#include <string>
#include <vector>

namespace mdxm {

// A stored reference to one audio device.
//
// THE ADDRESS IS THE ONLY ANCHOR THAT SURVIVES A CHANGE OF ADAPTER, and that
// is not a hypothetical on this machine: "I would prefer to not have to guess
// every time I change out the bluetooth adapter hoping for a better and more
// stable connection". Swapping dongles re-pairs every headset, which mints a
// new endpoint id AND a new ContainerId for each one, and Windows renames them
// too -- the same earbuds come back as "Headphones (12- WF-1000XM5)" where they
// were "(11- ...)". So an entry keyed on id and name alone is dead on arrival
// after a swap, which is how one failover list came to hold 21 entries for
// seven headsets.
//
// The Bluetooth address belongs to the HEADSET, not to the pairing, so it
// survives. It is empty for anything not on Bluetooth and for entries written
// before this field existed; those are filled in by HealDeviceRefs the first
// time the entry is matched to a live device.
struct DeviceRef   { std::wstring id, name, btAddress; };

// personalOutput.id when the route is not bound to a device at all, but
// follows the failover list: whichever allowed device is present wins, and a
// better one taking its place is an ordinary commit rather than a recovery.
//
// It exists because the dropdown could not say what it was doing. "None"
// meant "follow the Windows default", the failover list meant "take over if
// the bound device vanishes", and there was no way to ask for the obvious
// third thing -- "I don't care which, use my list" -- which is the one Shane
// actually wanted: "it should have an entry \"use failover\" ... otherwise
// it gets confusing".
inline const wchar_t* kFollowFailover = L"\x01followFailover";
struct CableRef    { DeviceRef render, capture; };
struct MixSetting  { float vol = 1.0f; bool mute = false; };
struct EqBandConfig{ double freq = 1000.0, gainDb = 0.0, q = 1.0; };
struct EqConfig    { bool enabled = false; std::vector<EqBandConfig> bands; };
struct ChannelConfig {
    std::wstring id, name;
    CableRef cable;
    MixSetting personal, streaming;
    EqConfig eq;
    std::vector<std::wstring> apps;      // exe paths assigned to this channel
};
struct MicConfig   { DeviceRef input; CableRef cable; float gain = 1.0f; EqConfig eq; };
struct FailoverConfig { bool armed = false; std::vector<DeviceRef> allow;   // ordered: first present wins
                        int stabilitySec = 5; int dwellSec = 30;
                        // The floor between ANY two endpoint reassignments,
                        // across every route (MDropDX12 #410). dwellSec
                        // governs one route and cannot see the others.
                        int minGapSec = 5; };
struct UiConfig    { // A taskbar button as well as the tray icon. ON by
                     // default: the spec asked for "both systray and taskbar
                     // options", and with it off the window has no taskbar
                     // entry at all -- click away from it and the only way
                     // back is the tray, which is what shipping it off
                     // actually felt like. The tray icon is there either way;
                     // this only decides whether the taskbar shows it too,
                     // and with it on, minimising goes to the taskbar instead
                     // of hiding.
                     bool taskbarButton = true;       // JSON: "ui": { "taskbarButton": .., "theme": .. }
                     // The sticky tack: keep the window above everything else.
                     //
                     // MDropDX12 puts this pin on all twenty-odd of its tool
                     // windows and defaults it ON there, because a tool window
                     // is small and is opened to be watched. This is the whole
                     // mixer, so it defaults OFF -- a full window forcing
                     // itself over everything on first run is a surprise, and
                     // one click makes it stick for good.
                     bool alwaysOnTop = false;
                     // Which tab was open when the window was last used.
                     //
                     // Saved on a USER tab change only, never when a tab is
                     // selected by MDXM_TAB: a script taking a screenshot of
                     // the Devices page must not rewrite where the window
                     // opens tomorrow. Clamped on load, so a config naming a
                     // tab that no longer exists opens on the Mixer rather
                     // than on nothing.
                     int activeTab = 0;
                     // Fader control: sliders, or step buttons. MDropDX12's
                     // own option, down to the name -- its Options tab reads
                     // "Fader control: sliders, or spin boxes that step by one
                     // percent" and stores spinBoxes. A slider cannot be
                     // nudged without aiming at it first, and dragging one to
                     // a level you can name is fiddly.
                     //
                     // How far a step moves is volumeStepPercent, which mdx12
                     // already defines and which the volume hotkeys use, so
                     // one number governs both and "+10" means the same thing
                     // wherever it appears.
                     bool spinBoxes = false;
                     // Draw the endpoints a provider owns. OFF, as in mdx12,
                     // and for its reason: they shadow the provider channels
                     // by name, sit at 1.000 and ignore a write, so the row
                     // that looks like the one you want is a decoy. See
                     // IsProviderOwnedEndpoint.
                     bool showVirtualEndpoints = false;
                     // How the failover allowlist is ORDERED ON SCREEN, and
                     // nothing more. MDropDX12 carries the same setting under
                     // the same name, with the same four values:
                     //
                     //   0  the preferred order -- the rule itself, the default
                     //   1  device name
                     //   2  battery
                     //   3  last seen
                     //
                     // A view rule, never written back to the rule: the stored
                     // order IS the failover preference, so a sorted view must
                     // not become one. Remembered because it is a standing
                     // choice about how the list reads -- "the problem is that
                     // I sort by last seen and it doesn't remember that".
                     int allowSort = 0;
                     // Which way, for a column clicked a second time. mdx12
                     // has no equivalent because its headers do not sort; here
                     // they do, so the direction is part of the same choice.
                     bool allowSortDesc = false;
                     // The tool windows' font height, shared by all of them.
                     // A CreateFontW height: negative is a character height,
                     // which is what the +/- buttons move and what mdx12
                     // stores as SettingsFontSize. Clamped on load to the
                     // same -12..-32 band those buttons enforce.
                     int toolFontSize = -20;
                     // "dracula" | "dark" (green) | "light" | "system"
                     std::wstring theme = L"system"; };
// The battery readout: a frameless, always-on-top line of text sitting over
// whatever is on screen.
//
// It exists because the battery is the one number worth knowing WITHOUT
// opening a mixer — and because more than one headset is often connected at
// once on this machine, which is usually the sign that one of them is not
// charging properly. Every connected device that reports a battery gets a
// line, so that case is visible rather than inferred.
struct BatteryOverlayConfig {
    bool enabled = false;
    int  x = 40, y = 40;        // screen position, persisted after a drag
    int  opacity = 85;          // percent, 10..100
    int  fontSize = 16;
    // Click-through: the window stops receiving the mouse entirely, which is
    // the point — it sits over a game and is not in the way. It also means it
    // cannot be dragged, so turning this OFF is how it gets moved, and the X
    // and Y boxes in Options move it either way.
    bool clickThrough = true;
    // Adapt to MDropDX12's watermark instead of competing with it.
    //
    // ON by default, because it is what makes the overlay behave when the two
    // programs run together -- which on this machine is most of the time. Two
    // things hang off it, both described in ui/topmost.h: mdx12's topmost
    // windows are FOLLOWED rather than raised over, so neither program spends
    // the day shoving the other down a shared band; and while the watermark is
    // actually over the overlay, its opacity is lifted by 30% of itself
    // (between 40 and 80 only) to pay for being read through a 30%-opaque
    // layer.
    //
    // Off is a real choice, not a safety valve: it makes the overlay compete
    // like any other topmost window, which is what someone who wants the
    // battery readout above everything -- watermark included -- would pick.
    bool mdx12Adaptive = true;
    // A painted panel behind the text, or nothing at all. Off by default:
    // "only the text Batt: ##% will appear". Off is done with a colour key,
    // which is why the text is drawn without antialiasing in that mode — the
    // blended edge pixels would key out as a halo.
    bool background = false;
    // A visible frame, and with it a window that can be dragged.
    //
    // Click-through and draggable are mutually exclusive — WS_EX_TRANSPARENT
    // means the window never receives the mouse — so this one switch governs
    // both: frame on, the overlay is solid, bordered, opaque and grabbable;
    // frame off, it goes back to being a line of text that the mouse passes
    // straight through. Persisted, because "turn the frame on, drag it, turn
    // it off" is a mode the user is in, not a gesture, and a mode that undid
    // itself would be one he had to re-enter for every nudge.
    bool frame = false;
    // $b  battery percent        $sn short name (the alias, if one is set)
    // $n  the full Windows name  $$  a literal dollar
    std::wstring text = L"Batt: $b%";
};

// The VBAN stream server (spec docs/specs/2026-10-07-vban-stream-server-design.md §6.1).
//
// A device that has been approved once, so the next connection from it is
// instant rather than another prompt. Keyed on the id the phone mints for
// itself, which is also what MDropDX12's TCP auth uses -- one approval concept
// across both services. `name` is only ever shown to a person.
struct VbanAuthorizedDevice { std::wstring id, name; std::wstring lastSeen; };

// The display-capture stream: how often, how large, how compressed. Separate
// from the audio settings because a person tuning picture quality is not
// thinking about the monitor mix.
struct VbanFramesConfig { double fps = 2.0; int quality = 60; int maxEdge = 480; };

struct VbanConfig {
    // OFF, and the listener only exists while it is on. This is the first
    // inbound socket in the program, so it is opt-in -- but note that unlike
    // the shared-memory feed, `enabled` PERSISTS: the point of the feature is
    // that the phone can subscribe at any moment without anyone touching the
    // PC, so the listener has to survive a restart. Emission does not: nothing
    // is sent until a subscriber asks.
    bool enabled = false;
    int  port = 6980;                         // the spec's default
    std::wstring bindAddress;                 // empty = any interface
    std::wstring streamName = L"mdxmixer";    // <= 16 ASCII on the wire
    // Which sum goes out. PERSONAL by default: the point is to hear what the
    // headphones hear, per-channel balance and personal mutes included. The
    // streaming sum is the programme mix and a different thing entirely.
    bool sourceStreaming = false;
    // int16 by default -- half the bandwidth and the format every VBAN
    // receiver accepts. f32 is lossless and exists for the case where the
    // makeup gain below is doing something extreme.
    bool formatFloat32 = false;
    // Makeup gain, applied PC-side before the wire. It goes to 6400% because
    // the personal mix on this machine runs at a few percent of full scale --
    // the faders ARE the listening level -- and sending that unamplified would
    // put a 20-40 dB-down signal into a phone.
    int  gainPercent = 100;                   // 0..6400
    // Empty disables remote control entirely: no PIN, no TXT, and the
    // authorized list below is inert. Discovery and (if opened) audio still
    // work, which is a legal and deliberate "serve, but take no orders" state.
    std::wstring pin;
    std::vector<VbanAuthorizedDevice> authorizedDevices;
    // Escape hatches for standard VBAN tools, which cannot authenticate
    // because the protocol has no notion of it. Both off: subscription-by-ping
    // is ours, and these are the two ways to serve software that only speaks
    // the plain protocol.
    bool openSubscribe = false;               // any pinger gets AUDIO, nothing else
    bool alwaysStream = false;                // emit with no subscriber at all
    bool alwaysFrames = false;                // ... and send display frames too
    std::wstring alwaysStreamTarget;          // "ip:port"; empty = inert
    VbanFramesConfig frames;
};

// Remember a device, or forget one. Pure, and shared by the approval callback and
// the Revoke button so the two cannot drift: approving twice must not list a phone
// twice (the Revoke list shows this, and two rows for one device is a trap -- which
// one does Revoke remove?), and a renamed phone keeps its single row.
//
// Both return whether anything actually changed, so a caller can skip a config
// write that would do nothing. An entry with no id is refused: it could never be
// matched to a device and would sit in the list unreachable.
bool UpsertAuthorizedDevice(std::vector<VbanAuthorizedDevice>& list,
                            const std::wstring& id, const std::wstring& name,
                            const std::wstring& lastSeen = std::wstring());
bool RemoveAuthorizedDevice(std::vector<VbanAuthorizedDevice>& list,
                            const std::wstring& id);

// Where the main window was, and how big.
//
// It was never saved: every launch opened at a hardcoded 860x600, so a window
// widened to read the device names had to be widened again next time -- "it's
// still not saving ... the width I have to keep widdening it to". The tool
// windows have had their own geometry store since they were ported; the main
// window simply never got one.
//
// w == 0 means nothing has been stored yet, which is how a first run still
// gets the default size.
struct WindowPlacement {
    int x = 0, y = 0, w = 0, h = 0;
    bool maximized = false;
};

struct MixerConfig {
    std::vector<ChannelConfig> channels;
    DeviceRef personalOutput;
    FailoverConfig personalFailover;
    CableRef  streamingCable;
    MicConfig mic;
    UiConfig  ui;
    BatteryOverlayConfig batteryOverlay;
    WindowPlacement window;
    // Our names for audio devices. Windows calls five identical pairs of
    // earbuds WF-1000XM5-1 through -5, refuses to rename them persistently,
    // and hands them a new endpoint id on every re-pair — so the name is ours
    // to keep, stored with the Windows name that re-associates it.
    std::vector<DeviceAlias> deviceNames;
    // Global hotkeys, all unbound until someone binds one. Sonar binds mute
    // and volume per channel and lets a key cover any number of channels, so
    // a binding here carries its own target list rather than there being one
    // fixed group.
    std::vector<HotkeyBinding> hotkeys;
    // Display order for the mixer rows, most used first.
    //
    // PORTED from MDropDX12, whose comment reads "Display order, as
    // 'channel|fader' keys, most used first. Anything not listed is shown
    // after, in provider order." Same idea, same fallback.
    //
    // Keys are "chan:<id>" for a channel and "dev:<endpointId>" for a
    // device. A CHANNEL, not a fader: its Personal and Streaming rows are a
    // pair and move together, which halves the list to manage and keeps the
    // pair adjacent where every other part of the layout expects it.
    //
    // It exists because there was no way to arrange the thing at all: "I
    // can't figure out how to move the faders I use up or pin them and
    // clicking on the channels doesn't do anything except allow me to hide
    // them". Pin and hide were device-only; nothing could be reordered.
    std::vector<std::wstring> order;
    int  volumeStepPercent = 5;    // what one press of a volume key moves
    // What MDropDX12 is fed, as a percentage of the streaming mix. 100 by
    // default: the visualiser wants a full-level signal, which is the whole
    // reason it reads the streaming mix rather than the personal one.
    int  mdx12FeedPercent = 100;
    // The FLOOR under the mix cushion, in milliseconds -- which is the
    // latency, because the cushion is what the mix waits for before it starts
    // draining a channel.
    //
    // A floor rather than the whole story: the cushion also adapts to the
    // largest pull in the last minute, so a render that bursts is covered by
    // that and is not what this governs (dsp/pull_window.h). This number is
    // the headroom carried when the link is behaving -- measured on this
    // machine, a WF-1000XM5 on a good link pulls a steady 480 frames, 10 ms,
    // so 30 ms is three pulls of slack.
    //
    // Exposed because the right value is a property of the hardware and the
    // radio environment, not something to hard-code: the honest way to find it
    // is to lower it and watch `underruns` in MDXM_DIAG, which is why
    // MDXM_CUSHION can set it live.
    int  cushionMs = 30;
    // The ADAPTIVE part of the cushion, which is what actually governs once
    // the floor above is out of the way:
    //
    //     cushion = max( floor , pull + pull*headroom% + flat )
    //
    // `headroom` is proportional slack against a pull that grows; `flat` is a
    // fixed margin for scheduling jitter that does not scale with buffer size.
    // Measured here: a WF-1000XM5 pulls 480 frames (10 ms), so the defaults of
    // 50% and 10 ms put the cushion at 25 ms.
    //
    // Both are exposed for the same reason as the floor: the right values
    // depend on the radio environment and the headset, and the way to find
    // them is to lower them and watch `underruns`.
    int  cushionHeadroomPercent = 50;
    int  cushionFlatMs = 10;
    bool autostart = false;
    int  logLevel = 2;
    // VBAN stream server (spec docs/specs/2026-10-07-vban-stream-server-design.md §6.1)
    VbanConfig vban;
};

MixerConfig  ConfigFromJson(const JsonValue& root);   // missing/wrong-typed fields -> defaults
std::wstring ConfigToJson(const MixerConfig& c);      // pretty-printed
// Never throws. The parser is lenient (a truncated file still parses to a partial
// object), so completeness is proven by a "complete": true marker written as the
// LAST key on save: truncation anywhere loses it and the load falls back to defaults.
MixerConfig  LoadConfig(const std::wstring& path, bool* usedDefaults);
bool         SaveConfigAtomic(const std::wstring& path, const MixerConfig& c); // temp + rename (JsonSaveFile)

// Owns the live config + its path. Mutations mark dirty; FlushIfDue (UI timer)
// writes at most once per second after the last mutation; FlushNow on exit.
class ConfigStore {
public:
    void Init(const std::wstring& path, const MixerConfig& initial) {
        m_path = path; m_config = initial; m_dirty = false;
    }
    MixerConfig& Get() { return m_config; }
    const MixerConfig& Get() const { return m_config; }
    void Mutate(const std::function<void(MixerConfig&)>& fn);
    void FlushIfDue();
    void FlushNow();

private:
    std::wstring m_path;
    MixerConfig m_config;
    bool m_dirty = false;
    unsigned long long m_lastMutateMs = 0;
};

} // namespace mdxm
