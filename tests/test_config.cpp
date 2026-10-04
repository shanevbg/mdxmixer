#include "test_framework.h"
#include "config/config.h"
#include <windows.h>

using namespace mdxm;

static MixerConfig SampleConfig() {
    MixerConfig c;
    ChannelConfig ch;
    ch.id = L"game"; ch.name = L"Game";
    ch.cable.render  = { L"{render-guid}",  L"CABLE-A Input" };
    ch.cable.capture = { L"{capture-guid}", L"CABLE-A Output" };
    ch.personal  = { 0.85f, false };
    ch.streaming = { 1.0f,  true };
    ch.eq.enabled = true;
    ch.eq.bands.push_back({ 62.5, 3.0, 1.4 });
    ch.apps.push_back(L"C:/Games/game.exe");
    c.channels.push_back(ch);
    c.personalOutput = { L"{hp-guid}", L"Headphones" };
    c.streamingCable.render = { L"{sc-guid}", L"CABLE-B Input" };
    c.mic.input = { L"{mic-guid}", L"Microphone" };
    c.mic.cable.render = { L"{mc-guid}", L"CABLE-C Input" };
    c.mic.gain = 0.9f;
    c.personalFailover.armed = true;
    c.personalFailover.allow.push_back({ L"{spk-guid}", L"Speakers" });
    c.personalFailover.stabilitySec = 7;
    c.personalFailover.dwellSec = 45;
    c.ui.taskbarButton = true;
    c.autostart = true;
    c.logLevel = 3;
    return c;
}

MDXM_TEST_CASE(Config_RoundTripPreservesEverything) {
    MixerConfig a = SampleConfig();
    MixerConfig b = ConfigFromJson(JsonParse(ConfigToJson(a)));
    CHECK(b.channels.size() == 1);
    CHECK(b.channels[0].id == L"game");
    CHECK(b.channels[0].cable.capture.name == L"CABLE-A Output");
    CHECK_NEAR(b.channels[0].personal.vol, 0.85f, 1e-6);
    CHECK(b.channels[0].streaming.mute == true);
    CHECK(b.channels[0].eq.enabled == true);
    CHECK_NEAR(b.channels[0].eq.bands.at(0).q, 1.4, 1e-9);
    CHECK(b.channels[0].apps.at(0) == L"C:/Games/game.exe");
    CHECK(b.personalOutput.name == L"Headphones");
    CHECK_NEAR(b.mic.gain, 0.9f, 1e-6);
    CHECK(b.personalFailover.armed == true);
    CHECK(b.personalFailover.allow.at(0).name == L"Speakers");
    CHECK(b.personalFailover.stabilitySec == 7);
    CHECK(b.personalFailover.dwellSec == 45);
    CHECK(b.ui.taskbarButton == true);
    CHECK(b.autostart == true);
    CHECK(b.logLevel == 3);
}

MDXM_TEST_CASE(Config_GarbageFallsBackToDefaults) {
    // Review Focus #1: corrupt/truncated file must yield defaults, never a crash.
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    std::wstring file = std::wstring(path) + L"mdxm_bad.json";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr; WriteFile(h, "{\"channels\": [{\"id\": \"ga", 24, &wr, nullptr);
    CloseHandle(h);
    bool usedDefaults = false;
    MixerConfig c = LoadConfig(file, &usedDefaults);
    CHECK(usedDefaults);
    CHECK(c.channels.empty());
    CHECK(c.logLevel == 2);
    DeleteFileW(file.c_str());
}

MDXM_TEST_CASE(Config_MissingFileUsesDefaults) {
    bool usedDefaults = false;
    MixerConfig c = LoadConfig(L"Z:/definitely/not/here.json", &usedDefaults);
    CHECK(usedDefaults);
    CHECK(c.autostart == false);
}

MDXM_TEST_CASE(Config_LoadClampsHostileValues) {
    JsonValue root = JsonParse(LR"({"channels":[{"id":"x","personal":{"vol":7.5},"streaming":{"vol":-2.0}}]})");
    MixerConfig c = ConfigFromJson(root);
    CHECK_NEAR(c.channels.at(0).personal.vol, 1.0f, 1e-6);
    CHECK_NEAR(c.channels.at(0).streaming.vol, 0.0f, 1e-6);
}

MDXM_TEST_CASE(Config_AtomicSaveRoundTrips) {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    std::wstring file = std::wstring(path) + L"mdxm_save.json";
    CHECK(SaveConfigAtomic(file, SampleConfig()));
    bool usedDefaults = true;
    MixerConfig c = LoadConfig(file, &usedDefaults);
    CHECK(!usedDefaults);
    CHECK(c.channels.size() == 1);
    DeleteFileW(file.c_str());
}

MDXM_TEST_CASE(Config_DeviceFilingRoundTrips) {
    // A pin or a hide is worth as much as a name: the list has to come back the
    // way it was left, including for a device whose only entry is a hide.
    MixerConfig c;
    // id, btAddress, containerId, windowsName, alias, hidden, pinned.
    c.deviceNames.push_back({ L"{id-1}", L"8099e7813463", L"{ct-1}",
                              L"Headphones (XM5-1)", L"Black buds", false, true });
    c.deviceNames.push_back({ L"{id-2}", L"", L"{ct-2}",
                              L"Headset (XM5-2)", L"", true, false });
    JsonValue root = JsonParse(ConfigToJson(c));
    MixerConfig back = ConfigFromJson(root);
    CHECK(back.deviceNames.size() == 2);
    CHECK(back.deviceNames[0].alias == L"Black buds");
    CHECK(back.deviceNames[0].pinned && !back.deviceNames[0].hidden);
    CHECK(back.deviceNames[1].alias.empty());
    CHECK(back.deviceNames[1].hidden && !back.deviceNames[1].pinned);
}

MDXM_TEST_CASE(Config_FailoverSortViewSurvivesASave) {
    // A standing choice about how the allowlist reads, not a per-session one:
    // sorting by last seen and finding it back on preferred order next launch
    // is the kind of small friction that makes a panel feel unresponsive.
    MixerConfig c;
    c.ui.allowSort = 3;            // last seen
    c.ui.allowSortDesc = true;
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.ui.allowSort == 3);
    CHECK(back.ui.allowSortDesc);
}

MDXM_TEST_CASE(Config_SortViewIsClampedToTheFourItHas) {
    // A hand-edited or future value must not index past the combo.
    MixerConfig c;
    MixerConfig back = ConfigFromJson(JsonParse(
        L"{\"ui\":{\"allowSort\":99},\"complete\":true}"));
    CHECK(back.ui.allowSort >= 0 && back.ui.allowSort <= 3);
    back = ConfigFromJson(JsonParse(L"{\"ui\":{\"allowSort\":-4},\"complete\":true}"));
    CHECK(back.ui.allowSort >= 0 && back.ui.allowSort <= 3);
    (void)c;
}

MDXM_TEST_CASE(Config_ToolFontSizeIsNegativeAndClamped) {
    // A CreateFontW height. NEGATIVE is a character height; positive is a
    // CELL height, which is how the tool windows came to open at a 9-pixel
    // font that the +/- buttons could not even reach -- they clamp to
    // -12..-32 and 9 is outside it.
    MixerConfig fresh;
    CHECK(fresh.ui.toolFontSize == -20);          // mdx12's default

    MixerConfig c;
    c.ui.toolFontSize = -26;
    CHECK(ConfigFromJson(JsonParse(ConfigToJson(c))).ui.toolFontSize == -26);

    // A positive value in a hand-edited file is the exact failure this
    // clamp exists for, and must not reach CreateFontW.
    MixerConfig bad = ConfigFromJson(JsonParse(
        L"{\"ui\":{\"toolFontSize\":9},\"complete\":true}"));
    CHECK(bad.ui.toolFontSize <= -12 && bad.ui.toolFontSize >= -32);
    MixerConfig huge = ConfigFromJson(JsonParse(
        L"{\"ui\":{\"toolFontSize\":-400},\"complete\":true}"));
    CHECK(huge.ui.toolFontSize == -32);
}
