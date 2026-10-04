// Devices tab: endpoint pickers (personal / streaming cable / mic in / mic
// cable / per-channel source), mic gain, failover, and the fallback banner.
// Device changes mutate config; Apply restarts the engine on the new bindings.
//
// Layout is a two-column grid driven by UiMetrics: every row advances a running
// cursor, so controls cannot land on top of each other however many channels
// are configured. Sections are a heading plus a hairline rather than a group
// box — the box's border clipped the controls inside it and drew a light frame
// that fought every dark theme.
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/main_window.h"
#include "device/endpoints.h"
#include "device/endpoint_volume.h"
#include "device/device_identity.h"
#include "device/seen_format.h"
#include "ui/theme.h"
#include <algorithm>
#include <string>
#include <windows.h>
#include <commctrl.h>
#include <vector>

namespace mdxm {

namespace {

constexpr wchar_t kClass[] = L"mdxmixerTabDevices";
constexpr int kPersonal = 1500, kStreamCable = 1501, kMicIn = 1502, kMicCable = 1503;
constexpr int kApply = 1504, kMicGain = 1505, kBanner = 1506, kStatus = 1507;
constexpr int kFoArmed = 1510, kFoList = 1511, kFoCombo = 1512;
constexpr int kFoAdd = 1513, kFoRemove = 1514, kFoUp = 1515, kFoDown = 1516;
constexpr int kFoHint = 1517;
constexpr int kFoView = 1518, kFoViewLabel = 1519;
constexpr int kFoName = 1522, kFoSetName = 1523, kFoNameLabel = 1524;
constexpr int kFoPrune = 1525, kFoNowOn = 1526;
constexpr int kHeadDevices = 1520, kHeadFailover = 1521;
constexpr int kChanBase = 1550;   // + channel index: source endpoint combo

struct DevicesTabState {
    UiContext* ctx = nullptr;
    // The aliased, sorted device list the failover rows are read from: short
    // names, battery and last-seen all come off it, so the allowlist and the
    // mixer cannot disagree about what a device is called.
    std::vector<DeviceLevel> levels;
    // Clicking a column header sorts by it, and clicking the same one again
    // reverses -- the behaviour every other list in mdx12 has (its hotkey
    // list, its tools list, its annotations list all handle
    // LVN_COLUMNCLICK), and the behaviour a column header implies whether or
    // not anything says so. The failover list shipped with LVS_NOSORTHEADER,
    // which is precisely the style that refuses the click.
    //
    // The View combo stays, and stays authoritative: it is the only way back
    // to the preferred order, which is not a sort but the RULE itself.
    bool sortDesc = false;
    DWORD lastLevelsTick = 0;   // throttles the endpoint sweep; see kRefreshMsg
    // Exactly what is on screen right now, cell for cell.
    //
    // The list is re-read every two seconds whether or not anything moved,
    // and DeleteAllItems + re-insert makes a visible flash every time: "the
    // list shouldn't flicker when there isn't any changes to the devices".
    // Rebuilding is skipped outright when the new rows are identical to
    // these, which on an idle machine is every time.
    std::vector<std::wstring> shownRows;
    std::vector<std::wstring> shownOffers;
    // What the Add dropdown is currently offering, parallel to its items.
    // mdxmixer's combo is CBS_DROPDOWNLIST, so unlike mdx12 -- whose combo is
    // editable and has to strip its own decoration back off a typed string --
    // the selection index is enough and the stored name stays plain.
    std::vector<DeviceRef> foOffers;
    // Visible row -> index into personalFailover.allow. The two differ the
    // moment a sort view is chosen, and every button has to act on the ENTRY,
    // not on the row number.
    std::vector<size_t> rowOrder;
    UiMetrics m;
    std::vector<EndpointInfo> eps;
    std::vector<int> renderIdx, captureIdx, allIdx;
    std::vector<std::wstring> chIds;
    RECT ruleLeft{}, ruleRight{};   // hairlines under the two section headings
};

bool IsHeading(int id) { return id == kHeadDevices || id == kHeadFailover; }

// Which endpoints a dropdown offers.
enum class Flow { Render, Capture, Any };

// How a device reads in ANY dropdown on this tab.
//
// One function, because there were two spellings of the same list and only
// one of them had been fixed: "I keep asking you to fix this drop list please
// fix this drop list to have the same logic and use the same short names as
// the other drop list on the same tab". The failover list had short names,
// last-seen dates and a useful order; the five device pickers beside it still
// had raw Windows names in enumeration order.
//
// `displayName` is the alias when one is set and the Windows name otherwise,
// so a device is called the same thing here, in the failover list, and on the
// Mixer tab.
std::wstring OfferLabel(const DeviceLevel& d, const std::wstring& absSeen) {
    std::wstring shown = d.displayName;
    // Said outright rather than left to the name: "Headphones (X)" and
    // "Headset (X)" differ by one word and are close to opposites, and once
    // aliases are in play the name carries no warning at all.
    if (d.isHandsFree) shown += L"   [hands-free]";
    if (!d.active)
        shown += L"   Last: " + (absSeen.empty() ? L"unknown" : FriendlySeen(absSeen));
    return shown;
}

// Live first, then the most recently seen, then the never-seen; by name
// within each. The one you want is almost always at the top.
struct Offer { size_t level; std::wstring shown, sortSeen; bool live; };

std::vector<Offer> BuildOffers(const std::vector<DeviceLevel>& levels, Flow flow) {
    std::vector<Offer> offers;
    for (size_t i = 0; i < levels.size(); ++i) {
        const DeviceLevel& d = levels[i];
        if (flow == Flow::Render && !d.isRender) continue;
        if (flow == Flow::Capture && d.isRender) continue;
        Offer o;
        o.level = i;
        o.live = d.active;
        o.sortSeen = AbsoluteSeen(d.lastConnectedUtc);
        o.shown = OfferLabel(d, o.sortSeen);
        offers.push_back(std::move(o));
    }
    std::stable_sort(offers.begin(), offers.end(), [](const Offer& a, const Offer& b) {
        if (a.live != b.live) return a.live;
        if (a.live) return _wcsicmp(a.shown.c_str(), b.shown.c_str()) < 0;
        const bool ak = !a.sortSeen.empty(), bk = !b.sortSeen.empty();
        if (ak != bk) return ak;
        if (ak && a.sortSeen != b.sortSeen) return a.sortSeen > b.sortSeen;
        return _wcsicmp(a.shown.c_str(), b.shown.c_str()) < 0;
    });
    return offers;
}

// The dropped list is not limited to the width of the box it drops from, and
// these entries are long. Measured against the real font rather than guessed
// from a character count, and capped at the width of the display so a stray
// long name cannot produce a list wider than the screen.
void FitDroppedWidth(HWND combo, const std::vector<Offer>& offers) {
    HDC dc = GetDC(combo);
    HFONT font = (HFONT)SendMessageW(combo, WM_GETFONT, 0, 0);
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    int widest = 0;
    for (const Offer& o : offers) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, o.shown.c_str(), (int)o.shown.size(), &sz);
        if (sz.cx > widest) widest = sz.cx;
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(combo, dc);
    SendMessageW(combo, CB_SETDROPPEDWIDTH,
                 (WPARAM)std::min(widest + GetSystemMetrics(SM_CXVSCROLL) + 16,
                                  GetSystemMetrics(SM_CXSCREEN)), 0);
}

// An endpoint picker: "None", then every device of the right flow.
//
// The chosen endpoint is carried in the item DATA rather than inferred from
// the index, so the sort order above cannot make a selection mean a different
// device than it did a moment ago.
// A choice that is not a device: its label, and the id stored for it.
struct SpecialChoice { const wchar_t* label; const wchar_t* id; };

void FillDeviceCombo(HWND combo, const std::vector<DeviceLevel>& levels, Flow flow,
                     const DeviceRef& current,
                     const std::vector<SpecialChoice>& specials = {
                         { L"None", L"" } }) {
    const std::vector<Offer> offers = BuildOffers(levels, flow);

    // Nothing is touched unless the content differs: replacing a combo's
    // items closes it, and these are refilled on a timer.
    std::wstring sig;
    for (const SpecialChoice& sp : specials) { sig += sp.label; sig += L"\x1f"; }
    for (const Offer& o : offers) { sig += o.shown; sig += L"\x1f"; }
    wchar_t had[4] = {};
    (void)had;
    if (GetPropW(combo, L"sig") && *(std::wstring*)GetPropW(combo, L"sig") == sig) {
        return;
    }
    auto* kept = (std::wstring*)GetPropW(combo, L"sig");
    if (!kept) { kept = new std::wstring(); SetPropW(combo, L"sig", (HANDLE)kept); }
    *kept = sig;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    // The non-device choices lead, and their item data is the NEGATIVE of
    // their position minus one, so ComboSelection can tell them from a device
    // index without a second parallel list.
    for (size_t i = 0; i < specials.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)specials[i].label);
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)(LONG_PTR)(-1 - (LONG_PTR)i));
    }
    int sel = 0;
    for (size_t i = 0; i < specials.size(); ++i)
        if (!std::wstring(specials[i].id).empty() && current.id == specials[i].id)
            sel = (int)i;
    for (const Offer& o : offers) {
        const DeviceLevel& d = levels[o.level];
        const int at = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)o.shown.c_str());
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)at, (LPARAM)(LONG_PTR)o.level);
        if ((!current.id.empty() && d.id == current.id) ||
            (sel == 0 && current.id.empty() && !current.name.empty() &&
             _wcsicmp(d.name.c_str(), current.name.c_str()) == 0))
            sel = at;
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
    FitDroppedWidth(combo, offers);
}

DeviceRef ComboSelection(HWND combo, const std::vector<DeviceLevel>& levels,
                         const std::vector<SpecialChoice>& specials = {
                             { L"None", L"" } }) {
    const int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return {};
    const LONG_PTR at = (LONG_PTR)SendMessageW(combo, CB_GETITEMDATA, (WPARAM)sel, 0);
    if (at < 0) {
        const size_t which = (size_t)(-1 - at);
        if (which < specials.size()) return { specials[which].id, specials[which].label };
        return {};
    }
    if ((size_t)at >= levels.size()) return {};
    return { levels[(size_t)at].id, levels[(size_t)at].name };
}

// The personal output has two automatic modes as well as the devices, and
// they are spelled out because the difference between them is the thing that
// was confusing: one follows whatever Windows calls the default, the other
// ignores Windows entirely and takes the first device in the failover list.
const std::vector<SpecialChoice>& PersonalChoices() {
    static const std::vector<SpecialChoice> kChoices = {
        { L"Use failover list \u2014 first allowed device that is present", kFollowFailover },
        { L"Follow the Windows default output", L"" },
    };
    return kChoices;
}

// One allowlist row, as three columns. PORTED from MDropDX12's AllowRowCells.
struct AllowCells {
    std::wstring device;    // presence marker + the name we show it under
    std::wstring battery;   // "50%", or empty when there is none to show
    std::wstring seen;      // when it last was here, or empty when it is here
    bool here = false;
    // No device of this description exists on the machine at all -- not
    // connected, not merely switched off. Distinct from `!here`, which is the
    // ordinary case for a headset in a drawer.
    bool unknown = false;
    int  battPct = -1;
    std::wstring sortSeen;  // the absolute form, which is what sorts
};

AllowCells AllowRow(DevicesTabState* st, const DeviceRef& entry) {
    // An entry matches by id, or -- for one added while its device was
    // switched off, which has no id -- by the Windows name it stored.
    const DeviceLevel* found = nullptr;
    for (const auto& d : st->levels) {
        if ((!entry.id.empty() && d.id == entry.id) ||
            (entry.id.empty() && !entry.name.empty() && d.name == entry.name)) {
            found = &d;
            break;
        }
    }

    AllowCells c;
    c.here = found && found->active;
    // Our own short name if there is one, since that is what it was given for.
    //
    // An entry whose endpoint Windows has FORGOTTEN -- an old pairing, which
    // most of this list is -- has no live device to read a name off, and used
    // to fall back to the raw Windows name: seven rows of "Headphones (WF-100..."
    // truncated to nothing. The alias store still knows those by the name they
    // were recorded under, so it is asked directly.
    const std::wstring label =
        found ? found->displayName
              : AliasFor(st->ctx->store->Get().deviceNames, entry.id, L"", entry.name);
    c.device = (c.here ? L"[+] " : L"[  ] ") + label;

    if (!found) { c.unknown = true; return c; }   // no such device: cells empty

    if (c.here) {
        // Battery only for a device that is connected AND reports one.
        // Windows keeps the last figure it was told after a disconnect -- one
        // of Shane's earbud sets has read 1% for months sitting in its case --
        // so a figure beside a device that is not here would be a number from
        // the past presented as the present.
        c.battPct = found->battery;
        c.battery = BatteryCell(found->battery);
        // Last seen stays empty for something here now: the answer would be
        // "now", which is what the marker already says.
        return c;
    }
    // NO battery figure for a device that is not here, which is mdx12's rule
    // and the reason for it: Windows keeps the last percentage it was told
    // after a disconnect, so the cell would be a number from the past
    // presented as the present. Shane read exactly that and drew exactly the
    // wrong conclusion -- "I switched headsets but it doesn't show when I am
    // looking at the screen so I think the headset is not in it's charging
    // case". The battery still SORTS on the remembered figure, which is a
    // reasonable hint; it just is not shown as current fact.
    c.battPct = found->battery;
    c.sortSeen = AbsoluteSeen(found->lastConnectedUtc);
    c.seen = c.sortSeen.empty() ? L"unknown" : FriendlySeen(c.sortSeen);
    return c;
}

int SelectedRow(HWND hwnd) {
    return ListView_GetNextItem(GetDlgItem(hwnd, kFoList), -1, LVNI_SELECTED);
}

// A visible row back to its index in personalFailover.allow.
int EntryForRow(DevicesTabState* st, int row) {
    if (row < 0 || row >= (int)st->rowOrder.size()) return -1;
    return (int)st->rowOrder[(size_t)row];
}

void SelectRow(HWND hwnd, int row) {
    HWND list = GetDlgItem(hwnd, kFoList);
    if (row < 0 || row >= ListView_GetItemCount(list)) return;
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
}

// The Add dropdown: every render endpoint, with WHEN it was last here.
//
// PORTED from MDropDX12, whose comment says exactly why: "That is what tells
// four identically-named pairs of earbuds apart: the ones in use were last
// seen within a day, the dead duplicates months ago." Without it the list is
// eighteen rows of near-identical names -- "in mdx12 in drop list box showing
// devices to fail over it would show the last connected date time so I wasn't
// so confused".
void FillFailoverOffers(HWND hwnd, DevicesTabState* st) {
    HWND combo = GetDlgItem(hwnd, kFoCombo);
    st->foOffers.clear();

    struct Offer { DeviceRef ref; std::wstring shown, sortSeen; bool live; };
    std::vector<Offer> offers;
    for (const auto& d : st->levels) {
        if (!d.isRender) continue;        // failover moves the personal OUTPUT
        Offer o;
        o.ref = { d.id, d.name };
        o.live = d.active;
        o.sortSeen = AbsoluteSeen(d.lastConnectedUtc);
        o.shown = d.displayName;
        // Said outright rather than left to the name: "Headphones (X)" and
        // "Headset (X)" differ by one word and are close to opposites, and
        // this app carries its own names for these devices, so the name
        // cannot be relied on to carry the warning.
        if (d.isHandsFree) o.shown += L"   [hands-free]";
        if (!o.live)
            o.shown += L"   Last: " + (o.sortSeen.empty() ? L"unknown"
                                                          : FriendlySeen(o.sortSeen));
        offers.push_back(std::move(o));
    }

    // Live first, then the most recently seen, then the never-seen; by name
    // within each. The one you want is almost always at the top.
    std::stable_sort(offers.begin(), offers.end(), [](const Offer& a, const Offer& b) {
        if (a.live != b.live) return a.live;
        if (a.live) return _wcsicmp(a.shown.c_str(), b.shown.c_str()) < 0;
        const bool ak = !a.sortSeen.empty(), bk = !b.sortSeen.empty();
        if (ak != bk) return ak;
        if (ak && a.sortSeen != b.sortSeen) return a.sortSeen > b.sortSeen;
        return _wcsicmp(a.shown.c_str(), b.shown.c_str()) < 0;
    });

    // Nothing on screen is touched unless the content actually differs: a
    // dropped-open combo closes when its contents are replaced, and a user
    // half way through choosing a device had it shut under them every two
    // seconds.
    std::vector<std::wstring> rows;
    for (const Offer& o : offers) rows.push_back(o.shown);
    for (const Offer& o : offers) st->foOffers.push_back(o.ref);
    if (rows == st->shownOffers) return;
    st->shownOffers = rows;

    const int keep = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const std::wstring& r : rows)
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)r.c_str());
    if (!rows.empty())
        SendMessageW(combo, CB_SETCURSEL,
                     (WPARAM)(keep >= 0 && keep < (int)rows.size() ? keep : 0), 0);

    // The dropped list is not limited to the width of the box it drops from,
    // and these entries are long. Measured against the real font rather than
    // guessed from a character count, and capped at the width of the display
    // so a stray long name cannot produce a list wider than the screen.
    HDC dc = GetDC(combo);
    HFONT font = (HFONT)SendMessageW(combo, WM_GETFONT, 0, 0);
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    int widest = 0;
    for (const Offer& o : offers) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, o.shown.c_str(), (int)o.shown.size(), &sz);
        if (sz.cx > widest) widest = sz.cx;
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(combo, dc);
    const int cap = GetSystemMetrics(SM_CXSCREEN);
    SendMessageW(combo, CB_SETDROPPEDWIDTH,
                 (WPARAM)std::min(widest + GetSystemMetrics(SM_CXVSCROLL) + 16, cap), 0);
}

void ReloadFailoverList(HWND hwnd, DevicesTabState* st);

// The view choice, persisted. Both controls that can change it come through
// here, so the combo and the column headers cannot save different things.
void RememberSort(HWND hwnd, DevicesTabState* st) {
    const int view = (int)SendDlgItemMessageW(hwnd, kFoView, CB_GETCURSEL, 0, 0);
    st->ctx->store->Mutate([&](MixerConfig& c) {
        c.ui.allowSort = (view < 0 || view > 3) ? 0 : view;
        c.ui.allowSortDesc = st->sortDesc;
    });
}

// Re-read the devices and repaint everything that depends on them, WITHOUT
// touching the endpoint pickers: those carry choices the user has made and
// not yet applied, and refilling them would throw those away.
void RefreshFailover(HWND hwnd, DevicesTabState* st) {
    if (!st->ctx->ctl) return;
    st->levels = st->ctx->ctl->GetDeviceLevels();
    // The Add combo is refilled, so its selection is remembered by DEVICE and
    // put back rather than by index, which the new sort order may have moved.
    const int sel = (int)SendDlgItemMessageW(hwnd, kFoCombo, CB_GETCURSEL, 0, 0);
    const std::wstring wanted = (sel >= 0 && sel < (int)st->foOffers.size())
                                    ? st->foOffers[(size_t)sel].id : std::wstring();
    FillFailoverOffers(hwnd, st);
    if (!wanted.empty()) {
        for (size_t i = 0; i < st->foOffers.size(); ++i)
            if (st->foOffers[i].id == wanted) {
                SendDlgItemMessageW(hwnd, kFoCombo, CB_SETCURSEL, (WPARAM)i, 0);
                break;
            }
    }
    ReloadFailoverList(hwnd, st);
}

void ReloadFailoverList(HWND hwnd, DevicesTabState* st) {
    HWND list = GetDlgItem(hwnd, kFoList);
    const int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);

    const auto& allow = st->ctx->store->Get().personalFailover.allow;
    st->rowOrder.clear();
    for (size_t i = 0; i < allow.size(); ++i) st->rowOrder.push_back(i);

    // The VIEW only, never the stored order.
    //
    // mdx12's rule, and the reason Move Up and Down grey out below: the
    // stored order IS the failover preference -- the watcher takes the first
    // entry that is present -- so a sorted view must never be written back,
    // and a move made through one would move a row somewhere the user cannot
    // see while the visible list sat still.
    //
    // Most-useful-first in every case: newest sighting, fullest battery, A to
    // Z, with "no figure" last.
    std::vector<AllowCells> cells;
    for (size_t i : st->rowOrder) cells.push_back(AllowRow(st, allow[i]));
    const int view = (int)SendDlgItemMessageW(hwnd, kFoView, CB_GETCURSEL, 0, 0);
    if (view > 0) {
        // The comparator always describes the MOST-USEFUL-FIRST order, and
        // the direction is applied by swapping its arguments. Writing it
        // twice is how the two directions drift apart.
        const auto less = [&](size_t a, size_t b) {
            const AllowCells& x = cells[a];
            const AllowCells& y = cells[b];
            if (view == 1) return _wcsicmp(x.device.c_str(), y.device.c_str()) < 0;
            if (view == 2) {
                if (x.battPct != y.battPct) return x.battPct > y.battPct;
                return _wcsicmp(x.device.c_str(), y.device.c_str()) < 0;
            }
            // Last seen. A device that is HERE outranks every date, because
            // "now" is the most recent sighting there is; an entry with no
            // record at all sorts last.
            if (x.here != y.here) return x.here;
            if (x.sortSeen.empty() != y.sortSeen.empty()) return !x.sortSeen.empty();
            if (x.sortSeen != y.sortSeen) return x.sortSeen > y.sortSeen;
            return _wcsicmp(x.device.c_str(), y.device.c_str()) < 0;
        };
        std::stable_sort(st->rowOrder.begin(), st->rowOrder.end(),
                         [&](size_t a, size_t b) {
            return st->sortDesc ? less(b, a) : less(a, b);
        });
    }

    // The arrow on the header, so the direction is visible rather than
    // remembered. Cleared from every other column, or two would claim it.
    if (HWND hdr = ListView_GetHeader(list)) {
        for (int c = 0; c < 3; ++c) {
            HDITEMW h = {};
            h.mask = HDI_FORMAT;
            Header_GetItem(hdr, c, &h);
            h.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (view > 0 && c == view - 1)
                h.fmt |= st->sortDesc ? HDF_SORTDOWN : HDF_SORTUP;
            Header_SetItem(hdr, c, &h);
        }
    }

    // Built first, compared, and only then drawn. DeleteAllItems followed by
    // a re-insert flashes the whole control, and on an idle machine the two
    // second re-read produces byte-identical rows every time.
    std::vector<std::wstring> rows;
    for (size_t i : st->rowOrder) {
        const AllowCells& c = cells[i];
        rows.push_back(c.device + L"\x1f" + c.battery + L"\x1f" + c.seen);
    }
    if (rows != st->shownRows) {
        st->shownRows = rows;
        ListView_DeleteAllItems(list);
        int row = 0;
        for (size_t i : st->rowOrder) {
            const AllowCells& c = cells[i];
            LVITEMW it = {};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = row;
            it.pszText = const_cast<wchar_t*>(c.device.c_str());
            it.lParam = (LPARAM)i;      // back to the entry, whatever the view
            ListView_InsertItem(list, &it);
            ListView_SetItemText(list, row, 1, const_cast<wchar_t*>(c.battery.c_str()));
            ListView_SetItemText(list, row, 2, const_cast<wchar_t*>(c.seen.c_str()));
            ++row;
        }
        if (sel >= 0 && sel < row)
            ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }

    // Which device the route is on RIGHT NOW, by the name the rest of the
    // app calls it. Read from the engine rather than from config: failover
    // can have moved it since, and the whole point of the line is to say
    // where the audio actually is.
    if (HWND nowOn = GetDlgItem(hwnd, kFoNowOn)) {
        std::wstring on;
        if (st->ctx->ctl) {
            const std::wstring id = st->ctx->ctl->GetDiag().personalDevice;
            for (const auto& d : st->levels)
                if (d.id == id) { on = d.displayName; break; }
            if (on.empty() && !id.empty()) on = id;
        }
        const std::wstring text = on.empty() ? L"Now on: nothing" : (L"Now on: " + on);
        wchar_t had[256] = {};
        GetWindowTextW(nowOn, had, 256);
        if (text != had) SetWindowTextW(nowOn, text.c_str());
    }

    // How many rows have no device behind them at all. Those are the ones
    // "Remove unknown" takes, and the number is what makes it answerable
    // before pressing it.
    int unknown = 0;
    for (const AllowCells& c : cells) if (c.unknown) ++unknown;
    if (HWND b = GetDlgItem(hwnd, kFoPrune)) {
        wchar_t label[48];
        swprintf(label, 48, L"Remove unknown (%d)", unknown);
        wchar_t had[48] = {};
        GetWindowTextW(b, had, 48);
        if (wcscmp(had, label) != 0) SetWindowTextW(b, label);
        EnableWindow(b, unknown > 0);
    }

    // Move Up and Down edit the RULE, so they are meaningless through a sort.
    const bool ordered = (view == 0);
    for (int id : { kFoUp, kFoDown })
        if (HWND b = GetDlgItem(hwnd, id)) EnableWindow(b, ordered);
}

void Reload(HWND hwnd, DevicesTabState* st) {
    st->eps = EnumerateEndpoints();
    // Aliased and sorted, exactly as the Mixer tab sees it — the allowlist
    // has to call a device what the rest of the app calls it.
    st->levels = st->ctx->ctl ? st->ctx->ctl->GetDeviceLevels() : std::vector<DeviceLevel>{};
    st->renderIdx.clear();
    st->captureIdx.clear();
    st->allIdx.clear();
    for (int i = 0; i < (int)st->eps.size(); ++i) {
        st->allIdx.push_back(i);
        (st->eps[(size_t)i].isRender ? st->renderIdx : st->captureIdx).push_back(i);
    }
    const MixerConfig& cfg = st->ctx->store->Get();
    FillDeviceCombo(GetDlgItem(hwnd, kPersonal), st->levels, Flow::Render, cfg.personalOutput,
                    PersonalChoices());
    FillDeviceCombo(GetDlgItem(hwnd, kStreamCable), st->levels, Flow::Render,
                    cfg.streamingCable.render);
    FillDeviceCombo(GetDlgItem(hwnd, kMicIn), st->levels, Flow::Capture, cfg.mic.input);
    FillDeviceCombo(GetDlgItem(hwnd, kMicCable), st->levels, Flow::Render, cfg.mic.cable.render);
    for (size_t i = 0; i < st->chIds.size(); ++i) {
        const ChannelConfig* ch = nullptr;
        for (const auto& c : cfg.channels)
            if (c.id == st->chIds[i]) { ch = &c; break; }
        // Any endpoint may be a channel source: a capture endpoint reads
        // directly, a render endpoint is tapped via loopback.
        FillDeviceCombo(GetDlgItem(hwnd, kChanBase + (int)i), st->levels, Flow::Any,
                        ch ? ch->cable.capture : DeviceRef{});
    }
    SendMessageW(GetDlgItem(hwnd, kMicGain), TBM_SETPOS, TRUE, (LPARAM)(cfg.mic.gain * 100.0f + 0.5f));
    SendMessageW(GetDlgItem(hwnd, kFoArmed), BM_SETCHECK,
                 cfg.personalFailover.armed ? BST_CHECKED : BST_UNCHECKED, 0);
    FillFailoverOffers(hwnd, st);
    ReloadFailoverList(hwnd, st);
}

void ApplyBindings(HWND hwnd, DevicesTabState* st) {
    st->ctx->store->Mutate([&](MixerConfig& cfg) {
        cfg.personalOutput = ComboSelection(GetDlgItem(hwnd, kPersonal), st->levels,
                                            PersonalChoices());
        cfg.streamingCable.render = ComboSelection(GetDlgItem(hwnd, kStreamCable), st->levels);
        cfg.mic.input = ComboSelection(GetDlgItem(hwnd, kMicIn), st->levels);
        cfg.mic.cable.render = ComboSelection(GetDlgItem(hwnd, kMicCable), st->levels);
        for (size_t i = 0; i < st->chIds.size(); ++i)
            for (auto& c : cfg.channels)
                if (c.id == st->chIds[i])
                    c.cable.capture = ComboSelection(GetDlgItem(hwnd, kChanBase + (int)i),
                                                     st->levels);
    });
    if (st->ctx->restartEngine) st->ctx->restartEngine();
    SetWindowTextW(GetDlgItem(hwnd, kStatus), L"Devices applied.");
}

void Build(HWND hwnd, DevicesTabState* st, HINSTANCE inst) {
    const UiMetrics& m = st->m;
    auto label = [&](const wchar_t* text, int x, int y, int w, int id = -1) {
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                        x, y, w, m.S(20), hwnd, (HMENU)(INT_PTR)id, inst, nullptr);
    };
    auto combo = [&](int id, int x, int y, int w) {
        CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, m.S(400), hwnd, (HMENU)(INT_PTR)id, inst, nullptr);
    };

    // ── Left column: devices ──────────────────────────────────────────────
    int y = m.Margin();
    label(L"Audio devices", m.Margin(), y, m.S(240), kHeadDevices);
    y += m.HeadingH();
    st->ruleLeft = { m.Margin(), y, m.CtlX() + m.CtlW(), y + 1 };
    y += m.S(10);

    auto row = [&](const wchar_t* text, int id) {
        label(text, m.Margin(), y + m.LabelPad(), m.LabelW());
        combo(id, m.CtlX(), y, m.CtlW());
        y += m.RowH();
    };
    row(L"Personal output", kPersonal);
    row(L"Streaming cable", kStreamCable);
    row(L"Mic input", kMicIn);
    row(L"Mic cable", kMicCable);

    const MixerConfig& cfg = st->ctx->store->Get();
    for (const auto& c : cfg.channels) {
        std::wstring text = (c.name.empty() ? c.id : c.name) + L" source";
        row(text.c_str(), kChanBase + (int)st->chIds.size());
        st->chIds.push_back(c.id);
    }

    label(L"Mic gain", m.Margin(), y + m.LabelPad(), m.LabelW());
    HWND gain = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
        m.CtlX(), y, m.CtlW(), m.CtlH(), hwnd, (HMENU)(INT_PTR)kMicGain, inst, nullptr);
    SendMessageW(gain, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    y += m.RowH() + m.SectionGap();

    CreateWindowExW(0, L"BUTTON", L"Apply device changes", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m.CtlX(), y, m.S(170), m.BtnH(), hwnd, (HMENU)(INT_PTR)kApply, inst, nullptr);
    y += m.BtnH() + m.S(12);

    // Two message lines, each with its own row so neither can land on the
    // button above: kStatus reports the last action, kBanner the live fallback.
    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                    m.Margin(), y, m.CtlX() + m.CtlW() - m.Margin(), m.S(20),
                    hwnd, (HMENU)(INT_PTR)kStatus, inst, nullptr);
    y += m.S(22);
    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                    m.Margin(), y, m.CtlX() + m.CtlW() - m.Margin(), m.S(20),
                    hwnd, (HMENU)(INT_PTR)kBanner, inst, nullptr);

    // ── Right column: failover ────────────────────────────────────────────
    const int rx = m.RightX();
    // Wider than the other panels: this one carries a three-column list, and
    // the Device column has to fit a name plus its presence marker. The same
    // number the window sizes itself from, so the two cannot disagree.
    const int rw = m.FailoverPanelW();
    int ry = m.Margin();
    label(L"Failover", rx, ry, m.S(240), kHeadFailover);
    ry += m.HeadingH();
    st->ruleRight = { rx, ry, rx + rw, ry + 1 };
    ry += m.S(10);

    CreateWindowExW(0, L"BUTTON", L"Armed", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    rx, ry, m.S(120), m.S(22), hwnd, (HMENU)(INT_PTR)kFoArmed, inst, nullptr);
    ry += m.S(26);
    // "Now on", ported from MDropDX12, which puts the same line under its
    // route picker.
    //
    // The allowlist is held in PREFERRED order, which is the rule and must
    // not be re-sorted, so the device that is actually connected can be
    // anywhere in it -- below the fold, on a list this long. Shane read the
    // visible rows, saw [  ] against every one of them, and concluded his
    // headset had dropped while it was playing: "my sound cut out for a
    // minute and thought the headset was disconnected because it didn't show
    // connected in the list but showed the battery %".
    //
    // One line answers that without disturbing the order the rule depends on.
    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                    rx, ry, rw, m.S(20), hwnd, (HMENU)(INT_PTR)kFoNowOn, inst, nullptr);
    ry += m.S(26);
    // ── The allowlist, as MDropDX12 draws it ─────────────────────────────
    //
    // Three columns, not one padded string in a LISTBOX. Shane was choosing
    // between eighteen pairings whose Windows names differ by one digit, and
    // the raw list gave him nothing to choose BY: "sadly as you didn't port
    // the short names and don't have the last time seen from mdx12 yet, I
    // can't guess which headset to use".
    //
    // The presence marker LEADS the device column rather than getting a
    // column of its own, which is mdx12's reasoning and worth keeping: the
    // point of it is that presence scans down the left edge without reading
    // each line to the end.
    label(L"View", rx, ry + m.S(3), m.S(50), kFoViewLabel);
    combo(kFoView, rx + m.S(56), ry, rw - m.S(56));
    for (const wchar_t* v : { L"Preferred order (the rule)", L"Device name",
                              L"Battery", L"Last seen" })
        SendDlgItemMessageW(hwnd, kFoView, CB_ADDSTRING, 0, (LPARAM)v);
    // Restored, not reset. It is a standing choice about how the list reads,
    // and re-picking it on every launch is the kind of small friction that
    // makes a panel feel like it is not listening.
    SendDlgItemMessageW(hwnd, kFoView, CB_SETCURSEL,
                        (WPARAM)st->ctx->store->Get().ui.allowSort, (LPARAM)0);
    st->sortDesc = st->ctx->store->Get().ui.allowSortDesc;
    ry += m.RowH();

    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                    WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                    LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                    rx, ry, rw, m.S(180), hwnd, (HMENU)(INT_PTR)kFoList, inst, nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        // Sized against the CLIENT width, not the window width.
        //
        // Three columns totalling 98% of the window overflow it the moment
        // the border and the vertical scrollbar are taken off, and a report
        // ListView answers that with a horizontal scrollbar -- which then
        // eats a row of height and hides the right-hand column anyway.
        //
        // Battery gets more than it needs for "82%" because the HEADER is the
        // wider of the two -- mdx12's note, and still true here. Device takes
        // what is left, because the name is the column anyone reads.
        RECT lc = {};
        GetClientRect(lv, &lc);
        int avail = (int)(lc.right - lc.left) - GetSystemMetrics(SM_CXVSCROLL) - m.S(4);
        if (avail < m.S(180)) avail = m.S(180);
        const int battW = m.S(58), seenW = m.S(96);
        const wchar_t* titles[] = { L"Device", L"Battery", L"Last seen" };
        const int widths[] = { avail - battW - seenW, battW, seenW };
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(titles[i]);
            col.cx = widths[i];
            ListView_InsertColumn(lv, i, &col);
        }
    }
    ry += m.S(180) + m.S(10);
    combo(kFoCombo, rx, ry, rw);
    ry += m.RowH();
    const int bw = m.BtnW(), bgap = m.S(8);
    CreateWindowExW(0, L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
                    rx, ry, bw, m.BtnH(), hwnd, (HMENU)(INT_PTR)kFoAdd, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE,
                    rx + bw + bgap, ry, bw, m.BtnH(), hwnd, (HMENU)(INT_PTR)kFoRemove, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Up", WS_CHILD | WS_VISIBLE,
                    rx + 2 * (bw + bgap), ry, bw, m.BtnH(), hwnd, (HMENU)(INT_PTR)kFoUp, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Down", WS_CHILD | WS_VISIBLE,
                    rx + 3 * (bw + bgap), ry, bw, m.BtnH(), hwnd, (HMENU)(INT_PTR)kFoDown, inst, nullptr);
    ry += m.BtnH() + m.S(6);
    // Clearing out entries whose endpoint Windows no longer has.
    //
    // NOT automatic. An allowlist entry is matched by id OR by the Windows
    // name it stored, so one whose id is dead can still come back if a device
    // ever reappears under that name -- and the ORDER of this list is the
    // failover preference, which a silent prune would quietly rewrite. The
    // count is on the button so the size of what it would remove is visible
    // before it is pressed.
    CreateWindowExW(0, L"BUTTON", L"Remove unknown", WS_CHILD | WS_VISIBLE,
                    rx, ry, m.BtnW() * 2, m.BtnH(),
                    hwnd, (HMENU)(INT_PTR)kFoPrune, inst, nullptr);
    ry += m.BtnH() + m.S(12);
    // Renaming lives HERE as well as on the Mixer tab, because this is the
    // list where the names are read and so the list where a wrong one is
    // noticed. mdx12 puts a "Short name" box under its allowlist for exactly
    // that reason.
    label(L"Short name", rx, ry + m.S(3), m.S(80), kFoNameLabel);
    CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                    rx + m.S(86), ry, rw - m.S(86) - m.BtnW() - m.S(8), m.S(24),
                    hwnd, (HMENU)(INT_PTR)kFoName, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Set name", WS_CHILD | WS_VISIBLE,
                    rx + rw - m.BtnW(), ry, m.BtnW(), m.BtnH(),
                    hwnd, (HMENU)(INT_PTR)kFoSetName, inst, nullptr);
    ry += m.BtnH() + m.S(12);
    // Height sized for the wrapped text at this width, so the last line is not
    // sheared off the bottom.
    CreateWindowExW(0, L"STATIC",
                    L"The first device in this list that is present wins. A commit is permanent: "
                    L"reconnecting the old device does not move the route back.",
                    WS_CHILD | WS_VISIBLE, rx, ry, rw, m.S(72),
                    hwnd, (HMENU)(INT_PTR)kFoHint, inst, nullptr);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (DevicesTabState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    try {
        switch (msg) {
        case WM_CREATE: {
            st = new DevicesTabState;
            st->ctx = (UiContext*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->m.Init(hwnd);
            Build(hwnd, st, ((CREATESTRUCTW*)lp)->hInstance);
            Reload(hwnd, st);
            return 0;
        }
        case WM_PAINT: {
            // Hairlines under the section headings: the structure that the
            // group box used to imply, without its clipping border.
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (st && st->ctx->theme) {
                if (const ThemeState* t = st->ctx->theme()) {
                    HGDIOBJ old = SelectObject(dc, t->borderPen);
                    for (const RECT* r : { &st->ruleLeft, &st->ruleRight }) {
                        MoveToEx(dc, r->left, r->top, nullptr);
                        LineTo(dc, r->right, r->top);
                    }
                    SelectObject(dc, old);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NOTIFY: {
            if (st) {
                auto* pnm = (NMHDR*)lp;
                if (pnm && pnm->idFrom == kFoList && pnm->code == LVN_COLUMNCLICK) {
                    // Column 0 is Device, 1 Battery, 2 Last seen -- which are
                    // View entries 1, 2 and 3, the combo's first entry being
                    // the preferred order rather than a sort.
                    auto* nlv = (NMLISTVIEW*)lp;
                    const int want = nlv->iSubItem + 1;
                    const int cur = (int)SendDlgItemMessageW(hwnd, kFoView, CB_GETCURSEL, 0, 0);
                    if (want == cur) st->sortDesc = !st->sortDesc;
                    else { st->sortDesc = false; SendDlgItemMessageW(hwnd, kFoView, CB_SETCURSEL, (WPARAM)want, 0); }
                    RememberSort(hwnd, st);
                    ReloadFailoverList(hwnd, st);
                    return 0;
                }
            }
            if (st && st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeTrackbarCustomDraw(lp, *t, &r))
                    return r;
            }
            break;
        }
        case WM_HSCROLL: {
            if (!st) break;
            if (GetDlgCtrlID((HWND)lp) == kMicGain) {
                float vol = (float)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0) / 100.0f;
                st->ctx->ctl->SetVolume(L"mic", Mix::Personal, vol);
                st->ctx->store->Mutate([vol](MixerConfig& c) { c.mic.gain = vol; });
            }
            return 0;
        }
        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wp), code = HIWORD(wp);
            // Combos FIRST: the filter below drops everything that is not a
            // button click, and a CBN_SELCHANGE arriving after it was simply
            // thrown away. Choosing a View from the dropdown therefore never
            // saved -- and only appeared to sort at all because the two
            // second refresh re-read the combo on its own a moment later.
            // Clicking a column header went through WM_NOTIFY instead and did
            // save, which is what made it look intermittent rather than
            // broken: "It's not saving the preferred order".
            if (id == kFoView && code == CBN_SELCHANGE) {
                st->sortDesc = false;   // a fresh choice starts most-useful-first
                RememberSort(hwnd, st);
                ReloadFailoverList(hwnd, st);
                return 0;
            }
            if (code != BN_CLICKED) break;
            if (id == kApply) { ApplyBindings(hwnd, st); return 0; }
            if (id == kFoArmed) {
                bool armed = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                st->ctx->store->Mutate([armed](MixerConfig& c) { c.personalFailover.armed = armed; });
                if (st->ctx->restartEngine) st->ctx->restartEngine();
                SetWindowTextW(GetDlgItem(hwnd, kStatus),
                               armed ? L"Failover armed." : L"Failover disarmed.");
                return 0;
            }
            if (id == kFoAdd) {
                // Straight out of foOffers, which is parallel to the combo's
                // items — so what is stored is the PLAIN name, not the
                // decorated string the list shows.
                const int sel = (int)SendDlgItemMessageW(hwnd, kFoCombo, CB_GETCURSEL, 0, 0);
                if (sel < 0 || sel >= (int)st->foOffers.size()) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus), L"Pick a device to add first.");
                    return 0;
                }
                const DeviceRef d = st->foOffers[(size_t)sel];
                // Adding the same endpoint twice would give the watcher two
                // rules that can never disagree, and one of them would sit in
                // the list forever doing nothing.
                for (const auto& e : st->ctx->store->Get().personalFailover.allow) {
                    if (!e.id.empty() && e.id == d.id) {
                        SetWindowTextW(GetDlgItem(hwnd, kStatus),
                                       L"That device is already in the list.");
                        return 0;
                    }
                }
                st->ctx->store->Mutate([&](MixerConfig& c) { c.personalFailover.allow.push_back(d); });
                ReloadFailoverList(hwnd, st);
                return 0;
            }
            if (id == kFoRemove || id == kFoUp || id == kFoDown) {
                // The selected ROW, translated back to the ENTRY it stands
                // for. Through a sort view the two are different, and acting
                // on the row number would move whichever rule happened to be
                // at that position.
                const int row = SelectedRow(hwnd);
                const int sel = EntryForRow(st, row);
                if (sel < 0) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus), L"Select a device in the list first.");
                    return 0;
                }
                st->ctx->store->Mutate([&](MixerConfig& c) {
                    auto& allow = c.personalFailover.allow;
                    if (sel >= (int)allow.size()) return;
                    if (id == kFoRemove) allow.erase(allow.begin() + sel);
                    else if (id == kFoUp && sel > 0) std::swap(allow[(size_t)sel], allow[(size_t)sel - 1]);
                    else if (id == kFoDown && sel + 1 < (int)allow.size()) std::swap(allow[(size_t)sel], allow[(size_t)sel + 1]);
                });
                ReloadFailoverList(hwnd, st);
                // Up and Down are only enabled in the stored order, where row
                // and entry are the same number, so following the move is safe.
                const int newRow = row + (id == kFoUp ? -1 : id == kFoDown ? 1 : 0);
                SelectRow(hwnd, id == kFoRemove ? row : newRow);
                return 0;
            }
            if (id == kFoPrune) {
                // Every entry with no device behind it, in one go. Removal is
                // the only destructive thing on this panel, so it says what
                // it is about to do and what the cost is.
                std::vector<size_t> dead;
                const auto& allow = st->ctx->store->Get().personalFailover.allow;
                for (size_t i = 0; i < allow.size(); ++i)
                    if (AllowRow(st, allow[i]).unknown) dead.push_back(i);
                if (dead.empty()) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus),
                                   L"Every entry has a device behind it.");
                    return 0;
                }
                wchar_t ask[320];
                swprintf(ask, 320,
                         L"Remove %zu entr%s whose device this machine no longer has?\n\n"
                         L"An entry can also be matched by the name it stored, so one "
                         L"removed here would have to be added again if that device ever "
                         L"comes back.",
                         dead.size(), dead.size() == 1 ? L"y" : L"ies");
                if (MessageBoxW(hwnd, ask, L"mdxmixer", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return 0;
                st->ctx->store->Mutate([&](MixerConfig& c) {
                    for (size_t n = dead.size(); n-- > 0; )
                        if (dead[n] < c.personalFailover.allow.size())
                            c.personalFailover.allow.erase(
                                c.personalFailover.allow.begin() + (ptrdiff_t)dead[n]);
                });
                RefreshFailover(hwnd, st);
                return 0;
            }
            if (id == kFoSetName) {
                const int sel = EntryForRow(st, SelectedRow(hwnd));
                if (sel < 0) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus), L"Select a device in the list first.");
                    return 0;
                }
                wchar_t buf[128] = {};
                GetDlgItemTextW(hwnd, kFoName, buf, 128);
                const DeviceRef entry = st->ctx->store->Get().personalFailover.allow[(size_t)sel];
                // Through the same store the Mixer tab writes, and keyed the
                // same way, so a name set here is the name shown there.
                std::wstring container, bt;
                for (const auto& d : st->levels)
                    if (d.id == entry.id) { container = d.containerId; bt = d.btAddress; break; }
                st->ctx->store->Mutate([&](MixerConfig& c) {
                    // The address keys the name to the HEADSET, so one name
                    // covers every pairing of it, past and future.
                    SetAlias(c.deviceNames, entry.id, container, entry.name, buf, bt);
                });
                Reload(hwnd, st);
                if (HWND root = GetAncestor(hwnd, GA_ROOT))
                    PostMessageW(root, MainWindow::kRebuildMsg, 0, 0);
                return 0;
            }
            break;
        }
        case MainWindow::kRebuildMsg:
            // A device came or went. The whole point of the presence marker,
            // the battery and the last-seen column is that they are current,
            // and until now nothing re-read them: switching headsets left the
            // old one marked [+] and the new one marked [  ] with a stale
            // date, indefinitely.
            if (st) RefreshFailover(hwnd, st);
            return 0;

        case MainWindow::kRefreshMsg: {
            if (!st) return 0;
            // Also on a slow timer, because a device change is not the only
            // thing that moves these: a battery drains and a last-seen ages
            // without any endpoint arriving or leaving. Throttled hard --
            // this is a COM endpoint sweep, and running one at the 250 ms
            // tick rate is what faulted the audio stack before.
            const DWORD now = GetTickCount();
            if (now - st->lastLevelsTick > 2000) {
                st->lastLevelsTick = now;
                RefreshFailover(hwnd, st);
            }
            DiagState d = st->ctx->ctl->GetDiag();
            SetWindowTextW(GetDlgItem(hwnd, kBanner),
                d.personalFallback ? L"Bound output missing. Using the default device."
                                   : L"");
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (st && st->ctx->theme) {
                if (const ThemeState* t = st->ctx->theme()) {
                    // Section headings carry the accent; the fallback banner
                    // carries the warning colour. Everything else is body text.
                    int id = GetDlgCtrlID((HWND)lp);
                    if (msg == WM_CTLCOLORSTATIC && t->colors.dark && IsHeading(id)) {
                        SetTextColor((HDC)wp, t->colors.accent);
                        SetBkColor((HDC)wp, t->colors.bg);
                        return (LRESULT)t->bgBrush;
                    }
                    if (msg == WM_CTLCOLORSTATIC && t->colors.dark && id == kBanner) {
                        SetTextColor((HDC)wp, t->colors.bad);
                        SetBkColor((HDC)wp, t->colors.bg);
                        return (LRESULT)t->bgBrush;
                    }
                    if (msg == WM_CTLCOLORSTATIC && t->colors.dark &&
                        (id == kFoHint || id == kStatus || id == kFoNowOn)) {
                        SetTextColor((HDC)wp, t->colors.muted);
                        SetBkColor((HDC)wp, t->colors.bg);
                        return (LRESULT)t->bgBrush;
                    }
                    LRESULT r;
                    if (ThemeCtlColor(msg, wp, *t, &r)) return r;
                }
            }
            break;
        }
        case WM_ERASEBKGND: {
            if (st && st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeEraseBkgnd(hwnd, wp, *t, &r)) return r;
            }
            break;
        }
        case WM_DESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
    } catch (...) {}
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND CreateDevicesTab(HWND parent, UiContext* ctx) {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    return CreateWindowExW(0, kClass, L"", WS_CHILD, 0, 0, 100, 100, parent, nullptr, inst, ctx);
}

} // namespace mdxm
