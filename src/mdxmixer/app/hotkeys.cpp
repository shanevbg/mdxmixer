#include "app/hotkeys.h"
#include <windows.h>
#include <commctrl.h>   // HOTKEYF_*, the HOTKEY control's own flags
#include <algorithm>

namespace mdxm {

int StepForBinding(const HotkeyBinding& b, int defaultPercent) {
    const int pct = (b.stepPercent > 0) ? b.stepPercent : defaultPercent;
    return pct < 1 ? 1 : (pct > 50 ? 50 : pct);
}

std::wstring ChannelTargetKey(const std::wstring& channelId, bool personal) {
    return L"chan:" + channelId + (personal ? L":p" : L":s");
}

std::wstring DeviceTargetKey(const std::wstring& endpointId) {
    return L"dev:" + endpointId;
}

bool ParseTargetKey(const std::wstring& key, bool* isDevice, std::wstring* id,
                    bool* personal) {
    if (key.rfind(L"dev:", 0) == 0) {
        if (isDevice) *isDevice = true;
        if (id) *id = key.substr(4);
        if (personal) *personal = true;
        return key.size() > 4;
    }
    if (key.rfind(L"chan:", 0) != 0) return false;
    size_t colon = key.rfind(L':');
    if (colon <= 4) return false;
    std::wstring tail = key.substr(colon + 1);
    if (tail != L"p" && tail != L"s") return false;
    if (isDevice) *isDevice = false;
    if (id) *id = key.substr(5, colon - 5);
    if (personal) *personal = (tail == L"p");
    return true;
}

std::wstring FormatCombo(unsigned mod, unsigned vk) {
    if (vk == 0) return L"(unbound)";
    std::wstring out;
    if (mod & MOD_CONTROL) out += L"CTRL+";
    if (mod & MOD_ALT)     out += L"ALT+";
    if (mod & MOD_SHIFT)   out += L"SHIFT+";
    if (mod & MOD_WIN)     out += L"WIN+";
    switch (vk) {
    case VK_RETURN:  out += L"ENTER"; break;
    case VK_ESCAPE:  out += L"ESC"; break;
    case VK_SPACE:   out += L"SPACE"; break;
    case VK_TAB:     out += L"TAB"; break;
    case VK_BACK:    out += L"BACKSPACE"; break;
    case VK_DELETE:  out += L"DELETE"; break;
    case VK_INSERT:  out += L"INSERT"; break;
    case VK_HOME:    out += L"HOME"; break;
    case VK_END:     out += L"END"; break;
    case VK_PRIOR:   out += L"PGUP"; break;
    case VK_NEXT:    out += L"PGDN"; break;
    case VK_UP:      out += L"UP"; break;
    case VK_DOWN:    out += L"DOWN"; break;
    case VK_LEFT:    out += L"LEFT"; break;
    case VK_RIGHT:   out += L"RIGHT"; break;
    case VK_SCROLL:  out += L"SCROLL LOCK"; break;
    case VK_PAUSE:   out += L"PAUSE"; break;
    // The keys a mixer is most likely to be given, so they read as themselves
    // rather than as 0xAF.
    case VK_VOLUME_UP:      out += L"VOLUME UP"; break;
    case VK_VOLUME_DOWN:    out += L"VOLUME DOWN"; break;
    case VK_VOLUME_MUTE:    out += L"VOLUME MUTE"; break;
    case VK_MEDIA_PLAY_PAUSE: out += L"PLAY/PAUSE"; break;
    case VK_MEDIA_STOP:     out += L"STOP"; break;
    case VK_MEDIA_NEXT_TRACK: out += L"NEXT TRACK"; break;
    case VK_MEDIA_PREV_TRACK: out += L"PREV TRACK"; break;
    case VK_OEM_3:      out += L"`"; break;
    case VK_OEM_4:      out += L"["; break;
    case VK_OEM_6:      out += L"]"; break;
    case VK_OEM_COMMA:  out += L","; break;
    case VK_OEM_PERIOD: out += L"."; break;
    case VK_OEM_MINUS:  out += L"-"; break;
    case VK_OEM_PLUS:   out += L"="; break;
    case VK_OEM_1:      out += L";"; break;
    case VK_OEM_2:      out += L"/"; break;
    case VK_OEM_5:      out += L"\\"; break;
    case VK_OEM_7:      out += L"'"; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F24) {
            wchar_t buf[8];
            swprintf(buf, 8, L"F%d", (int)(vk - VK_F1 + 1));
            out += buf;
        } else if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9')) {
            out += (wchar_t)vk;
        } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
            wchar_t buf[12];
            swprintf(buf, 12, L"NUM %d", (int)(vk - VK_NUMPAD0));
            out += buf;
        } else {
            wchar_t buf[16];
            swprintf(buf, 16, L"0x%02X", vk);
            out += buf;
        }
        break;
    }
    return out;
}

unsigned ModFromCtrlFlags(unsigned hotkeyfFlags) {
    unsigned mod = 0;
    if (hotkeyfFlags & HOTKEYF_ALT)     mod |= MOD_ALT;
    if (hotkeyfFlags & HOTKEYF_CONTROL) mod |= MOD_CONTROL;
    if (hotkeyfFlags & HOTKEYF_SHIFT)   mod |= MOD_SHIFT;
    return mod;   // the HOTKEY control cannot express the Windows key
}

unsigned CtrlFlagsFromMod(unsigned mod) {
    unsigned flags = 0;
    if (mod & MOD_ALT)     flags |= HOTKEYF_ALT;
    if (mod & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (mod & MOD_SHIFT)   flags |= HOTKEYF_SHIFT;
    return flags;
}

bool ComboIsUsable(unsigned mod, unsigned vk, std::wstring* why) {
    auto no = [why](const wchar_t* msg) { if (why) *why = msg; return false; };
    if (vk == 0) return no(L"No key set.");

    // The keys that exist for exactly this. F13 and up are not on a normal
    // keyboard, so nothing sends them by accident; the media and volume keys
    // are ones a mixer has every reason to take over.
    if (vk >= VK_F13 && vk <= VK_F24) return true;
    if (vk >= VK_VOLUME_MUTE && vk <= VK_LAUNCH_APP2) return true;
    if (vk == VK_MEDIA_PLAY_PAUSE || vk == VK_MEDIA_STOP ||
        vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK) return true;

    // Everything else fires wherever the user is typing, so it has to be
    // something a person does not type: Ctrl, Alt or Win. Shift alone is not
    // enough — Shift+A is a capital A.
    if (!(mod & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
        return no(L"Add Ctrl, Alt or Win: a global key without one would fire "
                  L"wherever you are typing.");
    return true;
}

std::vector<std::pair<std::wstring, float>> StepVolumes(
    const std::vector<TargetLevel>& targets, int stepPercent, bool up) {
    std::vector<std::pair<std::wstring, float>> out;
    if (stepPercent <= 0) stepPercent = 5;
    const float step = (up ? 1.0f : -1.0f) * (float)stepPercent / 100.0f;
    for (const TargetLevel& t : targets) {
        float v = t.vol + step;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        out.push_back({ t.key, v });
    }
    return out;
}

bool MuteAllDecision(const std::vector<TargetLevel>& targets) {
    for (const TargetLevel& t : targets)
        if (t.canMute && !t.muted) return true;   // anything audible: silence it
    return false;
}

// ── Registration ─────────────────────────────────────────────────────────

namespace {
// WM_HOTKEY ids are ours to choose; they only have to be unique in this
// window. Starting well above zero so a stray 0 cannot look like a binding.
constexpr int kIdBase = 0x2000;
} // namespace

void HotkeyRegistrar::Clear(void* hwnd) {
    for (const auto& pair : m_ids) UnregisterHotKey((HWND)hwnd, pair.first);
    m_ids.clear();
    m_grants.clear();
}

void HotkeyRegistrar::Apply(void* hwnd, const std::vector<HotkeyBinding>& bindings) {
    Clear(hwnd);
    if (!hwnd) return;

    // RegisterHotKey demands the thread that owns the window and answers
    // ERROR_WINDOW_OF_OTHER_THREAD (1408) for every binding when it does not.
    // That failure is worse than useless: the table would record the whole set
    // as lost to another application when nothing of the sort happened. Refuse
    // and say nothing rather than write a table of lies. (mdx12 fj#72.)
    if (GetWindowThreadProcessId((HWND)hwnd, nullptr) != GetCurrentThreadId())
        return;

    int nextId = kIdBase;
    for (const HotkeyBinding& b : bindings) {
        if (b.vk == 0) continue;
        if (!ComboIsUsable(b.mod, b.vk, nullptr)) continue;
        const int id = nextId++;
        HotkeyGrant g;
        g.bindingId = b.id;
        g.mod = b.mod;
        g.vk = b.vk;
        SetLastError(0);
        // MOD_NOREPEAT: a held volume key would otherwise arrive at the
        // keyboard's repeat rate and run a fader to the rail in a second.
        g.held = RegisterHotKey((HWND)hwnd, id, b.mod | MOD_NOREPEAT, b.vk) != FALSE;
        if (!g.held) g.lastError = GetLastError();
        // A failure never aborts the pass: the bindings are independent, and
        // abandoning the rest because one was taken turns one dead key into
        // all of them.
        if (g.held) m_ids.push_back({ id, b.id });
        m_grants.push_back(g);
    }
}

const HotkeyGrant* HotkeyRegistrar::Find(const std::wstring& bindingId) const {
    for (const HotkeyGrant& g : m_grants)
        if (g.bindingId == bindingId) return &g;
    return nullptr;
}

std::wstring HotkeyRegistrar::BindingForId(int id) const {
    for (const auto& pair : m_ids)
        if (pair.first == id) return pair.second;
    return {};
}

bool ComboIsFree(unsigned mod, unsigned vk) {
    if (vk == 0) return false;
    // hwnd = NULL registers against the CALLING THREAD, which is what makes
    // this callable from a dialog. Released in the same breath: holding it any
    // longer takes the combination away from whatever the user meant to use it
    // for, and an early return would leave it that way for the session.
    const int kProbeId = 0x4FFF;
    if (!RegisterHotKey(NULL, kProbeId, mod | MOD_NOREPEAT, vk)) return false;
    UnregisterHotKey(NULL, kProbeId);
    return true;
}

} // namespace mdxm
