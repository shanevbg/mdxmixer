#include "test_framework.h"
#include "device/device_identity.h"
#include <windows.h>

using namespace mdxm;

MDXM_TEST_CASE(Alias_IdMatchWins) {
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id-1}", L"{c-1}", L"Headphones (2- WF-1000XM5-1)", L"Black buds");
    SetAlias(aliases, L"{id-2}", L"{c-2}", L"Headphones (2- WF-1000XM5-2)", L"Rose gold");
    CHECK(DisplayName(aliases, L"{id-2}", L"{c-2}", L"Headphones (2- WF-1000XM5-2)") == L"Rose gold");
    CHECK(aliases.size() == 2);
}

MDXM_TEST_CASE(Alias_FollowsContainerWhenTheEndpointIdChanges) {
    // The reported case: no re-pairing, but a new endpoint id because the other
    // earbud connected first. ContainerId is assigned per PHYSICAL device and
    // survives that, so it is the anchor — the endpoint id is not.
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id-old}", L"{container-A}", L"Headphones (2- WF-1000XM5-3)", L"Off white");
    CHECK(DisplayName(aliases, L"{id-NEW}", L"{container-A}", L"Headphones (2- WF-1000XM5-3)")
          == L"Off white");
    // Re-homed onto the new id, so the next lookup is exact.
    CHECK(aliases.size() == 1);
    CHECK(aliases[0].id == L"{id-NEW}");
}

MDXM_TEST_CASE(Alias_ContainerBeatsAMatchingName) {
    // Two physical devices can present the same Windows name. The container
    // decides, so an alias never jumps to the wrong pair of earbuds.
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id-1}", L"{container-A}", L"Headphones (2- WF-1000XM5)", L"Black buds");
    CHECK(DisplayName(aliases, L"{id-2}", L"{container-B}", L"Headphones (2- WF-1000XM5)")
          == L"Headphones (2- WF-1000XM5)");     // different device, no alias
    CHECK(DisplayName(aliases, L"{id-9}", L"{container-A}", L"Headphones (2- WF-1000XM5)")
          == L"Black buds");
}

MDXM_TEST_CASE(Alias_FallsBackToNameWhenNoContainer) {
    // Non-Bluetooth endpoints have no container to key on; the Windows name is
    // then the only stable handle, which is what MDropDX12 has always used.
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id-old}", L"", L"Speakers (Realtek)", L"Desk speakers");
    CHECK(DisplayName(aliases, L"{id-new}", L"", L"Speakers (Realtek)") == L"Desk speakers");
}

MDXM_TEST_CASE(Alias_UnknownDeviceKeepsWindowsName) {
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id-1}", L"{c-1}", L"Headphones (2- WF-1000XM5-1)", L"Black buds");
    CHECK(DisplayName(aliases, L"{other}", L"{c-9}", L"Speakers (Realtek)") == L"Speakers (Realtek)");
}

MDXM_TEST_CASE(Alias_ClearingRemovesTheEntry) {
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id-1}", L"{c-1}", L"Headphones (2- WF-1000XM5-1)", L"Black buds");
    SetAlias(aliases, L"{id-1}", L"{c-1}", L"Headphones (2- WF-1000XM5-1)", L"");
    CHECK(aliases.empty());
    CHECK(DisplayName(aliases, L"{id-1}", L"{c-1}", L"Headphones (2- WF-1000XM5-1)")
          == L"Headphones (2- WF-1000XM5-1)");
}

MDXM_TEST_CASE(Battery_FormatsOnlyWhenKnown) {
    CHECK(FormatBattery(-1).empty());     // no reading: an empty cell, not "0%"
    CHECK(FormatBattery(0) == L"0%");     // a real zero is worth showing
    CHECK(FormatBattery(85) == L"85%");
    CHECK(FormatBattery(100) == L"100%");
}

MDXM_TEST_CASE(HandsFree_MarksTheLowQualityEndpoint) {
    // Windows publishes one headset twice: "Headphones (X)" is A2DP stereo,
    // "Headset (X)" turns the mic on and drops to mono narrowband. Two rows
    // that differ by one word and cost a change of headsets to confuse.
    CHECK(IsHandsFreeName(L"Headset (2- WF-1000XM5-1 Hands-Free)"));
    CHECK(IsHandsFreeName(L"Headset (SR11 Hands-Free)"));
    // Measured on this machine: Windows also publishes the HFP endpoint as a
    // plain "Headset (...)" with no "Hands-Free" anywhere in the name, against
    // an A2DP twin called "Headphones (...)". The leading word is the real
    // discriminator, so match that too.
    CHECK(IsHandsFreeName(L"Headset (2- WF-1000XM5-2)"));
    CHECK(!IsHandsFreeName(L"Headphones (2- WF-1000XM5-1)"));
    CHECK(!IsHandsFreeName(L"Speakers (NVIDIA Broadcast)"));
    CHECK(!IsHandsFreeName(L"Microphone (Wireless Microphone)"));
}

MDXM_TEST_CASE(LastSeen_LocalFileTimeIsNotConvertedTwice) {
    // DEVPKEY_Bluetooth_LastConnectedTime is stored in LOCAL time. Running it
    // through FileTimeToLocalFileTime subtracts the offset a second time,
    // which is how two headsets once landed exactly one timezone apart.
    SYSTEMTIME st = {};
    st.wYear = 2026; st.wMonth = 9; st.wDay = 23;
    st.wHour = 15; st.wMinute = 30;
    FILETIME localFt = {};
    CHECK(SystemTimeToFileTime(&st, &localFt));
    uint64_t local = ((uint64_t)localFt.dwHighDateTime << 32) | localFt.dwLowDateTime;

    uint64_t utc = LocalFileTimeToUtc(local);
    // Converting back must land on the same wall-clock reading we started from.
    FILETIME utcFt = { (DWORD)(utc & 0xFFFFFFFF), (DWORD)(utc >> 32) };
    FILETIME backLocal = {};
    CHECK(FileTimeToLocalFileTime(&utcFt, &backLocal));
    SYSTEMTIME back = {};
    CHECK(FileTimeToSystemTime(&backLocal, &back));
    CHECK(back.wYear == 2026 && back.wMonth == 9 && back.wDay == 23);
    CHECK(back.wHour == 15 && back.wMinute == 30);

    CHECK(FormatLastSeen(0, false).empty());     // no record at all
    CHECK(!FormatLastSeen(utc, false).empty());
}

MDXM_TEST_CASE(LastSeen_ConnectedDeviceIgnoresAStaleStamp) {
    // Measured on this machine: an XM6 sitting at present=1 with battery=100
    // still reported LastConnectedTime 2025-10-13 — Windows simply never
    // refreshed the property. "Last seen: a year ago" for a device that is
    // connected right now is wrong however the timestamp is converted, so
    // presence wins over the stamp.
    SYSTEMTIME st = {};
    st.wYear = 2025; st.wMonth = 10; st.wDay = 13; st.wHour = 18; st.wMinute = 10;
    FILETIME ft = {};
    CHECK(SystemTimeToFileTime(&st, &ft));
    uint64_t stale = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    CHECK(FormatLastSeen(stale, true) == L"Connected");
    CHECK(FormatLastSeen(stale, false) != L"Connected");
    // A device with no stamp at all still reads as connected when it is.
    CHECK(FormatLastSeen(0, true) == L"Connected");
}

MDXM_TEST_CASE(View_PinAndHideSurviveTheEndpointIdChanging) {
    // Filing a device has to be anchored the same way its name is, or the pin
    // falls off the moment the other earbud connects first.
    std::vector<DeviceAlias> aliases;
    SetView(aliases, L"{id-old}", L"{container-A}", L"Headphones (XM5-3)", { false, true });
    DeviceView v = LookupView(aliases, L"{id-NEW}", L"{container-A}", L"Headphones (XM5-3)");
    CHECK(v.pinned);
    CHECK(!v.hidden);
    CHECK(aliases.size() == 1);
    // Reading the flags does not rewrite the entry — DisplayName owns re-homing,
    // and runs over the same device in the same pass. Writing does re-home.
    CHECK(aliases[0].id == L"{id-old}");
    SetView(aliases, L"{id-NEW}", L"{container-A}", L"Headphones (XM5-3)", { false, true });
    CHECK(aliases.size() == 1);
    CHECK(aliases[0].id == L"{id-NEW}");
}

MDXM_TEST_CASE(View_UnfiledDeviceIsVisibleAndUnpinned) {
    std::vector<DeviceAlias> aliases;
    DeviceView v = LookupView(aliases, L"{id}", L"{c}", L"Speakers (Realtek)");
    CHECK(!v.hidden && !v.pinned);
    CHECK(aliases.empty());              // a plain lookup must not create an entry
}

MDXM_TEST_CASE(View_AndNameShareOneEntryAndOutliveEachOther) {
    // Clearing the name of a hidden device must not un-hide it, and unhiding a
    // named device must not forget the name — they are two fields of one entry.
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", L"Black buds");
    SetView(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", { true, false });
    CHECK(aliases.size() == 1);

    SetAlias(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", L"");
    CHECK(aliases.size() == 1);          // still hidden, so the entry stays
    CHECK(LookupView(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)").hidden);

    SetView(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", { false, false });
    CHECK(aliases.empty());              // nothing left worth keeping
}

MDXM_TEST_CASE(View_UnhidingKeepsTheName) {
    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", L"Black buds");
    SetView(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", { true, false });
    SetView(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)", { false, false });
    CHECK(DisplayName(aliases, L"{id}", L"{c}", L"Headphones (XM5-1)") == L"Black buds");
}

MDXM_TEST_CASE(NullContainer_DoesNotMakeSixDevicesOne) {
    // Measured on this machine: all six SteelSeries Sonar virtual endpoints
    // report ContainerId {00000000-0000-0000-FFFF-FFFFFFFFFFFF}. Anchoring on
    // that made one rename apply to all six, and filed them as one device.
    const wchar_t* kNull = L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";
    CHECK(IsNullContainer(kNull));
    CHECK(IsNullContainer(L"{00000000-0000-0000-ffff-ffffffffffff}"));   // case
    CHECK(IsNullContainer(L""));
    CHECK(!IsNullContainer(L"{425849FD-FF19-5CAD-A637-63E90E64254B}"));

    std::vector<DeviceAlias> aliases;
    SetAlias(aliases, L"{sonar-aux}", kNull, L"SteelSeries Sonar - Aux", L"Aux");
    // The next Sonar endpoint carries the same placeholder and a different
    // name: it must not pick up the alias, and must not be filed with it.
    CHECK(DisplayName(aliases, L"{sonar-chat}", kNull, L"SteelSeries Sonar - Chat")
          == L"SteelSeries Sonar - Chat");
    SetView(aliases, L"{sonar-chat}", kNull, L"SteelSeries Sonar - Chat", { true, false });
    CHECK(!LookupView(aliases, L"{sonar-aux}", kNull, L"SteelSeries Sonar - Aux").hidden);
    CHECK(aliases.size() == 2);
}

MDXM_TEST_CASE(ShortName_TrimsTheDriverAndKeepsTheIdentity) {
    // Six Sonar endpoints differ by one word in sixty-three characters, so in
    // a column narrow enough to leave room for the rest of the row they are
    // the same string. Shane: "I can't read the rest of the names otherwise."
    CHECK(ShortDeviceName(L"SteelSeries Sonar - Chat (SteelSeries Sonar Virtual Audio Device)")
          == L"[SS] Chat");
    CHECK(ShortDeviceName(L"SteelSeries Sonar - Gaming (SteelSeries Sonar Virtual Audio Device)")
          == L"[SS] Gaming");
    CHECK(ShortDeviceName(L"CABLE Input (VB-Audio Virtual Cable)") == L"[VB] CABLE Input");
    CHECK(ShortDeviceName(L"CABLE Output (VB-Audio Virtual Cable)") == L"[VB] CABLE Output");

    // A headset is the opposite shape: the generic word leads and the part
    // that tells five identical pairs apart is inside the brackets. Trimming
    // there would throw away the only discriminator.
    CHECK(ShortDeviceName(L"Headphones (11- WF-1000XM5)") == L"Headphones (11- WF-1000XM5)");
    CHECK(ShortDeviceName(L"Speakers (NVIDIA Broadcast)") == L"Speakers (NVIDIA Broadcast)");
    CHECK(ShortDeviceName(L"") == L"");
}

// ── One headset, one name, however many pairings ─────────────────────────

MDXM_TEST_CASE(Alias_BluetoothAddressSurvivesANewContainerAndId) {
    // A change of ADAPTER re-pairs everything: new endpoint id, new
    // ContainerId, same headset. Keyed on the address, the name follows.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{id-old}", L"{ct-old}", L"Headphones (WF-1000XM6)", L"XM6",
             L"581862717758");
    CHECK(a.size() == 1);
    const std::wstring shown =
        DisplayName(a, L"{id-new}", L"{ct-new}", L"Headphones (3- WF-1000XM6)",
                    L"581862717758");
    CHECK(shown == L"XM6");
    // And it re-homes, so the next lookup matches on the current anchors too.
    CHECK(a.size() == 1);
    CHECK(a[0].id == L"{id-new}");
    CHECK(a[0].containerId == L"{ct-new}");
}

MDXM_TEST_CASE(Alias_PairingsOfOneHeadsetFoldIntoOneEntry) {
    // Shane has ONE Razer headset and three alias entries for it, one per
    // pairing, because the entries predate the address being recorded. The
    // first time each pairing is seen again the address is stamped on, and
    // that is when they become visible as duplicates.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{id-1}", L"{ct-1}", L"Headphones (Razer BlackShark V2 Pro (BT))",
             L"Razer BlackSharkV2 Pro");
    SetAlias(a, L"{id-2}", L"{ct-2}", L"Headphones (2- Razer BlackShark V2 Pro (BT))",
             L"RazerBlackShark RTK");
    CHECK(a.size() == 2);        // two pairings, two entries, no address yet

    // The same physical headset turns up, now reporting its address.
    DisplayName(a, L"{id-1}", L"{ct-1}", L"Headphones (Razer BlackShark V2 Pro (BT))",
                L"445ecd3b2b30");
    DisplayName(a, L"{id-2}", L"{ct-2}", L"Headphones (2- Razer BlackShark V2 Pro (BT))",
                L"445ecd3b2b30");
    CHECK(a.size() == 1);
    // The first name wins; the second pairing now answers to it.
    CHECK(a[0].alias == L"Razer BlackSharkV2 Pro");
    CHECK(DisplayName(a, L"{id-9}", L"{ct-9}", L"Headphones (9- Razer BlackShark V2 Pro (BT))",
                      L"445ecd3b2b30") == L"Razer BlackSharkV2 Pro");
}

MDXM_TEST_CASE(Alias_FoldKeepsAPinOrHideFromAnyPairing) {
    // Folding must not quietly drop a filing flag: the entry being merged
    // away may be the one carrying the pin.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{id-1}", L"{ct-1}", L"Headphones (X)", L"Buds");
    SetView(a, L"{id-2}", L"{ct-2}", L"Headphones (2- X)", { false, true });
    CHECK(a.size() == 2);
    DisplayName(a, L"{id-1}", L"{ct-1}", L"Headphones (X)", L"aabbccddeeff");
    DisplayName(a, L"{id-2}", L"{ct-2}", L"Headphones (2- X)", L"aabbccddeeff");
    CHECK(a.size() == 1);
    CHECK(a[0].alias == L"Buds");
    CHECK(a[0].pinned);
}

MDXM_TEST_CASE(Alias_DifferentHeadsetsAreNeverFolded) {
    // Two Sony sets share an OUI and a Windows name shape. Only the full
    // address may collapse them, and these differ.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{id-1}", L"{ct-1}", L"Headphones (WF-1000XM5)", L"White",
             L"8099e7813463");
    SetAlias(a, L"{id-2}", L"{ct-2}", L"Headphones (2- WF-1000XM5)", L"Rose Gold",
             L"8099e74165d3");
    CHECK(a.size() == 2);
    CHECK(DisplayName(a, L"{id-1}", L"{ct-1}", L"x", L"8099e7813463") == L"White");
    CHECK(DisplayName(a, L"{id-2}", L"{ct-2}", L"x", L"8099e74165d3") == L"Rose Gold");
}

MDXM_TEST_CASE(Alias_NonBluetoothDevicesStillUseTheOlderAnchors) {
    // An empty address must not match every other empty address, or every
    // speaker on the machine would share one name.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{spk-1}", L"{ct-1}", L"Speakers (NVIDIA Broadcast)", L"NV");
    SetAlias(a, L"{spk-2}", L"{ct-2}", L"Speakers (Realtek)", L"RT");
    CHECK(a.size() == 2);
    CHECK(DisplayName(a, L"{spk-1}", L"{ct-1}", L"Speakers (NVIDIA Broadcast)", L"") == L"NV");
    CHECK(DisplayName(a, L"{spk-2}", L"{ct-2}", L"Speakers (Realtek)", L"") == L"RT");
}

MDXM_TEST_CASE(Alias_AliasForNamesADeviceThatIsNotHere) {
    // A failover allowlist is mostly devices that are switched off, and an
    // entry whose endpoint Windows has forgotten has no live device to read a
    // name off. The store still knows it by the name it was recorded under.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{id-gone}", L"{ct-gone}", L"Headphones (WF-1000XM5-4)", L"XM5 Black #1");
    CHECK(AliasFor(a, L"{id-gone}", L"", L"Headphones (WF-1000XM5-4)") == L"XM5 Black #1");

    // An entry recorded with NO container is the one matched by name alone --
    // that is all an alias added while its device was switched off ever has.
    // An entry that does carry a container is deliberately NOT matched that
    // way: two physical devices can share a Windows name, and letting the
    // name win there would move a name onto the wrong pair of earbuds.
    std::vector<DeviceAlias> b;
    SetAlias(b, L"", L"", L"Headphones (WF-1000XM5-4)", L"XM5 Black #1");
    CHECK(AliasFor(b, L"", L"", L"Headphones (WF-1000XM5-4)") == L"XM5 Black #1");
    // Unknown devices come back as themselves, so a caller can print either.
    CHECK(AliasFor(a, L"{other}", L"", L"Speakers (Realtek)") == L"Speakers (Realtek)");
}

MDXM_TEST_CASE(Alias_AliasForNeverRehomesTheEntry) {
    // The read must not move the anchor. Re-homing an absent device onto an
    // id that no longer exists would destroy the one anchor still working --
    // and this runs on a timer, against every row, whether or not anyone is
    // looking.
    std::vector<DeviceAlias> a;
    SetAlias(a, L"{id-old}", L"{ct-old}", L"Headphones (X)", L"Buds");
    AliasFor(a, L"{id-new}", L"{ct-new}", L"Headphones (X)");
    CHECK(a.size() == 1);
    CHECK(a[0].id == L"{id-old}");
    CHECK(a[0].containerId == L"{ct-old}");
}
