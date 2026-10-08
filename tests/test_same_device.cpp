// test_same_device.cpp — one rule for "is this stored reference this device?",
// shared by the failover engine and the list that draws it.
//
// THE BUG THIS PINS, from Shane's machine on 2026-10-05. His failover list had
// 21 entries and the list box showed a battery and a date for exactly ONE of
// them: "According to list box has only seen one device ever and that was
// yesterday". Everything else read as unknown -- raw Windows name, empty cells
// -- and the tab offered to delete all twenty.
//
// Nothing was actually missing. The endpoints had been removed and re-created
// ("These all were removed and replaced with different endpoints"), so every
// stored id but one was dead, while the engine went on matching those same
// entries BY NAME and failed over onto them correctly. Two rules for one list:
// the engine's forgiving one, and the tab's id-only one driving a delete
// button.
#include "test_framework.h"
#include "device/device_identity.h"

using namespace mdxm;

namespace {
// The entry that saved him, and the endpoint it saved him onto. Same headset,
// different id: stored when Windows called it 12- under one pairing, matched
// when it came back under another.
const DeviceRefKeys kStored{ L"{0.0.0.00000000}.{...04c198f}",
                             L"Headphones (12- WF-1000XM5)", L"" };
const DeviceRefKeys kLive{ L"{0.0.0.00000000}.{1e165427-47d9-4abb-a056-3eeb8499fb7e}",
                           L"Headphones (12- WF-1000XM5)", L"Bk2" };
} // namespace

MDXM_TEST_CASE(SameDevice_MatchesTheIdWhenTheIdIsStillGood) {
    const DeviceRefKeys ref{ L"{id-a}", L"Headphones (14- WF-1000XM5)", L"" };
    const DeviceRefKeys dev{ L"{id-a}", L"Headphones (14- WF-1000XM5)", L"Rg2" };
    CHECK(SameDevice(ref, dev));
}

// The whole point: a dead id must not make a live entry unknown.
MDXM_TEST_CASE(SameDevice_SurvivesTheIdBeingReplaced) {
    CHECK(SameDevice(kStored, kLive));
}

// An entry added while its headset was switched off has no id at all. That was
// already handled before, and must stay handled.
MDXM_TEST_CASE(SameDevice_MatchesByNameWithNoIdAtAll) {
    const DeviceRefKeys ref{ L"", L"Headphones (3- WF-1000XM6)", L"" };
    const DeviceRefKeys dev{ L"{id-b}", L"Headphones (3- WF-1000XM6)", L"XM6" };
    CHECK(SameDevice(ref, dev));
}

// The case pure name matching cannot reach: Windows bumped the prefix, so the
// stored name is gone too. Only our own alias, which DisplayName re-homes
// through btAddress and container, still knows them for the same earbuds --
// "WMX5 White" came back as "Headphones (9- WF-1000XM5)" within the hour.
MDXM_TEST_CASE(SameDevice_MatchesTheAliasWhenIdAndWindowsNameBothMoved) {
    const DeviceRefKeys ref{ L"{old}", L"WMX5 White", L"Wht" };
    const DeviceRefKeys dev{ L"{new}", L"Headphones (9- WF-1000XM5)", L"Wht" };
    CHECK(SameDevice(ref, dev));
}

// ── and the half that matters just as much: NOT matching ─────────────────
//
// Five pairs of identical earbuds. A rule loose enough to file two of them
// together would point failover at the wrong headset, which is worse than an
// unknown row.
MDXM_TEST_CASE(SameDevice_DoesNotMatchTwoDifferentHeadsets) {
    const DeviceRefKeys ref{ L"{id-a}", L"Headphones (10- WF-1000XM5)", L"Rg1" };
    const DeviceRefKeys dev{ L"{id-b}", L"Headphones (11- WF-1000XM5)", L"Bk1" };
    CHECK(!SameDevice(ref, dev));
}

// Empty is not a match. Most of his saved entries carry an alias and nothing
// else, or an id and nothing else; a blank field on either side must never be
// what makes two rows the same device -- that would collapse the whole list
// onto whichever row came first.
MDXM_TEST_CASE(SameDevice_EmptyFieldsNeverMatch) {
    CHECK(!SameDevice({ L"", L"", L"" }, { L"", L"", L"" }));
    CHECK(!SameDevice({ L"", L"", L"" }, { L"{id}", L"Headphones", L"Bk1" }));
    CHECK(!SameDevice({ L"{id}", L"Headphones", L"Bk1" }, { L"", L"", L"" }));
    // One side missing an alias is not an alias match.
    CHECK(!SameDevice({ L"{a}", L"One", L"" }, { L"{b}", L"Two", L"Bk1" }));
}

// Exactly, not nearly. Windows hands both sides the same string, and five
// headsets whose names differ by one character are the thing being told apart.
MDXM_TEST_CASE(SameDevice_NameComparisonIsExact) {
    CHECK(!SameDevice({ L"", L"headphones (10- wf-1000xm5)", L"" },
                      { L"{id}", L"Headphones (10- WF-1000XM5)", L"" }));
}

// ── duplicates ───────────────────────────────────────────────────────────
//
// "deduplicate devices please". His list held 21 entries for seven physical
// headsets: each re-pairing was added beside the entry it replaced, and until
// SameDevice the two did not look alike -- one resolved to "Rg1", the other
// was drawn under whatever Windows called it that week.

namespace {
bool Dropped(const std::vector<size_t>& v, size_t i) {
    for (size_t x : v) if (x == i) return true;
    return false;
}
} // namespace

MDXM_TEST_CASE(DuplicateRefs_FindsTwoEntriesForOneHeadset) {
    const std::vector<DeviceRefKeys> refs{
        { L"{old}", L"Headphones (2- WF-1000XM5-2)", L"Rg1" },
        { L"{new}", L"Headphones (10- WF-1000XM5)", L"Rg1" },
        { L"{xm6}", L"Headphones (3- WF-1000XM6)", L"XM6" },
    };
    const auto drop = DuplicateRefs(refs);
    CHECK(drop.size() == 1);
    CHECK(Dropped(drop, 1));      // the LATER one goes
}

// The order of an allowlist is the preference -- "first present entry wins" --
// so dropping the earlier entry would silently demote a device the user had
// deliberately put at the top.
MDXM_TEST_CASE(DuplicateRefs_KeepsTheEarliestEvenWhenItIsWorseAnchored) {
    const std::vector<DeviceRefKeys> refs{
        { L"", L"", L"Bk1" },                                  // alias only
        { L"{live}", L"Headphones (11- WF-1000XM5)", L"Bk1" }, // fully anchored
    };
    const auto drop = DuplicateRefs(refs);
    CHECK(drop.size() == 1);
    CHECK(Dropped(drop, 1));
    CHECK(!Dropped(drop, 0));
}

MDXM_TEST_CASE(DuplicateRefs_LeavesDistinctDevicesAlone) {
    const std::vector<DeviceRefKeys> refs{
        { L"{a}", L"Headphones (10- WF-1000XM5)", L"Rg1" },
        { L"{b}", L"Headphones (11- WF-1000XM5)", L"Bk1" },
        { L"{c}", L"Headphones (3- WF-1000XM6)", L"XM6" },
        { L"{d}", L"Headphones (3- Razer BlackShark V2 Pro (BT))", L"Rzr" },
    };
    CHECK(DuplicateRefs(refs).empty());
}

// Five identical pairs of earbuds and no names yet: entries with nothing in
// common but emptiness must not collapse onto each other, which would delete
// four headsets from the list.
MDXM_TEST_CASE(DuplicateRefs_EmptyEntriesAreNotAllTheSameDevice) {
    const std::vector<DeviceRefKeys> refs{
        { L"{a}", L"", L"" },
        { L"{b}", L"", L"" },
        { L"{c}", L"", L"" },
    };
    CHECK(DuplicateRefs(refs).empty());
}

// SameDevice is not transitive: A and B share an alias, B and C share a name,
// A and C share nothing. Comparing a candidate only against KEPT entries stops
// one junk row pulling two unrelated devices together.
MDXM_TEST_CASE(DuplicateRefs_DoesNotChainThroughADroppedEntry) {
    const std::vector<DeviceRefKeys> refs{
        { L"{a}", L"Name-A", L"Shared" },
        { L"{b}", L"Name-C", L"Shared" },   // duplicate of A, by alias
        { L"{c}", L"Name-C", L"Other" },    // shares a NAME with B, which is gone
    };
    const auto drop = DuplicateRefs(refs);
    CHECK(drop.size() == 1);
    CHECK(Dropped(drop, 1));
    CHECK(!Dropped(drop, 2));               // kept: it is not A
}

MDXM_TEST_CASE(DuplicateRefs_IndicesComeBackAscending) {
    const std::vector<DeviceRefKeys> refs{
        { L"{a}", L"One", L"Rg1" },
        { L"{b}", L"Two", L"Rg1" },
        { L"{c}", L"Three", L"Rg1" },
        { L"{d}", L"Four", L"XM6" },
    };
    const auto drop = DuplicateRefs(refs);
    CHECK(drop.size() == 2);
    CHECK(drop[0] == 1);
    CHECK(drop[1] == 2);
}

MDXM_TEST_CASE(DuplicateRefs_EmptyListIsNoWork) {
    CHECK(DuplicateRefs({}).empty());
}

// ── the anchor that survives a new adapter ───────────────────────────────
//
// "I would prefer to not have to guess every time I change out the bluetooth
// adapter hoping for a better and more stable connection." A new dongle
// re-pairs every headset: new endpoint id, new ContainerId, and Windows bumps
// the name too. The headset's own address is the only thing left standing.

MDXM_TEST_CASE(SameDevice_TheBluetoothAddressOutlivesEverythingElse) {
    const DeviceRefKeys stored{ L"{old-id}", L"Headphones (11- WF-1000XM5)", L"", L"ac800aaade78" };
    const DeviceRefKeys live{ L"{new-id}", L"Headphones (12- WF-1000XM5)", L"Bk1", L"ac800aaade78" };
    CHECK(SameDevice(stored, live));   // nothing else matches, and it is the same earbuds
}

// A STRONG ANCHOR THAT DISAGREES OUTRANKS A WEAK ONE THAT AGREES, and this is
// the case that proves it is needed rather than tidy. Windows reuses names:
// "Headphones (11- WF-1000XM5)" belongs to whichever of the five identical
// pairs was paired eleventh, and a later pairing of a DIFFERENT pair inherits
// it. Matching those two on the shared name would aim failover at the wrong
// earbuds -- and this test failed when the ladder was positive-only.
MDXM_TEST_CASE(SameDevice_DifferentAddressesAreDifferentHeadsets) {
    const DeviceRefKeys a{ L"", L"Headphones (11- WF-1000XM5)", L"", L"ac800aaade78" };
    const DeviceRefKeys b{ L"", L"Headphones (11- WF-1000XM5)", L"", L"ac800a294655" };
    CHECK(!SameDevice(a, b));
}

// AN UN-NAMED ENTRY HAS NO ALIAS, NOT AN ALIAS THAT LOOKS LIKE ITS NAME.
//
// AliasFor and DisplayName both fall back to the Windows name, which is right
// for a list that must put something in the column and wrong for matching.
// Passing that fallback in as an "alias" gave entry 18 of Shane's failover
// list the alias "Headphones (12- WF-1000XM5)", compared it against the live
// device's real alias "Bk2", and ruled out a match on the one headset he was
// listening through -- while the log cheerfully reported entries healed.
// AliasOrNone is what both sides must go through.
MDXM_TEST_CASE(AliasOrNone_TreatsTheWindowsNameAsNoAliasAtAll) {
    CHECK(AliasOrNone(L"Headphones (12- WF-1000XM5)", L"Headphones (12- WF-1000XM5)").empty());
    CHECK(AliasOrNone(L"", L"Headphones (12- WF-1000XM5)").empty());
    CHECK(AliasOrNone(L"Bk2", L"Headphones (12- WF-1000XM5)") == L"Bk2");
}

MDXM_TEST_CASE(SameDevice_AnUnnamedEntryStillMatchesANamedDevice) {
    const std::wstring name = L"Headphones (12- WF-1000XM5)";
    const DeviceRefKeys stored{ L"{dead-id}", name, AliasOrNone(name, name), L"" };
    const DeviceRefKeys live{ L"{live-id}", name, AliasOrNone(L"Bk2", name), L"ac800a294655" };
    CHECK(SameDevice(stored, live));
}

MDXM_TEST_CASE(SameDevice_DifferentAliasesAreDifferentHeadsets) {
    // He named them apart on purpose. A stale shared Windows name must not
    // put Bk1's entry onto Bk2.
    const DeviceRefKeys a{ L"{x}", L"Headphones (11- WF-1000XM5)", L"Bk1", L"" };
    const DeviceRefKeys b{ L"{y}", L"Headphones (11- WF-1000XM5)", L"Bk2", L"" };
    CHECK(!SameDevice(a, b));
}

// ...but a DIFFERENT ID never rules anything out, because an id changing under
// one device is the entire problem this function was written for.
MDXM_TEST_CASE(SameDevice_ADifferentIdIsNotADisqualifier) {
    const DeviceRefKeys a{ L"{old}", L"Headphones (11- WF-1000XM5)", L"", L"ac800aaade78" };
    const DeviceRefKeys b{ L"{new}", L"Headphones (12- WF-1000XM5)", L"", L"ac800aaade78" };
    CHECK(SameDevice(a, b));
}

MDXM_TEST_CASE(SameDevice_AnEmptyAddressDecidesNothing) {
    // Everything that is not on Bluetooth has no address at all, and two
    // cables sharing "no address" are not one device.
    const DeviceRefKeys a{ L"{cable-in}", L"CABLE Input (VB-Audio Virtual Cable)", L"", L"" };
    const DeviceRefKeys b{ L"{cable-16}", L"CABLE In 16ch (VB-Audio Virtual Cable)", L"", L"" };
    CHECK(!SameDevice(a, b));
}

MDXM_TEST_CASE(HealDeviceRef_TakesTheAddressTheFirstTimeItMeetsTheDevice) {
    DeviceRefKeys stored{ L"{old}", L"Headphones (11- WF-1000XM5)", L"", L"" };
    const DeviceRefKeys live{ L"{new}", L"Headphones (12- WF-1000XM5)", L"Bk1", L"ac800aaade78" };
    CHECK(HealDeviceRef(stored, live));
    CHECK(stored.btAddress == L"ac800aaade78");
    CHECK(stored.id == L"{new}");
    CHECK(stored.name == L"Headphones (12- WF-1000XM5)");
}

MDXM_TEST_CASE(HealDeviceRef_ReportsNoChangeWhenTheEntryIsAlreadyRight) {
    // It runs on every sweep. Saying "nothing changed" is what keeps it from
    // writing the config file four times a second.
    DeviceRefKeys stored{ L"{id}", L"Headphones (12- WF-1000XM5)", L"", L"ac800aaade78" };
    const DeviceRefKeys live{ L"{id}", L"Headphones (12- WF-1000XM5)", L"Bk2", L"ac800aaade78" };
    CHECK(!HealDeviceRef(stored, live));
}

MDXM_TEST_CASE(HealDeviceRef_KeepsWhatTheDeviceCannotTell) {
    // Matched while the headset is switched off: no address to learn, and the
    // last known id and name are better than blanks.
    DeviceRefKeys stored{ L"{known}", L"Headphones (12- WF-1000XM5)", L"", L"ac800a294655" };
    const DeviceRefKeys live{ L"", L"", L"Bk2", L"" };
    CHECK(!HealDeviceRef(stored, live));
    CHECK(stored.id == L"{known}");
    CHECK(stored.name == L"Headphones (12- WF-1000XM5)");
    CHECK(stored.btAddress == L"ac800a294655");
}

MDXM_TEST_CASE(HealDeviceRef_NeverOverwritesAnAddressItAlreadyHas) {
    DeviceRefKeys stored{ L"", L"", L"", L"ac800aaade78" };
    const DeviceRefKeys live{ L"{x}", L"Headphones", L"Bk1", L"8099e7813463" };
    HealDeviceRef(stored, live);
    CHECK(stored.btAddress == L"ac800aaade78");
}

// The sequence this is all for: an entry saved under one adapter, found again
// under the next one, still matching.
MDXM_TEST_CASE(HealDeviceRef_SurvivesTheAdapterSwapItWasWrittenFor) {
    DeviceRefKeys stored{ L"{realtek-era-id}", L"Headphones (11- WF-1000XM5)", L"", L"" };
    const DeviceRefKeys beforeSwap{ L"{realtek-era-id}", L"Headphones (11- WF-1000XM5)",
                                    L"Bk1", L"ac800aaade78" };
    HealDeviceRef(stored, beforeSwap);

    // New dongle: everything is re-paired, so the id and the name both move.
    const DeviceRefKeys afterSwap{ L"{barrot-era-id}", L"Headphones (4- WF-1000XM5)",
                                   L"Bk1", L"ac800aaade78" };
    CHECK(SameDevice(stored, afterSwap));
    CHECK(HealDeviceRef(stored, afterSwap));
    CHECK(stored.id == L"{barrot-era-id}");
}
