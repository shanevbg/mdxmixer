// test_vban_config.cpp — the VBAN section of mdxmixer.json.
//
// The round trip is the part worth testing: a field written and not read back
// is a setting that silently resets on every restart, which presents as "it
// keeps forgetting" rather than as a missing line of code. The clamps are here
// because this file is hand-editable and the values reach a socket, a GPU
// capture and an audio gain -- a nonsense number should become a sane one at
// the door rather than at the thing it would break.
#include "test_framework.h"
#include "config/config.h"

using namespace mdxm;

MDXM_TEST_CASE(VbanConfig_Defaults) {
    MixerConfig c;
    // Off, and listening on the spec's port when it is turned on. Off matters:
    // this is the first thing in the program that opens an inbound socket.
    CHECK(!c.vban.enabled);
    CHECK(c.vban.port == 6980);
    CHECK(c.vban.streamName == L"mdxmixer");
    // Personal mix and int16 are the defaults the spec argues for: the monitor
    // clone rather than the programme mix, and the format every VBAN receiver
    // accepts.
    CHECK(!c.vban.sourceStreaming && !c.vban.formatFloat32);
    CHECK(c.vban.gainPercent == 100);
    // No PIN and no authorized devices: remote control is off until somebody
    // sets a PIN, and nothing is trusted until it has been approved once.
    CHECK(c.vban.pin.empty() && c.vban.authorizedDevices.empty());
    CHECK(!c.vban.openSubscribe && !c.vban.alwaysStream && !c.vban.alwaysFrames);
    CHECK(c.vban.frames.fps == 2.0 && c.vban.frames.quality == 60 &&
          c.vban.frames.maxEdge == 480);
}

MDXM_TEST_CASE(VbanConfig_RoundTrip) {
    MixerConfig c;
    c.vban.enabled = true;
    c.vban.port = 7000;
    c.vban.bindAddress = L"192.168.0.10";
    c.vban.streamName = L"rig";
    c.vban.sourceStreaming = true;
    c.vban.formatFloat32 = true;
    c.vban.gainPercent = 1600;
    c.vban.pin = L"4242";
    c.vban.authorizedDevices.push_back({ L"androidid1", L"Pixel 9", L"" });
    c.vban.openSubscribe = true;
    c.vban.alwaysStream = true;
    c.vban.alwaysFrames = true;
    c.vban.alwaysStreamTarget = L"192.168.0.77:6980";
    c.vban.frames = { 5.0, 80, 320 };
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.vban.enabled && back.vban.port == 7000);
    CHECK(back.vban.bindAddress == L"192.168.0.10");
    CHECK(back.vban.streamName == L"rig");
    CHECK(back.vban.sourceStreaming && back.vban.formatFloat32);
    CHECK(back.vban.gainPercent == 1600);
    CHECK(back.vban.pin == L"4242");
    CHECK(back.vban.authorizedDevices.size() == 1);
    CHECK(back.vban.authorizedDevices[0].id == L"androidid1");
    CHECK(back.vban.authorizedDevices[0].name == L"Pixel 9");
    CHECK(back.vban.openSubscribe && back.vban.alwaysStream && back.vban.alwaysFrames);
    CHECK(back.vban.alwaysStreamTarget == L"192.168.0.77:6980");
    CHECK(back.vban.frames.fps == 5.0 && back.vban.frames.quality == 80 &&
          back.vban.frames.maxEdge == 320);
}

MDXM_TEST_CASE(VbanConfig_Clamps) {
    MixerConfig c;
    c.vban.gainPercent = 999999;
    c.vban.port = -5;
    c.vban.frames.fps = 100.0;
    c.vban.frames.quality = 999;
    c.vban.frames.maxEdge = 9999;
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.vban.gainPercent == 6400);           // 0..6400 (spec §6.1)
    CHECK(back.vban.port == 6980);                  // nonsense port -> the default
    CHECK(back.vban.frames.fps == 10.0);            // 0.2..10
    CHECK(back.vban.frames.quality == 95);          // 10..95, a usable JPEG range
    CHECK(back.vban.frames.maxEdge == 1024);        // 64..1024
}

MDXM_TEST_CASE(VbanConfig_AbsentSectionKeepsDefaults) {
    // Every config written before this version has no "vban" key at all, and a
    // missing section must read as the defaults rather than as zeros -- a port
    // of 0 or an empty stream name would be a listener nobody could reach.
    const MixerConfig c = ConfigFromJson(JsonParse(LR"({"logLevel":2})"));
    CHECK(!c.vban.enabled);
    CHECK(c.vban.port == 6980);
    CHECK(c.vban.streamName == L"mdxmixer");
    CHECK(c.vban.gainPercent == 100);
    CHECK(c.vban.frames.fps == 2.0);
    CHECK(c.vban.frames.maxEdge == 480);
}

MDXM_TEST_CASE(VbanAuthorizedDevices_UpsertIsIdempotentAndRenames) {
    // Approving a device twice must not list it twice -- the Revoke button shows
    // this list, and two rows for one phone is both confusing and a trap (which
    // one does Revoke remove?). A phone that was renamed keeps its single row.
    std::vector<VbanAuthorizedDevice> list;
    CHECK(UpsertAuthorizedDevice(list, L"id1", L"Pixel 9"));
    CHECK(list.size() == 1);
    CHECK(!UpsertAuthorizedDevice(list, L"id1", L"Pixel 9"));   // nothing changed
    CHECK(list.size() == 1);
    CHECK(UpsertAuthorizedDevice(list, L"id1", L"Shane's Pixel"));   // renamed
    CHECK(list.size() == 1);
    CHECK(list[0].name == L"Shane's Pixel");
    CHECK(UpsertAuthorizedDevice(list, L"id2", L"Tablet"));
    CHECK(list.size() == 2);
    // An entry with no id can never be matched to a device, so it is refused
    // rather than stored unreachable.
    CHECK(!UpsertAuthorizedDevice(list, L"", L"Ghost"));
    CHECK(list.size() == 2);
}

MDXM_TEST_CASE(VbanAuthorizedDevices_LastSeenStampsAndUpdates) {
    // The approval timestamp: a caller that knows the date stamps it; one that
    // does not (empty) leaves whatever was stored alone, so passing no date is
    // never mistaken for clearing it.
    std::vector<VbanAuthorizedDevice> list;
    CHECK(UpsertAuthorizedDevice(list, L"id1", L"Pixel", L"2026-10-08 14:30"));
    CHECK(list.size() == 1 && list[0].lastSeen == L"2026-10-08 14:30");
    // Same name, no date: nothing changes.
    CHECK(!UpsertAuthorizedDevice(list, L"id1", L"Pixel"));
    CHECK(list[0].lastSeen == L"2026-10-08 14:30");
    // A newer connection restamps it, even with the same name.
    CHECK(UpsertAuthorizedDevice(list, L"id1", L"Pixel", L"2026-10-09 09:00"));
    CHECK(list[0].lastSeen == L"2026-10-09 09:00");
    // It round-trips through the config.
    MixerConfig c;
    c.vban.authorizedDevices = list;
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.vban.authorizedDevices.size() == 1);
    CHECK(back.vban.authorizedDevices[0].lastSeen == L"2026-10-09 09:00");
}

MDXM_TEST_CASE(VbanAuthorizedDevices_RemoveTakesExactlyOne) {
    std::vector<VbanAuthorizedDevice> list;
    UpsertAuthorizedDevice(list, L"id1", L"A");
    UpsertAuthorizedDevice(list, L"id2", L"B");
    CHECK(!RemoveAuthorizedDevice(list, L"nope"));      // says so rather than silently
    CHECK(list.size() == 2);
    CHECK(RemoveAuthorizedDevice(list, L"id1"));
    CHECK(list.size() == 1 && list[0].id == L"id2");
    CHECK(!RemoveAuthorizedDevice(list, L"id1"));       // and it stays gone
}

MDXM_TEST_CASE(VbanConfig_AuthorizedDeviceWithoutIdIsDropped) {
    // The id is what an approval is recorded against, so an entry without one
    // can never match a device and would sit in the list for ever, unexplained.
    const MixerConfig c = ConfigFromJson(JsonParse(
        LR"({"vban":{"authorizedDevices":[{"id":"","name":"ghost"},
                                          {"id":"real","name":"Pixel 9"}]}})"));
    CHECK(c.vban.authorizedDevices.size() == 1);
    CHECK(c.vban.authorizedDevices[0].id == L"real");
}
