#include "device_identity.h"
#include <windows.h>
#include <algorithm>

namespace mdxm {

bool IsNullContainer(const std::wstring& containerId) {
    if (containerId.empty()) return true;
    return _wcsicmp(containerId.c_str(), L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}") == 0;
}

namespace {
bool SameName(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}
} // namespace

namespace {
constexpr size_t kNoEntry = (size_t)-1;

// The entry anchored on this PAIRING: container, then id, then name.
//
// A device that HAS a container is never matched by name: two physical
// devices can share a Windows name, and letting the name win there would move
// an alias onto the wrong pair of earbuds.
size_t MatchPairing(const std::vector<DeviceAlias>& aliases, const std::wstring& id,
                    const std::wstring& rawContainerId, const std::wstring& windowsName) {
    // The placeholder container matches nothing: it is shared by every device
    // that has none, so anchoring on it would make all six Sonar endpoints
    // the same device.
    const std::wstring containerId = IsNullContainer(rawContainerId) ? L"" : rawContainerId;
    if (!containerId.empty()) {
        for (size_t i = 0; i < aliases.size(); ++i)
            if (!aliases[i].containerId.empty() && aliases[i].containerId == containerId)
                return i;
    }
    for (size_t i = 0; i < aliases.size(); ++i)
        if (!aliases[i].id.empty() && aliases[i].id == id) return i;
    if (containerId.empty()) {
        for (size_t i = 0; i < aliases.size(); ++i)
            if (aliases[i].containerId.empty() &&
                SameName(aliases[i].windowsName, windowsName)) return i;
    }
    return kNoEntry;
}

// The entry anchored on the DEVICE itself.
size_t MatchAddress(const std::vector<DeviceAlias>& aliases, const std::wstring& btAddress) {
    if (btAddress.empty()) return kNoEntry;
    for (size_t i = 0; i < aliases.size(); ++i)
        if (!aliases[i].btAddress.empty() && aliases[i].btAddress == btAddress) return i;
    return kNoEntry;
}

// Merge `from` into `keep` and erase it. The first NAME wins; a pin or a hide
// held by either is kept, because the entry being merged away may be the one
// carrying it.
void Absorb(std::vector<DeviceAlias>& aliases, size_t keep, size_t from) {
    if (keep == from || keep >= aliases.size() || from >= aliases.size()) return;
    if (aliases[keep].alias.empty()) aliases[keep].alias = aliases[from].alias;
    aliases[keep].hidden = aliases[keep].hidden || aliases[from].hidden;
    aliases[keep].pinned = aliases[keep].pinned || aliases[from].pinned;
    aliases.erase(aliases.begin() + (ptrdiff_t)from);
}

// Resolve this endpoint to ONE entry, folding away any duplicate it reveals.
//
// Both anchors are looked up, not just the stronger one. That is the whole
// mechanism: entries written before the address was recorded are keyed on a
// container that a re-pair has already invalidated, so one headset
// accumulates an entry per pairing -- one Razer headset, three rows. When a
// pairing turns up carrying an address that ANOTHER entry already holds, the
// two are the same device and are merged on the spot.
//
// Looking up only the address would never find the stale sibling, and looking
// up only the pairing would never notice they are the same device.
size_t Resolve(std::vector<DeviceAlias>& aliases, const std::wstring& id,
               const std::wstring& rawContainerId, const std::wstring& windowsName,
               const std::wstring& btAddress, bool stamp) {
    size_t byAddr = MatchAddress(aliases, btAddress);
    size_t byPair = MatchPairing(aliases, id, rawContainerId, windowsName);

    if (byAddr != kNoEntry && byPair != kNoEntry && byAddr != byPair) {
        Absorb(aliases, byAddr, byPair);
        if (byPair < byAddr) --byAddr;      // the erase shifted it
        return byAddr;
    }
    const size_t found = (byAddr != kNoEntry) ? byAddr : byPair;
    // Stamped the first time we see it, which is what lets the NEXT pairing
    // of the same headset be recognised at all.
    if (stamp && found != kNoEntry && !btAddress.empty())
        aliases[found].btAddress = btAddress;
    return found;
}

DeviceAlias* Match(std::vector<DeviceAlias>& aliases, const std::wstring& id,
                   const std::wstring& rawContainerId, const std::wstring& windowsName,
                   const std::wstring& btAddress = {}) {
    const size_t i = Resolve(aliases, id, rawContainerId, windowsName, btAddress, false);
    return i == kNoEntry ? nullptr : &aliases[i];
}
} // namespace

namespace {
// An entry that holds nothing — no name, not pinned, not hidden — is litter.
void DropIfEmpty(std::vector<DeviceAlias>& aliases, DeviceAlias* entry) {
    if (entry->alias.empty() && !entry->hidden && !entry->pinned)
        aliases.erase(aliases.begin() + (entry - aliases.data()));
}

DeviceAlias* Ensure(std::vector<DeviceAlias>& aliases, const std::wstring& id,
                    const std::wstring& rawContainerId, const std::wstring& windowsName,
                    const std::wstring& btAddress = {}) {
    const std::wstring containerId = IsNullContainer(rawContainerId) ? L"" : rawContainerId;
    const size_t i = Resolve(aliases, id, containerId, windowsName, btAddress, true);
    if (i == kNoEntry) {
        aliases.push_back({ id, btAddress, containerId, windowsName, L"" });
        return &aliases.back();
    }
    aliases[i].id = id;                // re-home onto whatever id it carries now
    aliases[i].containerId = containerId;
    aliases[i].windowsName = windowsName;
    return &aliases[i];
}
} // namespace

void SetAlias(std::vector<DeviceAlias>& aliases, const std::wstring& id,
              const std::wstring& containerId, const std::wstring& windowsName,
              const std::wstring& alias, const std::wstring& btAddress) {
    if (alias.empty() && !Match(aliases, id, containerId, windowsName, btAddress)) return;
    DeviceAlias* entry = Ensure(aliases, id, containerId, windowsName, btAddress);
    entry->alias = alias;
    DropIfEmpty(aliases, entry);       // clearing a name leaves no litter behind
}

DeviceView LookupView(std::vector<DeviceAlias>& aliases, const std::wstring& id,
                      const std::wstring& containerId, const std::wstring& windowsName,
                      const std::wstring& btAddress) {
    const DeviceAlias* found = Match(aliases, id, containerId, windowsName, btAddress);
    if (!found) return {};
    return { found->hidden, found->pinned };
}

void SetView(std::vector<DeviceAlias>& aliases, const std::wstring& id,
             const std::wstring& containerId, const std::wstring& windowsName,
             DeviceView view, const std::wstring& btAddress) {
    if (!view.hidden && !view.pinned &&
        !Match(aliases, id, containerId, windowsName, btAddress)) return;
    DeviceAlias* entry = Ensure(aliases, id, containerId, windowsName, btAddress);
    entry->hidden = view.hidden;
    entry->pinned = view.pinned;
    DropIfEmpty(aliases, entry);
}

std::wstring DisplayName(std::vector<DeviceAlias>& aliases, const std::wstring& id,
                         const std::wstring& containerId, const std::wstring& windowsName,
                         const std::wstring& btAddress) {
    // `stamp`: a plain read is also where the address is first recorded and
    // where duplicates are folded, because reading the list is the thing that
    // happens on every tick -- waiting for a rename would mean the fold never
    // ran for a device nobody renames.
    const size_t i = Resolve(aliases, id, containerId, windowsName, btAddress, true);
    if (i == kNoEntry) return windowsName;
    aliases[i].id = id;                // the id moves; the alias follows the device
    if (!IsNullContainer(containerId)) aliases[i].containerId = containerId;
    return aliases[i].alias;
}

std::wstring AliasFor(const std::vector<DeviceAlias>& aliases, const std::wstring& id,
                      const std::wstring& containerId, const std::wstring& windowsName,
                      const std::wstring& btAddress) {
    // A copy, so the read cannot re-home, stamp or fold anything. The cost is
    // one vector copy on a list that holds a couple of dozen short strings,
    // and the alternative is a second matcher that can drift from this one.
    std::vector<DeviceAlias> scratch = aliases;
    const size_t i = Resolve(scratch, id, containerId, windowsName, btAddress, false);
    if (i == kNoEntry || scratch[i].alias.empty()) return windowsName;
    return scratch[i].alias;
}

bool IsProviderOwnedEndpoint(const std::wstring& windowsName) {
    static const wchar_t* const kPrefixes[] = { L"SteelSeries Sonar - " };
    for (const wchar_t* p : kPrefixes)
        if (windowsName.compare(0, wcslen(p), p) == 0) return true;
    return false;
}

std::wstring ShortDeviceName(const std::wstring& windowsName) {
    // The driver's own name in brackets carries no information once the
    // prefix has been read: every Sonar endpoint ends the same way, and so
    // does every cable.
    struct Rule { const wchar_t* prefix; const wchar_t* tag; };
    static const Rule kRules[] = {
        { L"SteelSeries Sonar - ", L"[SS] " },
    };
    for (const Rule& r : kRules) {
        const size_t plen = wcslen(r.prefix);
        if (windowsName.compare(0, plen, r.prefix) == 0) {
            std::wstring rest = windowsName.substr(plen);
            const size_t paren = rest.find(L" (");
            if (paren != std::wstring::npos) rest = rest.substr(0, paren);
            return std::wstring(r.tag) + rest;
        }
    }
    // A cable: the bracketed half is the driver, the leading half is which
    // cable it is.
    if (windowsName.find(L"(VB-Audio") != std::wstring::npos) {
        const size_t paren = windowsName.find(L" (");
        if (paren != std::wstring::npos)
            return L"[VB] " + windowsName.substr(0, paren);
    }
    return windowsName;
}

std::wstring FormatBattery(int percent) {
    if (percent < 0) return {};
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", percent);
    return buf;
}

bool IsHandsFreeName(const std::wstring& windowsName) {
    // The marker Windows itself puts in the name. Matching that rather than a
    // form-factor property, because a virtual render device can report Headset
    // too and marking it "low quality" would be wrong.
    std::wstring lower = windowsName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.find(L"hands-free") != std::wstring::npos) return true;
    // Windows does not always put "Hands-Free" in the name: the HFP endpoint of
    // a headset is published as "Headset (...)" against an A2DP twin called
    // "Headphones (...)". The leading word is what separates them.
    return lower.rfind(L"headset", 0) == 0;
}

uint64_t LocalFileTimeToUtc(uint64_t localFileTime) {
    if (localFileTime == 0) return 0;
    FILETIME local = { (DWORD)(localFileTime & 0xFFFFFFFF), (DWORD)(localFileTime >> 32) };
    FILETIME utc = {};
    if (!LocalFileTimeToFileTime(&local, &utc)) return localFileTime;
    return ((uint64_t)utc.dwHighDateTime << 32) | utc.dwLowDateTime;
}

bool SameDevice(const DeviceRefKeys& ref, const DeviceRefKeys& dev) {
    // ── what RULES IT OUT, before anything that rules it in ──────────────
    //
    // A strong anchor that disagrees is decisive, and must not be overridden
    // by a weak one that happens to agree. Windows REUSES a name: "Headphones
    // (11- WF-1000XM5)" belongs to whichever of the five identical pairs was
    // paired eleventh, and the twelfth pairing of a different pair inherits it
    // later. Two headsets with different addresses wearing the same name is
    // therefore an ordinary occurrence here, and calling them one device would
    // point failover at the wrong earbuds.
    //
    // Only the two anchors that belong to the DEVICE can rule out. The id and
    // the Windows name are per-pairing and disagree constantly between two
    // references to the same headset -- that is the whole reason this function
    // exists -- so neither can ever be a disqualifier.
    if (!ref.btAddress.empty() && !dev.btAddress.empty() && ref.btAddress != dev.btAddress)
        return false;
    if (!ref.alias.empty() && !dev.alias.empty() && ref.alias != dev.alias)
        return false;

    // ── and then what rules it IN ────────────────────────────────────────
    //
    // The headset's own address first. It outlives the pairing, the endpoint
    // id, the ContainerId and the Windows name, so it is the only one of these
    // that is still true after the adapter is swapped.
    if (!ref.btAddress.empty() && ref.btAddress == dev.btAddress) return true;
    // Then our own name: already re-homed onto whatever endpoint the device is
    // wearing today.
    if (!ref.alias.empty() && ref.alias == dev.alias) return true;
    if (!ref.id.empty() && ref.id == dev.id) return true;
    // Windows names are compared exactly, not case-insensitively: they come
    // from the same source on both sides, and a loose compare here would file
    // two genuinely different endpoints together.
    if (!ref.name.empty() && ref.name == dev.name) return true;
    return false;
}

bool HealDeviceRef(DeviceRefKeys& ref, const DeviceRefKeys& dev) {
    bool changed = false;
    // The address only ever arrives; it is never overwritten, because a device
    // that answers to an address IS that device and a second opinion would
    // have to be wrong.
    if (ref.btAddress.empty() && !dev.btAddress.empty()) {
        ref.btAddress = dev.btAddress;
        changed = true;
    }
    // The id and the Windows name are the perishable ones, so they are brought
    // up to date whenever they differ. Not cleared when the device has none:
    // keeping the last known value is strictly better than nothing for an
    // entry that may next be matched while its headset is switched off.
    if (!dev.id.empty() && ref.id != dev.id) { ref.id = dev.id; changed = true; }
    if (!dev.name.empty() && ref.name != dev.name) { ref.name = dev.name; changed = true; }
    return changed;
}

std::vector<size_t> DuplicateRefs(const std::vector<DeviceRefKeys>& refs) {
    std::vector<size_t> drop;
    std::vector<size_t> kept;
    for (size_t i = 0; i < refs.size(); ++i) {
        bool dup = false;
        for (size_t k : kept)
            if (SameDevice(refs[k], refs[i]) || SameDevice(refs[i], refs[k])) { dup = true; break; }
        if (dup) drop.push_back(i);
        else kept.push_back(i);
    }
    return drop;
}

std::wstring FormatLastSeen(uint64_t utcFileTime, bool present) {
    if (present) return L"Connected";   // outranks any stamp; see the header
    if (utcFileTime == 0) return {};
    FILETIME utc = { (DWORD)(utcFileTime & 0xFFFFFFFF), (DWORD)(utcFileTime >> 32) };
    FILETIME local = {};
    if (!FileTimeToLocalFileTime(&utc, &local)) return {};
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&local, &st)) return {};
    static const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    if (st.wMonth < 1 || st.wMonth > 12) return {};
    wchar_t buf[32];
    swprintf(buf, 32, L"%d %s %02d:%02d", st.wDay, kMonths[st.wMonth - 1], st.wHour, st.wMinute);
    return buf;
}

} // namespace mdxm
