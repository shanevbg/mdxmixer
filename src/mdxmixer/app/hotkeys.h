#pragma once
// Global hotkeys: mute, louder and quieter, aimed at any number of faders.
//
// The shape follows what Sonar does and what MDropDX12 learned doing it.
// Sonar binds mute / volume up / volume down PER CHANNEL, and a key may cover
// none, one or several channels; mdx12 shipped one "Group 1" whose members are
// ticked from a list. Those are the same thing seen from two ends, so a binding
// here carries an action and a TARGET SET: one target is Sonar's per-channel
// key, several is mdx12's group, and zero is allowed and says so rather than
// silently doing nothing.
//
// Everything ships UNBOUND. A mixer that claimed keys on first run would take
// them from whatever the user already had on them.
//
// Pure except for Registrar, so the parts that are easy to get wrong — what a
// combination is worth registering, how a group moves together, which way a
// mixed group's mute key goes — are tested without a keyboard.
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm {

enum class HotkeyAction { VolumeUp, VolumeDown, MuteToggle, ShowWindow };

// A fader a hotkey can move. "chan:<id>:p" and "chan:<id>:s" are a channel's
// Personal and Streaming faders; "dev:<endpointId>" is a device's own Windows
// volume. One key space, so a binding never has to say which kind it holds.
std::wstring ChannelTargetKey(const std::wstring& channelId, bool personal);
std::wstring DeviceTargetKey(const std::wstring& endpointId);
bool ParseTargetKey(const std::wstring& key, bool* isDevice, std::wstring* id,
                    bool* personal);

struct HotkeyBinding {
    std::wstring id;             // stable, for the config file and the UI list
    std::wstring label;          // what the user called it
    // How far ONE press of this key moves a fader, as a percentage.
    //
    // 0 means "use the default", which is ui/volumeStepPercent -- stored as a
    // sentinel rather than as a copy of the default, so changing the default
    // moves every key that never asked for anything else. A key that wants
    // its own number holds it here and ignores the default thereafter.
    //
    // NOT in MDropDX12, which has only the one global number. One size does
    // not fit: a coarse key for finding the right ballpark and a fine one for
    // settling on a level are different keys, and Shane already adjusts his
    // aux personal level in steps of 1 while moving other things in tens.
    int stepPercent = 0;
    HotkeyAction action = HotkeyAction::VolumeUp;
    unsigned mod = 0;            // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    unsigned vk = 0;             // 0 = unbound, which is how everything ships
    std::vector<std::wstring> targets;
};

// The step one press of this binding moves: its own, or the default when it
// has not asked for one. Clamped to the same 1..50 the Options box allows,
// so a hand-edited config cannot produce a key that moves nothing or jumps
// the whole fader.
int StepForBinding(const HotkeyBinding& b, int defaultPercent);

// "CTRL+ALT+F5", or "(unbound)" for vk 0. Named the way the keys are printed
// on the keyboard rather than by virtual-key number.
std::wstring FormatCombo(unsigned mod, unsigned vk);

// The HOTKEY common control speaks HOTKEYF_*, RegisterHotKey speaks MOD_*, and
// the two disagree on every bit: HOTKEYF_SHIFT is 1 where MOD_ALT is 1. A
// silent mistranslation binds the wrong combination, so it is converted in one
// place and tested.
unsigned ModFromCtrlFlags(unsigned hotkeyfFlags);
unsigned CtrlFlagsFromMod(unsigned mod);

// Whether a combination is worth handing to RegisterHotKey.
//
// A global key fires wherever the user is typing, so a bare letter or a plain
// Shift+letter would eat that letter everywhere on the machine. Ctrl, Alt or
// Win is required — except for the keys that exist for this: F13..F24, which
// no keyboard sends by accident, and the media and volume keys, where taking
// the key over IS the point for a mixer.
bool ComboIsUsable(unsigned mod, unsigned vk, std::wstring* why);

// ── What a key does when it lands ────────────────────────────────────────
//
// One target as read at the moment the key was pressed.
struct TargetLevel {
    std::wstring key;
    float vol = 1.0f;       // 0..1
    bool  muted = false;
    bool  canMute = true;   // a fader that refuses mute is skipped, not obeyed
};

// New levels for a volume key, clamped PER TARGET.
//
// Not scaled together and not refused as a whole when one member is at the
// rail: a group whose faders sit at different levels has to keep those
// differences after the key is held down at the top and let back up. Both
// alternatives flatten the group to one level within a few presses.
std::vector<std::pair<std::wstring, float>> StepVolumes(
    const std::vector<TargetLevel>& targets, int stepPercent, bool up);

// Which way a mute key moves the whole group: true mutes every member.
//
// One decision for the group, not a toggle each. Toggling independently leaves
// a mixed group flip-flopping between two mixed states and never reaching
// silence, and silence is what a mute key is for. If ANY member is audible the
// key mutes; only when all of them are already muted does it unmute. A target
// that refuses mute does not get a vote.
bool MuteAllDecision(const std::vector<TargetLevel>& targets);

// ── Registration ─────────────────────────────────────────────────────────
//
// A binding is a REQUEST, not a fact: RegisterHotKey is first-come-first-served
// across the whole machine, so what the user asked for and what Windows granted
// are two different things and mdx12 learned to keep them apart (fj#72). A
// combination another application owns otherwise looks configured, does
// nothing, and explains nothing.
struct HotkeyGrant {
    std::wstring bindingId;
    unsigned mod = 0, vk = 0;
    bool held = false;       // RegisterHotKey returned TRUE
    unsigned long lastError = 0;   // why not, when it did not
};

class HotkeyRegistrar {
public:
    // Registers every bound entry against `hwnd`, replacing whatever was
    // registered before. Must run on the thread that owns the window:
    // RegisterHotKey answers ERROR_WINDOW_OF_OTHER_THREAD otherwise, for every
    // binding, which would report the whole table as stolen.
    void Apply(void* hwnd, const std::vector<HotkeyBinding>& bindings);
    void Clear(void* hwnd);

    // What Windows answered, rebuilt by each Apply.
    const std::vector<HotkeyGrant>& Grants() const { return m_grants; }
    const HotkeyGrant* Find(const std::wstring& bindingId) const;

    // The binding a WM_HOTKEY id belongs to, or empty.
    std::wstring BindingForId(int id) const;

private:
    std::vector<HotkeyGrant> m_grants;
    std::vector<std::pair<int, std::wstring>> m_ids;   // WM_HOTKEY id -> binding
};

// Try a combination and let it go again, to say whether it is free.
//
// Windows offers no way to ask what another process holds, so the only honest
// test is to take it and release it — the same standard a real binding is held
// to. Never call it for a combination this process already holds: a second
// registration fails inside one process exactly as it does across two, so the
// answer would be "taken" by us, about us.
bool ComboIsFree(unsigned mod, unsigned vk);

} // namespace mdxm
