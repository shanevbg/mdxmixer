#include "test_framework.h"
#include "app/hotkeys.h"
#include "config/config.h"
#include <windows.h>
#include <commctrl.h>

using namespace mdxm;

MDXM_TEST_CASE(Hotkey_TargetKeysRoundTrip) {
    bool isDev = true, personal = false;
    std::wstring id;
    CHECK(ParseTargetKey(ChannelTargetKey(L"game", true), &isDev, &id, &personal));
    CHECK(!isDev && id == L"game" && personal);
    CHECK(ParseTargetKey(ChannelTargetKey(L"game", false), &isDev, &id, &personal));
    CHECK(!isDev && id == L"game" && !personal);

    // An endpoint id is full of braces, dots and its own colons; the parse has
    // to survive that, which is why the fader half is taken from the END.
    const std::wstring ep = L"{0.0.0.00000000}.{abc-def}";
    CHECK(ParseTargetKey(DeviceTargetKey(ep), &isDev, &id, &personal));
    CHECK(isDev && id == ep);

    CHECK(!ParseTargetKey(L"nonsense", &isDev, &id, &personal));
    CHECK(!ParseTargetKey(L"chan:game:x", &isDev, &id, &personal));
}

MDXM_TEST_CASE(Hotkey_ComboFormatsAsPrintedOnTheKeyboard) {
    CHECK(FormatCombo(0, 0) == L"(unbound)");
    CHECK(FormatCombo(MOD_CONTROL | MOD_ALT, VK_F5) == L"CTRL+ALT+F5");
    CHECK(FormatCombo(MOD_WIN, L'K') == L"WIN+K");
    CHECK(FormatCombo(0, VK_VOLUME_UP) == L"VOLUME UP");
}

MDXM_TEST_CASE(Hotkey_ModifierFlagsAreNotTheSameNumbers) {
    // HOTKEYF_SHIFT is 1 and MOD_ALT is 1: the two vocabularies collide on
    // every bit, so a missing conversion binds a different combination than
    // the one the user pressed and nothing about it looks wrong.
    CHECK(HOTKEYF_SHIFT != MOD_SHIFT);
    CHECK(ModFromCtrlFlags(HOTKEYF_SHIFT) == MOD_SHIFT);
    CHECK(ModFromCtrlFlags(HOTKEYF_CONTROL | HOTKEYF_ALT) == (MOD_CONTROL | MOD_ALT));
    CHECK(CtrlFlagsFromMod(MOD_ALT) == HOTKEYF_ALT);
    CHECK(CtrlFlagsFromMod(MOD_CONTROL | MOD_SHIFT) == (HOTKEYF_CONTROL | HOTKEYF_SHIFT));
    // Round trip, for the three the control can express.
    unsigned mod = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    CHECK(ModFromCtrlFlags(CtrlFlagsFromMod(mod)) == mod);
}

MDXM_TEST_CASE(Hotkey_BareKeysAreRefused) {
    std::wstring why;
    CHECK(!ComboIsUsable(0, L'K', &why));          // would eat every K typed
    CHECK(!why.empty());
    CHECK(!ComboIsUsable(MOD_SHIFT, L'K', &why));  // Shift+K is a capital K
    CHECK(!ComboIsUsable(0, 0, &why));             // nothing set

    CHECK(ComboIsUsable(MOD_CONTROL | MOD_ALT, L'K', nullptr));
    CHECK(ComboIsUsable(MOD_WIN, VK_F5, nullptr));
    // The keys that exist for this, modifier or not: nothing sends F13 by
    // accident, and taking the volume keys over is what a mixer is for.
    CHECK(ComboIsUsable(0, VK_F13, nullptr));
    CHECK(ComboIsUsable(0, VK_VOLUME_UP, nullptr));
    CHECK(ComboIsUsable(0, VK_MEDIA_PLAY_PAUSE, nullptr));
}

MDXM_TEST_CASE(Hotkey_VolumeClampsPerFaderNotPerGroup) {
    // The whole point of the rule: a group at mixed levels run to the top and
    // back down again must come back to the levels it started at. Scaling the
    // members together, or refusing the press because one is at the rail,
    // both flatten the group within a few presses.
    std::vector<TargetLevel> t = {
        { L"chan:game:p", 0.90f, false, true },
        { L"chan:chat:p", 0.50f, false, true },
    };
    auto up = StepVolumes(t, 20, true);
    CHECK(up.size() == 2);
    CHECK(up[0].second > 0.999f);          // clamped at the top
    CHECK(up[1].second > 0.69f && up[1].second < 0.71f);

    t[0].vol = up[0].second;
    t[1].vol = up[1].second;
    auto down = StepVolumes(t, 20, false);
    CHECK(down[0].second > 0.79f && down[0].second < 0.81f);
    CHECK(down[1].second > 0.49f && down[1].second < 0.51f);   // back where it was

    // The floor holds too.
    std::vector<TargetLevel> low = { { L"dev:x", 0.05f, false, true } };
    CHECK(StepVolumes(low, 20, false)[0].second == 0.0f);
}

MDXM_TEST_CASE(Hotkey_MuteIsOneDecisionForTheWholeGroup) {
    // Toggling each member independently leaves a mixed group flip-flopping
    // between two mixed states and never reaching silence, and reaching
    // silence is what a mute key is for.
    std::vector<TargetLevel> mixed = {
        { L"a", 0.5f, true,  true },
        { L"b", 0.5f, false, true },
    };
    CHECK(MuteAllDecision(mixed));      // one is audible: silence them all

    std::vector<TargetLevel> allMuted = {
        { L"a", 0.5f, true, true },
        { L"b", 0.5f, true, true },
    };
    CHECK(!MuteAllDecision(allMuted));  // all silent: bring them all back

    // A fader that refuses mute does not get to decide the group's direction.
    std::vector<TargetLevel> withRefuser = {
        { L"a", 0.5f, true,  true },
        { L"b", 0.5f, false, false },   // cannot mute, and is unmuted
    };
    CHECK(!MuteAllDecision(withRefuser));

    CHECK(!MuteAllDecision({}));        // nothing to mute
}

MDXM_TEST_CASE(Hotkey_RegistrarRecordsWhatWindowsAnswered) {
    // A binding is a REQUEST. Registering the same combination twice fails the
    // second time even inside one process, which is exactly the shape of the
    // collision this table exists to report (mdx12 fj#72).
    const wchar_t* kCls = L"mdxmixerTestHotkeyWnd";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kCls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    CHECK(hwnd != nullptr);

    // A combination nothing sensible owns: Ctrl+Alt+Shift+Win+F19.
    const unsigned mod = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    std::vector<HotkeyBinding> bindings;
    HotkeyBinding a;
    a.id = L"hk1"; a.label = L"first"; a.mod = mod; a.vk = VK_F19;
    bindings.push_back(a);
    HotkeyBinding b = a;
    b.id = L"hk2"; b.label = L"second";      // the same combination, again
    bindings.push_back(b);
    HotkeyBinding unbound;
    unbound.id = L"hk3"; unbound.label = L"never set";
    bindings.push_back(unbound);

    HotkeyRegistrar reg;
    reg.Apply(hwnd, bindings);
    // Two asked, one unbound and therefore not asked at all.
    CHECK(reg.Grants().size() == 2);
    const HotkeyGrant* first = reg.Find(L"hk1");
    const HotkeyGrant* second = reg.Find(L"hk2");
    CHECK(first && second);
    CHECK(first->held);
    CHECK(!second->held);                       // taken, and says so
    CHECK(second->lastError != 0);
    CHECK(reg.Find(L"hk3") == nullptr);

    // Only the granted one is dispatchable.
    bool foundFirst = false;
    for (int id = 0x2000; id < 0x2010; ++id)
        if (reg.BindingForId(id) == L"hk1") foundFirst = true;
    CHECK(foundFirst);
    CHECK(reg.BindingForId(0x9999).empty());

    reg.Clear(hwnd);
    CHECK(reg.Grants().empty());
    // Released: the same combination registers cleanly again afterwards.
    CHECK(ComboIsFree(mod, VK_F19));

    DestroyWindow(hwnd);
    UnregisterClassW(kCls, GetModuleHandleW(nullptr));
}

// ── A step per key, falling back to the default ──────────────────────────

MDXM_TEST_CASE(Hotkey_StepFallsBackToTheDefault) {
    // 0 is stored as a SENTINEL, not as a copy of the default, so changing
    // the default moves every key that never asked for its own number.
    HotkeyBinding b;
    b.stepPercent = 0;
    CHECK(StepForBinding(b, 5) == 5);
    CHECK(StepForBinding(b, 10) == 10);
}

MDXM_TEST_CASE(Hotkey_OwnStepWinsOverTheDefault) {
    // A coarse key for finding the ballpark and a fine one for settling on a
    // level are different keys; one global number cannot be both.
    HotkeyBinding coarse, fine;
    coarse.stepPercent = 20;
    fine.stepPercent = 1;
    CHECK(StepForBinding(coarse, 5) == 20);
    CHECK(StepForBinding(fine, 5) == 1);
}

MDXM_TEST_CASE(Hotkey_StepIsClampedBothWays) {
    // A hand-edited config must not make a key that moves nothing, nor one
    // that crosses the whole fader in a press.
    HotkeyBinding b;
    b.stepPercent = 9000;
    CHECK(StepForBinding(b, 5) == 50);
    // A negative or zero stored step is the sentinel, and the DEFAULT is
    // what gets clamped then.
    b.stepPercent = -3;
    CHECK(StepForBinding(b, 0) == 1);
    CHECK(StepForBinding(b, 999) == 50);
}

MDXM_TEST_CASE(Hotkey_StepSurvivesASave) {
    MixerConfig c;
    HotkeyBinding b;
    b.id = L"hk1";
    b.label = L"Aux fine";
    b.action = HotkeyAction::VolumeUp;
    b.stepPercent = 1;
    c.hotkeys.push_back(b);
    HotkeyBinding d;
    d.id = L"hk2";
    d.label = L"Aux coarse";
    c.hotkeys.push_back(d);          // no step of its own
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.hotkeys.size() == 2);
    if (back.hotkeys.size() == 2) {
        CHECK(back.hotkeys[0].stepPercent == 1);
        CHECK(back.hotkeys[1].stepPercent == 0);   // still the sentinel
    }
}
