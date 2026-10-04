// Mixer tab: one row per fader, in the idiom MDropDX12's mixer window already
// proved on this machine —
//
//   Game [P]   [=========slider=========]  [ 85]▲▼  (speaker)
//
// Decisions carried over from there rather than reinvented:
//  * One row per fader, not a block per channel. The whole surface is meant to
//    be scanned down a single column.
//  * [P]/[S] tags instead of repeating the words "Personal" and "Streaming" on
//    every row; the tag means the same thing the whole way down.
//  * The speaker glyph IS the mute control, red or green. A separate tick box
//    says the same thing twice and adds a second thing to look at and hit.
//  * A number with a spinner beside the slider: a slider cannot be driven one
//    percent at a time, and that is exactly how a personal level gets trimmed.
//  * Every row builds the same controls, so toggling mute never reflows the
//    column and the rows stay aligned.
#include "ui/ui_context.h"
#include "app/channel_setup.h"
#include "routing/sonar_api.h"
#include "ui/controls.h"
#include "ui/owner_draw.h"
#include "ui/ui_metrics.h"
#include "ui/main_window.h"
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <climits>
#include <map>
#include <math.h>
#include <string>
#include <deque>
#include <vector>

namespace mdxm {

namespace {

constexpr wchar_t kClass[] = L"mdxmixerTabMixer";
// Per row: +0 name, +1 slider, +2 value edit, +3 spin, +4 speaker.
constexpr int kRowBase = 1000;
constexpr int kRowStride = 10;
constexpr int kHeading = 990;
constexpr int kDeviceHeading = 991;      // "Outputs"
constexpr int kInputHeading = 988;       // "Inputs"
constexpr int kEmptyLabel = 989;
constexpr int kRenameLabel = 992, kRenameEdit = 993, kRenameBtn = 994, kRenameHint = 995;
constexpr int kShowHidden = 996;
constexpr int kShowOffline = 997;
constexpr int kTopHeading = 998;         // "In use", above everything
constexpr int kChanHeading = 987;        // "Channels", over the channel faders
// The channel builder, under the Channels heading.
constexpr int kSourceCombo = 980, kChannelName = 981, kAddChannel = 982, kAddSonar = 983;
// Row context menu.
constexpr int kMenuPin = 2001, kMenuHide = 2002, kMenuRename = 2003, kMenuShowHidden = 2004;
constexpr int kMenuRemoveChannel = 2005;
constexpr int kMenuUp = 2006, kMenuDown = 2007, kMenuTop = 2008;
// Per row, alongside the five above: +5 battery, +6 status.
constexpr int kFieldBattery = 5, kFieldStatus = 6, kFieldMeter = 7;
// The step buttons that stand in for the slider under ui.spinBoxes. How far
// they move is ui.volumeStepPercent -- the same number the volume hotkeys
// use, so a step means one thing wherever it is taken.
constexpr int kFieldMinus = 8, kFieldPlus = 9;

// Segoe MDL2 Assets: a speaker with waves, and a speaker with a cross.
constexpr wchar_t kGlyphLive[]  = L"\xE767";
constexpr wchar_t kGlyphMuted[] = L"\xE74F";

// What a row is called in the stored order.
//
// A CHANNEL rather than a fader: the Personal and Streaming rows are a pair
// and move together, which halves the list to manage and keeps them adjacent,
// where the rest of the layout assumes they are.
std::wstring OrderKeyFor(const std::wstring& channelId, bool isDevice) {
    return (isDevice ? L"dev:" : L"chan:") + channelId;
}

// A row is either a channel fader or an audio device's own Windows volume.
// Both are levels the user reaches for, so they share one surface and one row
// shape; only where the value is written differs.
enum class RowKind { ChannelFader, DeviceVolume };

struct Row {
    RowKind kind = RowKind::ChannelFader;
    std::wstring channelId;   // ChannelFader: channel id. DeviceVolume: endpoint id.
    Mix mix = Mix::Personal;
    bool muted = false;
    bool isDefault = false;   // DeviceVolume: the system default for its flow
    bool present = true;
    std::wstring windowsName; // DeviceVolume: what Windows calls it, for aliasing
    std::wstring containerId; // DeviceVolume: the anchor a name or pin hangs on
    bool isRender = true;     // DeviceVolume: which section it belongs to
    bool hidden = false;      // filed out of the list; only shown with Show hidden
    bool pinned = false;
    // Drawn ABOVE the channel faders, in its own short section at the very top.
    //
    // Sorting a pinned device to the head of the Outputs section is not enough,
    // because Outputs comes after Channels: Shane pinned his headset, watched
    // it reach the top of its own section, and reported "I still don't see the
    // connected headset at the top of the faders". Adding Sonar's six channels
    // pushed it further down again.
    //
    // MDropDX12 has no such problem because it has no sections — its pin is one
    // stable_partition across the entire fader list, so a failover device
    // outranks every channel too. This is that behaviour on a sectioned layout.
    bool atTop = false;
    // What is actually flowing, 0..1, or kPeakUnknown (-1) when there is no
    // meter to read. The meter below draws only for peak > 0, so an unknown
    // row paints nothing -- which is right, and is also what a zero did, so
    // the -1 costs no extra handling here. It matters on the wire, where a
    // client sorting by what is making sound must not read "cannot know" as
    // "silent" (docs/ipc.md §2.1).
    float peak = kPeakUnknown;
};

struct MixerTabState {
    UiContext* ctx = nullptr;
    UiMetrics m;
    std::vector<Row> rows;
    HFONT glyphFont = nullptr;
    int lineH = 26, rowH = 30, nameW = 180, valueW = 52, iconW = 40;
    int battW = 46, statusW = 92, meterW = 54;
    int stepW = 40;             // each of the step buttons
    bool spinBoxes = false;     // ui.spinBoxes: step buttons instead of a slider
    int step = 5;               // ui.volumeStepPercent
    int topHeadingY = 0, chanHeadingY = 0, deviceHeadingY = 0, inputHeadingY = 0;
    // The last visual walk, so a move can read the peers off the screen
    // rather than recomputing the sort and risking a different answer.
    std::vector<size_t> visualOrder;
    // Client-space top of each row, parallel to `rows`.
    //
    // Right-clicking used to identify a row from the CONTROL under the
    // cursor, which only works where a control happens to be and happens to
    // accept the mouse. A channel's name label is a plain STATIC, so the
    // click fell through to the page and the row came back as -1: "right
    // click doesn't let me move any channels still" -- the menu appeared but
    // had nothing row-specific on it. Geometry answers for the whole row,
    // including the gaps between its controls.
    std::vector<int> rowTop;
    int scrollY = 0;          // the list is longer than any window; see LayoutRows
    int contentH = 0;
    int selected = -1;        // device row whose name the rename box edits
    bool showHidden = false;  // reveal the devices filed away, to unfile them
    int hiddenCount = 0;

    // Paired-but-not-connected endpoints, off by default.
    //
    // This is what MDropDX12 does, and it is the single biggest thing standing
    // between this list and being readable. Its endpoint provider publishes a
    // channel only for an ACTIVE endpoint, so a headset that is paired but
    // switched off simply has no fader — "while connected" needs no test of
    // its own over there. Here the sweep deliberately enumerates UNPLUGGED
    // devices too, because their battery and last-seen are what you choose a
    // failover device BY; on this machine that is six dormant pairings and
    // their six hands-free twins, twelve of twenty-eight rows, and Shane's
    // report was exactly that: "I can't figure out where the current connected
    // headset is in the list of all the faders in some random order."
    //
    // So they are kept, and filed away behind a switch, the way hidden devices
    // are — the data stays available to the failover list without the mixer
    // paying for it.
    bool showOffline = false;
    int offlineCount = 0;
    std::vector<std::pair<std::wstring, std::wstring>> sources;  // combo: id, name
    std::vector<std::pair<std::wstring, std::wstring>> sonar;    // unadopted: id, channel name
    int builderY = 0;
    // The whole Windows name, for the rows whose label is trimmed. TTM_ADDTOOL
    // keeps the POINTER it is handed rather than copying, so a temporary would
    // leave the tooltip reading freed memory -- a deque because pushing
    // another string must not move the ones already handed out.
    HWND tips = nullptr;
    std::deque<std::wstring> tipText;
};

int RowIndexFromId(int id) { return (id - kRowBase) / kRowStride; }
int RowFieldFromId(int id) { return (id - kRowBase) % kRowStride; }

// Slider spans whatever is left between the name and the right-hand group, so
// the row grows with the window instead of stranding space at the edge.
// Whatever sits between the status column and the value box gets the leftover
// width: the slider when there is one, the meter when the slider has been
// replaced by step buttons.
int MiddleWidth(const MixerTabState* st, int clientW) {
    int right = clientW - st->m.Margin() - st->iconW - st->m.S(8) - st->valueW - st->m.S(10);
    if (st->spinBoxes) right -= st->stepW * 2 + st->m.S(6);
    int left = st->m.Margin() + st->nameW + st->m.S(10) + st->battW + st->m.S(6) +
               st->statusW + st->m.S(6) + (st->spinBoxes ? 0 : st->meterW + st->m.S(10));
    int w = right - left;
    return w < st->m.S(60) ? st->m.S(60) : w;
}

void LayoutRows(HWND hwnd, MixerTabState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UiMetrics& m = st->m;
    // Device names run long — "SteelSeries Sonar - Gaming (SteelSeries Sonar\n    // Virtual Audio Device)" — and a fixed column truncates them to the point
    // where Aux, Chat and Stream are indistinguishable. The column takes a
    // share of the window instead, so widening the window buys name, not just
    // slider.
    st->nameW = rc.right * 3 / 10;
    st->nameW = std::max(st->nameW, m.S(150));
    st->nameW = std::min(st->nameW, m.S(340));
    const int middleW = MiddleWidth(st, rc.right);
    const int battX   = m.Margin() + st->nameW + m.S(10);
    const int statusX = battX + st->battW + m.S(6);
    // With step buttons the meter takes the middle; with a slider the meter
    // keeps its own narrow column and the slider takes the middle.
    const int meterX  = statusX + st->statusW + m.S(6);
    const int meterW  = st->spinBoxes ? middleW : st->meterW;
    const int sliderX = meterX + meterW + m.S(10);
    const int sliderW = st->spinBoxes ? 0 : middleW;
    const int valueX  = st->spinBoxes ? meterX + meterW + m.S(10)
                                      : sliderX + sliderW + m.S(10);
    const int minusX  = valueX + st->valueW + m.S(6);
    const int plusX   = minusX + st->stepW + m.S(2);
    const int iconX   = (st->spinBoxes ? plusX + st->stepW : valueX + st->valueW) + m.S(8);
    int y = m.Margin() + m.HeadingH() + m.S(6);
    // The empty-state line stands where the channel rows would have been, so
    // the device section below it still starts clear of everything.
    bool anyChannel = false;
    for (const Row& r : st->rows) if (r.kind == RowKind::ChannelFader) anyChannel = true;
    const int off = st->scrollY;
    if (!anyChannel) {
        if (HWND empty = GetDlgItem(hwnd, kEmptyLabel))
            MoveWindow(empty, m.Margin(), y - off, m.S(600), m.S(20), TRUE);
        y += m.S(26);
    }
    // Each device section's heading gets its line reserved the moment its first
    // row is reached, whether or not anything came before it. Deriving the
    // position from the preceding row instead left the heading at y=0, stacked
    // on top of the Channels heading, whenever no channel was configured.
    st->topHeadingY = st->chanHeadingY = -1;
    st->deviceHeadingY = st->inputHeadingY = -1;
    bool topStarted = false, chansStarted = false;
    bool outputsStarted = false, inputsStarted = false;

    // The rows are BUILT channels-first, devices-second, and that order is
    // load-bearing: a row's control ids are kRowBase + index * kRowStride, and
    // every hit test, refresh and context menu finds its row back through
    // them. So the visual order is decided here instead, by walking the rows
    // in a different sequence. Nothing is moved, renumbered or rebuilt.
    //
    // Pinned devices, then the channel faders, then the rest of the devices.
    std::vector<size_t> order;
    order.reserve(st->rows.size());
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i].kind == RowKind::DeviceVolume && st->rows[i].atTop) order.push_back(i);
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i].kind == RowKind::ChannelFader) order.push_back(i);
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i].kind == RowKind::DeviceVolume && !st->rows[i].atTop) order.push_back(i);

    // The user's own order, applied WITHIN each group.
    //
    // Within, not across: a channel cannot be dragged into the Outputs list
    // and a device cannot be dragged among the channels, because the section
    // a row belongs to is a fact about the row and not a preference. What is
    // a preference is where it sits among its peers.
    //
    // Anything not listed sorts after everything that is, in the order it was
    // built -- mdx12's rule, and the reason a brand new channel appears at
    // the bottom rather than somewhere arbitrary in the middle.
    {
        const auto& stored = st->ctx->store->Get().order;
        std::map<std::wstring, size_t> rank;
        for (size_t i = 0; i < stored.size(); ++i) rank.emplace(stored[i], i);
        const size_t unlisted = stored.size() + 1;
        auto rankOf = [&](size_t row) {
            const Row& r = st->rows[row];
            auto it = rank.find(OrderKeyFor(r.channelId, r.kind == RowKind::DeviceVolume));
            return it == rank.end() ? unlisted : it->second;
        };
        auto group = [&](size_t row) {
            const Row& r = st->rows[row];
            if (r.kind == RowKind::DeviceVolume && r.atTop) return 0;
            if (r.kind == RowKind::ChannelFader) return 1;
            return r.isRender ? 2 : 3;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (group(a) != group(b)) return group(a) < group(b);
            const size_t ra = rankOf(a), rb = rankOf(b);
            if (ra != rb) return ra < rb;
            // Same rank means the two faders of one channel: Personal first,
            // which is the order they were built in and the order they are
            // read in everywhere else.
            return a < b;
        });
    }

    st->visualOrder = order;
    st->rowTop.assign(st->rows.size(), INT_MIN);
    for (size_t n = 0; n < order.size(); ++n) {
        const size_t i = order[n];
        const Row& r = st->rows[i];
        bool isDevice = r.kind == RowKind::DeviceVolume;
        if (isDevice && r.atTop && !topStarted) {
            topStarted = true;
            st->topHeadingY = y;
            y += m.HeadingH() + m.S(6);
        }
        if (!isDevice && !chansStarted) {
            chansStarted = true;
            if (n > 0) y += m.S(8);
            st->chanHeadingY = y;
            y += m.HeadingH() + m.S(6);
        }
        if (isDevice && !r.atTop && r.isRender && !outputsStarted) {
            outputsStarted = true;
            if (n > 0) y += m.S(8);
            st->deviceHeadingY = y;
            y += m.HeadingH() + m.S(6);
        }
        // `!r.atTop` on this one too: a pinned device's hands-free twin is a
        // capture endpoint, and without the test it would drag the Inputs
        // heading up into the top section.
        if (isDevice && !r.atTop && !r.isRender && !inputsStarted) {
            inputsStarted = true;
            if (n > 0) y += m.S(8);
            st->inputHeadingY = y;
            y += m.HeadingH() + m.S(6);
        }
        int base = kRowBase + (int)i * kRowStride;
        auto place = [&](int id, int x, int w, int h, int dy) {
            if (HWND c = GetDlgItem(hwnd, id))
                MoveWindow(c, x, y + dy - off, w, h, TRUE);
        };
        st->rowTop[i] = y - off;
        place(base + 0, m.Margin(), st->nameW, st->lineH, 0);
        place(base + kFieldBattery, battX, st->battW, st->lineH, 0);
        place(base + kFieldStatus, statusX, st->statusW, st->lineH, 0);
        place(base + kFieldMeter, meterX, meterW, st->lineH, 0);
        if (!st->spinBoxes) place(base + 1, sliderX, sliderW, st->lineH, 0);
        place(base + kFieldMinus, minusX, st->stepW, st->lineH, 0);
        place(base + kFieldPlus, plusX, st->stepW, st->lineH, 0);
        place(base + 2, valueX, st->valueW, st->lineH, 0);
        place(base + 4, iconX, st->iconW, st->lineH, 0);
        // UDS_ALIGNRIGHT sizes and parks the spinner inside its buddy's right
        // edge, but only at the moment the buddy is set — so it has to be set
        // again every time the edit moves, or the arrows stay at zero size.
        if (HWND spin = GetDlgItem(hwnd, base + 3))
            SendMessageW(spin, UDM_SETBUDDY, (WPARAM)GetDlgItem(hwnd, base + 2), 0);
        y += st->rowH;
        // A channel is two faders; a little air after the pair groups them
        // without a rule for every single row.
        //
        // Decided from the VISUAL neighbour rather than from the build index.
        // `i % 2` worked only while channel rows were laid out in the order
        // they were built, which a user-chosen order is free to change.
        if (st->rows[i].kind == RowKind::ChannelFader) {
            const bool last = (n + 1 >= order.size());
            const Row* next = last ? nullptr : &st->rows[order[n + 1]];
            if (!next || next->kind != RowKind::ChannelFader ||
                next->channelId != st->rows[i].channelId)
                y += m.S(8);
        }
    }
    // A section with no rows keeps its heading off-screen rather than stranding
    // a label over nothing.
    for (auto pair : { std::make_pair(kTopHeading, st->topHeadingY),
                       std::make_pair(kChanHeading, st->chanHeadingY),
                       std::make_pair(kDeviceHeading, st->deviceHeadingY),
                       std::make_pair(kInputHeading, st->inputHeadingY) }) {
        if (HWND head = GetDlgItem(hwnd, pair.first)) {
            ShowWindow(head, pair.second >= 0 ? SW_SHOW : SW_HIDE);
            if (pair.second >= 0)
                MoveWindow(head, m.Margin(), pair.second - off, m.S(300), m.S(20), TRUE);
        }
    }

    // ── Setup, under the faders ──────────────────────────────────────────
    //
    // The rename strip and the channel builder are both SETUP: things done
    // once, when a device is first named or a channel first made. They used
    // to bracket the list, with the builder's combo, name box and two buttons
    // taking the top of the page — so opening the mixer on a short window
    // showed a row of setup furniture and exactly one fader.
    //
    // The faders are what the window is opened FOR, so they get the top and
    // the setup goes below them.
    y += m.SectionGap();
    auto place = [&](int id, int x, int w, int h) {
        if (HWND c = GetDlgItem(hwnd, id)) MoveWindow(c, x, y - off, w, h, TRUE);
    };
    place(kRenameLabel, m.Margin(), m.S(150), m.S(22));
    place(kRenameEdit, m.Margin() + m.S(156), m.S(220), m.S(24));
    place(kRenameBtn, m.Margin() + m.S(384), m.S(100), m.S(26));
    y += m.S(34);

    y += m.SectionGap();
    st->builderY = y;
    if (HWND head = GetDlgItem(hwnd, kHeading))
        MoveWindow(head, m.Margin(), y - off, m.S(240), m.S(20), TRUE);
    y += m.HeadingH() + m.S(6);
    {
        int x = m.Margin();
        auto placeBuilder = [&](int id, int w, int h) {
            if (HWND c = GetDlgItem(hwnd, id)) MoveWindow(c, x, y - off, w, h, TRUE);
            x += w + m.S(8);
        };
        int comboW = std::min(m.S(340), std::max(m.S(160), (int)rc.right / 3));
        placeBuilder(kSourceCombo, comboW, m.S(320));      // height = dropped list
        placeBuilder(kChannelName, m.S(130), m.S(24));
        placeBuilder(kAddChannel, m.S(110), m.S(26));
        placeBuilder(kAddSonar, m.S(170), m.S(26));
    }
    st->contentH = y + m.S(34);

    // The hint and the two filing switches ride at the top of the page, and
    // do NOT scroll with it: they are about the list as a whole, and a hint
    // nobody scrolls to teaches nobody anything.
    //
    // The "Add a channel" heading used to be on this line, over the builder.
    // Both moved to the bottom with the rest of the setup — see above.
    int topY = m.Margin() - off;
    // The hint stops where the switches start. kSwitchesW is the width of BOTH
    // of them plus the gap — it used to reserve room for one, and the second
    // one was added on top of the hint, so "Right-click any device row to pin
    // or hide it" ran underneath "Show disconnected" and neither was readable.
    //
    // One constant, used by the hint and by the two MoveWindow calls below, so
    // a third switch cannot reintroduce the overlap by being added in only one
    // of the three places.
    const int kSwitchesW = m.S(366);
    if (HWND hint = GetDlgItem(hwnd, kRenameHint))
        MoveWindow(hint, m.Margin(), topY + m.S(2),
                   std::max(m.S(120),
                            (int)rc.right - m.Margin() * 2 - kSwitchesW - m.S(8)),
                   m.S(20), TRUE);
    // The two filing switches sit side by side on the heading's line. They are
    // the pair that decides how many rows exist, so they belong together.
    const int switchW = m.S(180);
    if (HWND show = GetDlgItem(hwnd, kShowHidden))
        MoveWindow(show, rc.right - m.Margin() - switchW, topY, switchW, m.S(22), TRUE);
    if (HWND showOff = GetDlgItem(hwnd, kShowOffline))
        MoveWindow(showOff, rc.right - m.Margin() - kSwitchesW, topY, switchW, m.S(22), TRUE);

    // The list is taller than any window this holds, so the page scrolls.
    SCROLLINFO si = { sizeof si };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = st->contentH;
    si.nPage = (UINT)rc.bottom;
    si.nPos = st->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

// Clamp the scroll position to what the content actually needs, then lay out
// again if it moved — the window can be resized taller than the list.
void LayoutScrolled(HWND hwnd, MixerTabState* st) {
    LayoutRows(hwnd, st);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int maxScroll = st->contentH - rc.bottom;
    if (maxScroll < 0) maxScroll = 0;
    if (st->scrollY > maxScroll || st->scrollY < 0) {
        st->scrollY = st->scrollY < 0 ? 0 : maxScroll;
        LayoutRows(hwnd, st);
    }
}

void ScrollTo(HWND hwnd, MixerTabState* st, int pos) {
    int before = st->scrollY;
    st->scrollY = pos;
    LayoutScrolled(hwnd, st);
    if (st->scrollY != before) InvalidateRect(hwnd, nullptr, TRUE);
}

void SelectDevice(HWND hwnd, MixerTabState* st, int index);
void LayoutScrolled(HWND hwnd, MixerTabState* st);
void ScrollTo(HWND hwnd, MixerTabState* st, int pos);

void Build(HWND hwnd, MixerTabState* st) {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    const UiMetrics& m = st->m;
    if (st->ctx->store) {
        st->spinBoxes = st->ctx->store->Get().ui.spinBoxes;
        st->step = st->ctx->store->Get().volumeStepPercent;
        if (st->step < 1) st->step = 1;
    }
    st->lineH = m.S(26);
    st->rowH  = m.S(30);
    st->valueW = m.S(62);   // room for three digits plus the spinner inside
    st->iconW = m.S(40);
    st->glyphFont = CreateFontW(-(st->lineH * 5 / 9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");

    // One tooltip control for the page; each trimmed name registers with it.
    st->tipText.clear();
    st->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               hwnd, nullptr, inst, nullptr);
    if (st->tips) {
        // Long device names wrap rather than running off the screen.
        SendMessageW(st->tips, TTM_SETMAXTIPWIDTH, 0, 500);
        SendMessageW(st->tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }

    // "Add a channel", not "Channels": this label is pinned to the top line
    // beside the two filing checkboxes, and what sits under it is the builder
    // strip — a source combo, a name box and two buttons. The channel FADERS
    // now have their own heading down in the flow (kChanHeading), because a
    // pinned device is drawn above them and one word cannot head two sections
    // separated by a third.
    CreateWindowExW(0, L"STATIC", L"Add a channel", WS_CHILD | WS_VISIBLE,
                    0, 0, m.S(240), m.S(20),
                    hwnd, (HMENU)(INT_PTR)kHeading, inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Channels", WS_CHILD | WS_VISIBLE,
                    0, 0, m.S(300), m.S(20), hwnd, (HMENU)(INT_PTR)kChanHeading, inst, nullptr);

    auto chans = st->ctx->ctl->GetChannels();

    // ── The channel builder ───────────────────────────────────────────────
    //
    // A channel is a source mixed at two independent levels, Personal and
    // Streaming. Until this strip existed there was no way to make one: the
    // faders only ever appeared for channels already in the config file, and
    // nothing wrote them there. A render endpoint as the source is tapped by
    // loopback, so "SteelSeries Sonar - Gaming" becomes a channel with no
    // cable to install.
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                    0, 0, 10, m.S(320), hwnd, (HMENU)(INT_PTR)kSourceCombo, inst, nullptr);
    st->sources.clear();
    st->sonar.clear();
    std::vector<std::wstring> bound;
    for (const auto& c : chans) bound.push_back(c.name);
    for (const auto& d : st->ctx->ctl->GetDevices()) {
        const std::wstring& id = std::get<0>(d);
        const std::wstring& name = std::get<1>(d);
        bool active = std::get<3>(d);
        if (!active) continue;                     // an unplugged device is not a source
        st->sources.push_back({ id, name });
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        std::wstring sonarName = SonarChannelName(name);
        if (!sonarName.empty() &&
            std::find(bound.begin(), bound.end(), sonarName) == bound.end())
            st->sonar.push_back({ id, sonarName });
    }
    if (!st->sources.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);

    CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                    0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kChannelName, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Add channel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kAddChannel, inst, nullptr);
    // One click for the four Sonar mixes, because typing four names and
    // picking four devices to reproduce what Sonar already publishes is the
    // kind of setup nobody finishes.
    wchar_t sonarText[64];
    swprintf(sonarText, 64, L"Add Sonar channels (%d)", (int)st->sonar.size());
    HWND sonarBtn = CreateWindowExW(0, L"BUTTON", sonarText,
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                    (HMENU)(INT_PTR)kAddSonar, inst, nullptr);
    EnableWindow(sonarBtn, !st->sonar.empty());
    st->rows.clear();
    for (const auto& c : chans) {
        for (int pass = 0; pass < 2; ++pass) {
            bool personal = (pass == 0);
            Row row;
            row.channelId = c.id;
            row.mix = personal ? Mix::Personal : Mix::Streaming;
            row.muted = personal ? c.pmute : c.smute;
            int base = kRowBase + (int)st->rows.size() * kRowStride;
            float vol = personal ? c.pvol : c.svol;
            int pct = (int)(vol * 100.0f + 0.5f);

            std::wstring name = c.name + (personal ? L" [P]" : L" [S]");
            CreateWindowExW(0, L"STATIC", name.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 0), inst, nullptr);

            // Fader control, as on the device rows below: a slider, or the
            // step buttons from mdx12's option.
            if (!st->spinBoxes) {
                HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 1), inst, nullptr);
                SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
                SendMessageW(slider, TBM_SETPOS, TRUE, pct);
            } else {
                wchar_t minus[8], plus[8];
                swprintf(minus, 8, L"-%d", st->step);
                swprintf(plus, 8, L"+%d", st->step);
                CreateWindowExW(0, L"BUTTON", minus, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldMinus),
                                inst, nullptr);
                CreateWindowExW(0, L"BUTTON", plus, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldPlus),
                                inst, nullptr);
            }

            wchar_t num[16];
            swprintf(num, 16, L"%d", pct);
            HWND edit = CreateWindowExW(0, L"EDIT", num,
                            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_RIGHT,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 2), inst, nullptr);
            HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                            WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                            UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(base + 3), inst, nullptr);
            SendMessageW(spin, UDM_SETBUDDY, (WPARAM)edit, 0);
            SendMessageW(spin, UDM_SETRANGE32, 0, 100);
            SendMessageW(spin, UDM_SETPOS32, 0, pct);

            // The glyph is the mute control: SS_NOTIFY static rather than a
            // button, because its colour comes from WM_CTLCOLORSTATIC, which a
            // BUTTON never receives.
            HWND icon = CreateWindowExW(0, L"STATIC", row.muted ? kGlyphMuted : kGlyphLive,
                            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 4), inst, nullptr);
            if (st->glyphFont) {
                SendMessageW(icon, WM_SETFONT, (WPARAM)st->glyphFont, TRUE);
                SetPropW(icon, kKeepFontProp, (HANDLE)1);   // survive the window's font pass
            }

            st->rows.push_back(row);
        }
    }
    if (chans.empty())
        CreateWindowExW(0, L"STATIC",
                        L"No channels yet - pick a source above and Add channel. Each one gets a "
                        L"Personal and a Streaming fader.",
                        WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                        (HMENU)(INT_PTR)kEmptyLabel, inst, nullptr);

    // ── Audio devices: each endpoint's own Windows volume ─────────────────
    //
    // Two sections, and the list arrives already sorted (SortDevices): pinned
    // first, then the default, then whatever is connected. Windows publishes
    // far more endpoints than anyone mixes with — five headsets each with a
    // hands-free twin, every virtual Sonar device — so anything filed away with
    // Hide stays out until Show hidden asks for it.
    // "In use" heads the pinned block above the channel faders. Named for what
    // earns a device the place rather than for how it got there: the devices
    // there are the one you are listening through and anything you pinned by
    // hand, and "Pinned" would read as a filing category when the point is
    // that this is the row you actually reach for.
    CreateWindowExW(0, L"STATIC", L"In use", WS_CHILD | WS_VISIBLE,
                    0, 0, m.S(300), m.S(20), hwnd, (HMENU)(INT_PTR)kTopHeading, inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Outputs", WS_CHILD | WS_VISIBLE,
                    0, 0, m.S(300), m.S(20), hwnd, (HMENU)(INT_PTR)kDeviceHeading, inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Inputs", WS_CHILD | WS_VISIBLE,
                    0, 0, m.S(300), m.S(20), hwnd, (HMENU)(INT_PTR)kInputHeading, inst, nullptr);
    st->hiddenCount = 0;
    st->offlineCount = 0;
    for (const auto& d : st->ctx->ctl->GetDeviceLevels()) {
        if (d.hidden) ++st->hiddenCount;
        if (d.hidden && !st->showHidden) continue;
        // Counted only among rows that got past Hide, so the number on the box
        // is the number of rows it would actually add.
        // `active`, not `present` — see DeviceLevel. A paired headset in a
        // drawer is `present`, so filtering on that filtered nothing.
        if (!d.active) ++st->offlineCount;
        if (!d.active && !st->showOffline) continue;
        // A provider's own endpoints are decoys once its real channels are in
        // the list: an [SS] Aux row stuck at 100% that ignores a write, right
        // above the Aux channel at 4% that does not. Off by default, as in
        // mdx12; the switch is on the Options tab.
        if (!st->ctx->store->Get().ui.showVirtualEndpoints &&
            IsProviderOwnedEndpoint(d.name))
            continue;
        Row row;
        row.kind = RowKind::DeviceVolume;
        row.channelId = d.id;
        row.muted = d.mute;
        row.isDefault = d.isDefault;
        row.isRender = d.isRender;
        row.containerId = d.containerId;
        row.hidden = d.hidden;
        row.pinned = d.pinned;
        // Either kind of pin hoists the row. `pinned` is the user's own,
        // `autoPinned` is membership of the failover allowlist while the
        // device is connected — which is what puts the headset you are
        // actually listening through at the top without you doing anything.
        row.atTop = d.pinned || d.autoPinned;
        int base = kRowBase + (int)st->rows.size() * kRowStride;
        int pct = (int)(d.vol * 100.0f + 0.5f);

        // The default device leads with a marker so the column can be scanned
        // for it rather than read; capture endpoints say so, since a name alone
        // does not tell you which direction a device faces.
        row.present = d.present;
        row.windowsName = d.name;
        // The alias is what you called it; the marker says which is the system
        // default; "(in)" says a name alone cannot — which direction it faces;
        // and the hands-free twin is spelled out because confusing it with the
        // stereo endpoint costs a change of headsets.
        // The section already says which way it faces, so "(in)" is gone; what
        // is left are the things two rows of the same name do not tell you
        // apart on: which is the system default, which one you pinned, which
        // one is filed away, and which is the mono hands-free twin.
        //
        // The label is the SHORT name unless the user has given it one of his
        // own, in which case his wins. The full Windows name goes on a
        // tooltip, so nothing is hidden, only moved.
        const bool renamed = (d.displayName != d.name);
        std::wstring label = renamed ? d.displayName : ShortDeviceName(d.name);
        std::wstring name = (d.isDefault ? L"* " : L"   ") + label +
                            (d.isHandsFree ? L"  [mic - low quality]" : L"") +
                            (d.pinned ? L"  [pinned]" : L"") +
                            (d.hidden ? L"  [hidden]" : L"");
        HWND nameCtl = CreateWindowExW(0, L"STATIC", name.c_str(),
                        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOTIFY,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 0), inst, nullptr);
        if (st->tips && (renamed || label != d.name)) {
            // What it is really called, plus what it was called before the
            // rename -- the two things the trimmed label leaves out.
            st->tipText.push_back(renamed ? (d.displayName + L"\n" + d.name) : d.name);
            TTTOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)nameCtl;
            ti.lpszText = const_cast<wchar_t*>(st->tipText.back().c_str());
            SendMessageW(st->tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        CreateWindowExW(0, L"STATIC", FormatBattery(d.battery).c_str(),
                        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_RIGHT,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldBattery), inst, nullptr);
        CreateWindowExW(0, L"STATIC",
                        // d.active, not d.present: the Bluetooth node reports
                        // a paired headset as present whether or not Windows
                        // can stream to it, so present made all seven read
                        // "Connected" at once and hid the last-seen time that
                        // tells them apart.
                        FormatLastSeen(d.lastConnectedUtc, d.active).c_str(),
                        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldStatus), inst, nullptr);
        // A peak meter per device: the one thing on the surface that says
        // audio is actually REACHING this device, as opposed to being aimed
        // at it. Asked for after an outage where every other indicator read
        // healthy and nothing could be heard.
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldMeter),
                        inst, nullptr);
        row.peak = d.peak;
        // Fader control, mdx12's option: a slider, or a pair of step buttons
        // beside the number. The buttons move by ui.volumeStepPercent.
        if (!st->spinBoxes) {
            HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 1), inst, nullptr);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(slider, TBM_SETPOS, TRUE, pct);
        } else {
            wchar_t minus[8], plus[8];
            swprintf(minus, 8, L"-%d", st->step);
            swprintf(plus, 8, L"+%d", st->step);
            CreateWindowExW(0, L"BUTTON", minus, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldMinus),
                            inst, nullptr);
            CreateWindowExW(0, L"BUTTON", plus, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + kFieldPlus),
                            inst, nullptr);
        }
        wchar_t num[16];
        swprintf(num, 16, L"%d", pct);
        HWND edit = CreateWindowExW(0, L"EDIT", num,
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_RIGHT,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 2), inst, nullptr);
        HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                        UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(base + 3), inst, nullptr);
        SendMessageW(spin, UDM_SETBUDDY, (WPARAM)edit, 0);
        SendMessageW(spin, UDM_SETRANGE32, 0, 100);
        SendMessageW(spin, UDM_SETPOS32, 0, pct);
        HWND icon = CreateWindowExW(0, L"STATIC", row.muted ? kGlyphMuted : kGlyphLive,
                        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(base + 4), inst, nullptr);
        if (st->glyphFont) {
            SendMessageW(icon, WM_SETFONT, (WPARAM)st->glyphFont, TRUE);
            SetPropW(icon, kKeepFontProp, (HANDLE)1);
        }
        st->rows.push_back(row);
    }

    // Renaming. Windows will not let these be renamed persistently and hands
    // out new endpoint ids on its own, so the name has to be ours — click a
    // device to select it, type, Set name. Empty clears back to the Windows
    // name.
    CreateWindowExW(0, L"STATIC", L"Selected device name", WS_CHILD | WS_VISIBLE,
                    0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kRenameLabel, inst, nullptr);
    CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                    0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kRenameEdit, inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Set name", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kRenameBtn, inst, nullptr);
    CreateWindowExW(0, L"STATIC",
                    L"Click a device to rename it. Right-click any device row to pin or hide it.",
                    WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                    (HMENU)(INT_PTR)kRenameHint, inst, nullptr);
    wchar_t showText[64];
    swprintf(showText, 64, L"Show hidden devices (%d)", st->hiddenCount);
    // Owner-drawn, like every other checkbox in the app.
    //
    // A BS_AUTOCHECKBOX under visual styles IGNORES WM_CTLCOLORBTN and paints
    // its own label in the system button-text colour -- near-black, on a dark
    // theme, against a dark background. The text was there the whole time and
    // simply could not be read: "okay the font color you used is not readable
    // for me then, I moved it around and looked at in under different
    // contrasts". These two were the last plain ones left; the ported
    // CreateCheck exists precisely so a label gets the THEME's text colour.
    CreateCheck(hwnd, showText, kShowHidden, 0, 0, 10, 10, nullptr, st->showHidden);
    wchar_t offText[64];
    swprintf(offText, 64, L"Show disconnected (%d)", st->offlineCount);
    CreateCheck(hwnd, offText, kShowOffline, 0, 0, 10, 10, nullptr, st->showOffline);
    LayoutScrolled(hwnd, st);
}

// Filing a device changes which rows exist, so the whole surface is rebuilt.
// Every row builds the same controls, so this is cheap and cannot leave the
// column half-updated the way patching rows in place would.
void Rebuild(HWND hwnd, MixerTabState* st) {
    st->selected = -1;
    // The UI font is handed down by the main window, which will not do it again
    // for controls created after the fact — so carry it across the rebuild.
    HFONT font = nullptr;
    if (HWND head = GetDlgItem(hwnd, kHeading))
        font = (HFONT)SendMessageW(head, WM_GETFONT, 0, 0);
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }
    if (st->glyphFont) { DeleteObject(st->glyphFont); st->glyphFont = nullptr; }
    st->rows.clear();
    Build(hwnd, st);
    if (font) ApplyFontToChildren(hwnd, font);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Pin and hide, written through the same anchor the name uses so they survive
// Windows handing the device a new endpoint id.
void SetRowView(HWND hwnd, MixerTabState* st, int index, bool hidden, bool pinned) {
    if (index < 0 || index >= (int)st->rows.size()) return;
    const Row& row = st->rows[(size_t)index];
    if (row.kind != RowKind::DeviceVolume) return;
    st->ctx->ctl->SetDeviceView(row.channelId, row.containerId, row.windowsName, hidden, pinned);
    Rebuild(hwnd, st);
}

void AddChannelFromBuilder(HWND hwnd, MixerTabState* st) {
    int sel = (int)SendMessageW(GetDlgItem(hwnd, kSourceCombo), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)st->sources.size()) return;
    wchar_t name[64] = {};
    GetWindowTextW(GetDlgItem(hwnd, kChannelName), name, 64);
    // An empty name takes the source's own: naming a thing twice to create it
    // is a step, and this one has an obvious answer.
    std::wstring chName = name[0] ? name : st->sources[(size_t)sel].second;
    std::wstring sonarName = SonarChannelName(st->sources[(size_t)sel].second);
    if (!name[0] && !sonarName.empty()) chName = sonarName;
    std::wstring err;
    if (!st->ctx->ctl->AddChannel(chName, st->sources[(size_t)sel].first, &err)) {
        MessageBoxW(hwnd, err.c_str(), L"mdxmixer", MB_OK | MB_ICONWARNING);
        return;
    }
    Rebuild(hwnd, st);
}

void AddSonarChannels(HWND hwnd, MixerTabState* st) {
    auto pending = st->sonar;      // Rebuild rewrites st->sonar under us
    std::wstring firstErr;
    for (const auto& s : pending) {
        std::wstring err;
        if (!st->ctx->ctl->AddChannel(s.second, s.first, &err) && firstErr.empty())
            firstErr = err;
    }
    Rebuild(hwnd, st);
    if (!firstErr.empty())
        MessageBoxW(hwnd, firstErr.c_str(), L"mdxmixer", MB_OK | MB_ICONWARNING);
}

void RemoveChannelRow(HWND hwnd, MixerTabState* st, int index) {
    if (index < 0 || index >= (int)st->rows.size()) return;
    const Row& row = st->rows[(size_t)index];
    if (row.kind != RowKind::ChannelFader) return;
    std::wstring ask = L"Remove the channel \"" + row.channelId +
                       L"\"? Its levels and EQ go with it.";
    if (MessageBoxW(hwnd, ask.c_str(), L"mdxmixer", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    st->ctx->ctl->RemoveChannel(row.channelId);
    Rebuild(hwnd, st);
}

// Move one row among its PEERS -- the rows sharing its section.
//
// `delta` is -1 up, +1 down, 0 straight to the top. The stored order holds
// only the rows someone has arranged; everything else trails it in build
// order, so the first move of a row has to write out the peers above it too,
// or "up" from an unlisted row would have nothing to swap with.
void MoveRow(HWND hwnd, MixerTabState* st, int index, int delta) {
    if (index < 0 || index >= (int)st->rows.size()) return;
    const Row& row = st->rows[(size_t)index];
    const bool isDev = row.kind == RowKind::DeviceVolume;
    const std::wstring key = OrderKeyFor(row.channelId, isDev);

    // The peers, in the order they are drawn right now.
    std::vector<std::wstring> peers;
    for (int n = 0; n < (int)st->visualOrder.size(); ++n) {
        const Row& r = st->rows[st->visualOrder[(size_t)n]];
        const bool rDev = r.kind == RowKind::DeviceVolume;
        if (rDev != isDev) continue;
        if (isDev && r.isRender != row.isRender) continue;
        if (isDev && r.atTop != row.atTop) continue;
        const std::wstring k = OrderKeyFor(r.channelId, rDev);
        if (peers.empty() || peers.back() != k) peers.push_back(k);
    }
    size_t at = peers.size();
    for (size_t i = 0; i < peers.size(); ++i) if (peers[i] == key) { at = i; break; }
    if (at >= peers.size()) return;

    size_t to = at;
    if (delta == 0) to = 0;
    else if (delta < 0) { if (at == 0) return; to = at - 1; }
    else { if (at + 1 >= peers.size()) return; to = at + 1; }
    peers.erase(peers.begin() + (ptrdiff_t)at);
    peers.insert(peers.begin() + (ptrdiff_t)to, key);

    st->ctx->store->Mutate([&](MixerConfig& c) {
        // Everything that is NOT one of these peers keeps its stored
        // position; the peers are written back in their new order.
        std::vector<std::wstring> rebuilt;
        for (const auto& k : c.order) {
            bool isPeer = false;
            for (const auto& pk : peers) if (pk == k) { isPeer = true; break; }
            if (!isPeer) rebuilt.push_back(k);
        }
        rebuilt.insert(rebuilt.end(), peers.begin(), peers.end());
        c.order.swap(rebuilt);
    });
    Rebuild(hwnd, st);
}

void ShowRowMenu(HWND hwnd, MixerTabState* st, int index, POINT screen) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // Not offered on a Sonar channel: it is Sonar's, not ours. Removing it
    // here would delete nothing and the row would be back on the next tick.
    bool onChannel = index >= 0 && index < (int)st->rows.size() &&
                     st->rows[(size_t)index].kind == RowKind::ChannelFader &&
                     SonarKeyFromChannelId(st->rows[(size_t)index].channelId).empty();
    if (onChannel) {
        AppendMenuW(menu, MF_STRING, kMenuRemoveChannel, L"Remove this channel");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    bool onRow = index >= 0 && index < (int)st->rows.size() &&
                 st->rows[(size_t)index].kind == RowKind::DeviceVolume;
    if (onRow) {
        const Row& row = st->rows[(size_t)index];
        AppendMenuW(menu, MF_STRING, kMenuPin, row.pinned ? L"Unpin from top" : L"Pin to top");
        AppendMenuW(menu, MF_STRING, kMenuHide, row.hidden ? L"Show in list" : L"Hide from list");
        AppendMenuW(menu, MF_STRING, kMenuRename, L"Rename...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    // Reordering, on EVERY row rather than only on devices. Right-clicking a
    // channel used to offer "Remove" and nothing else, so the faders anyone
    // actually reaches for could not be brought to the top of the list.
    if (index >= 0 && index < (int)st->rows.size()) {
        AppendMenuW(menu, MF_STRING, kMenuTop, L"Move to top");
        AppendMenuW(menu, MF_STRING, kMenuUp, L"Move up");
        AppendMenuW(menu, MF_STRING, kMenuDown, L"Move down");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    wchar_t showText[64];
    swprintf(showText, 64, L"Show hidden devices (%d)", st->hiddenCount);
    AppendMenuW(menu, MF_STRING | (st->showHidden ? MF_CHECKED : 0), kMenuShowHidden, showText);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case kMenuPin:
        SetRowView(hwnd, st, index, st->rows[(size_t)index].hidden,
                   !st->rows[(size_t)index].pinned);
        break;
    case kMenuHide:
        // Hiding a pinned device clears the pin: it cannot be both at the top
        // of the list and absent from it, and the pin would surprise whoever
        // unhides it months later.
        SetRowView(hwnd, st, index, !st->rows[(size_t)index].hidden, false);
        break;
    case kMenuRename:
        SelectDevice(hwnd, st, index);
        ScrollTo(hwnd, st, st->contentH);      // the box lives under the last row
        SetFocus(GetDlgItem(hwnd, kRenameEdit));
        break;
    case kMenuRemoveChannel:
        RemoveChannelRow(hwnd, st, index);
        break;
    case kMenuTop:
    case kMenuUp:
    case kMenuDown:
        MoveRow(hwnd, st, index, cmd == kMenuTop ? 0 : (cmd == kMenuUp ? -1 : 1));
        break;
    case kMenuShowHidden:
        st->showHidden = !st->showHidden;
        Rebuild(hwnd, st);
        break;
    default: break;
    }
}

// Push a new level from any of the three controls that can set it, keeping the
// other two in step without letting them fight the one being dragged.
void SetLevel(HWND hwnd, MixerTabState* st, int index, int pct, bool fromSlider, bool fromEdit) {
    if (index < 0 || index >= (int)st->rows.size()) return;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    const Row& row = st->rows[(size_t)index];
    int base = kRowBase + index * kRowStride;
    if (row.kind == RowKind::DeviceVolume)
        st->ctx->ctl->SetDeviceVolume(row.channelId, pct / 100.0f);
    else
        st->ctx->ctl->SetVolume(row.channelId, row.mix, pct / 100.0f);
    if (!fromSlider && !st->spinBoxes)
        SendMessageW(GetDlgItem(hwnd, base + 1), TBM_SETPOS, TRUE, pct);
    if (!fromEdit) {
        wchar_t num[16];
        swprintf(num, 16, L"%d", pct);
        SetWindowTextW(GetDlgItem(hwnd, base + 2), num);
    }
}

void ToggleMute(HWND hwnd, MixerTabState* st, int index) {
    if (index < 0 || index >= (int)st->rows.size()) return;
    Row& row = st->rows[(size_t)index];
    row.muted = !row.muted;
    if (row.kind == RowKind::DeviceVolume)
        st->ctx->ctl->SetDeviceMute(row.channelId, row.muted);
    else
        st->ctx->ctl->SetMute(row.channelId, row.mix, row.muted);
    HWND icon = GetDlgItem(hwnd, kRowBase + index * kRowStride + 4);
    SetWindowTextW(icon, row.muted ? kGlyphMuted : kGlyphLive);
    InvalidateRect(icon, nullptr, TRUE);
}

void RefreshDeviceRows(HWND hwnd, MixerTabState* st) {
    // Device levels are owned by Windows, so they can move underneath us —
    // the tray slider, a headset's own wheel, another app. Poll them back.
    auto levels = st->ctx->ctl->GetDeviceLevels();
    for (size_t i = 0; i < st->rows.size(); ++i) {
        if (st->rows[i].kind != RowKind::DeviceVolume) continue;
        const DeviceLevel* found = nullptr;
        for (const auto& d : levels)
            if (d.id == st->rows[i].channelId) { found = &d; break; }
        if (!found) continue;
        int base = kRowBase + (int)i * kRowStride;
        int want = (int)(found->vol * 100.0f + 0.5f);
        HWND slider = st->spinBoxes ? nullptr : GetDlgItem(hwnd, base + 1);
        if (slider && GetCapture() != slider &&
            SendMessageW(slider, TBM_GETPOS, 0, 0) != want)
            SendMessageW(slider, TBM_SETPOS, TRUE, want);
        HWND edit = GetDlgItem(hwnd, base + 2);
        if (GetFocus() != edit) {
            wchar_t cur[16] = {};
            GetWindowTextW(edit, cur, 16);
            if (_wtoi(cur) != want) {
                wchar_t num[16];
                swprintf(num, 16, L"%d", want);
                SetWindowTextW(edit, num);
            }
        }
        // Repaint only when the bar would actually move: at 250 ms this runs
        // for every device row, and invalidating all of them unconditionally
        // is a repaint of the whole column four times a second for nothing.
        if (fabsf(found->peak - st->rows[i].peak) > 0.004f) {
            st->rows[i].peak = found->peak;
            InvalidateRect(GetDlgItem(hwnd, base + kFieldMeter), nullptr, FALSE);
        }
        if (found->mute != st->rows[i].muted) {
            st->rows[i].muted = found->mute;
            HWND icon = GetDlgItem(hwnd, base + 4);
            SetWindowTextW(icon, found->mute ? kGlyphMuted : kGlyphLive);
            InvalidateRect(icon, nullptr, TRUE);
        }
    }
}

void SelectDevice(HWND hwnd, MixerTabState* st, int index) {
    if (index < 0 || index >= (int)st->rows.size()) return;
    if (st->rows[(size_t)index].kind != RowKind::DeviceVolume) return;
    int prev = st->selected;
    st->selected = index;
    // Repaint both names so the accent moves with the selection.
    for (int i : { prev, index })
        if (i >= 0 && i < (int)st->rows.size())
            InvalidateRect(GetDlgItem(hwnd, kRowBase + i * kRowStride + 0), nullptr, TRUE);

    // Seed the box with the current alias, or empty when the device still
    // carries its Windows name — typing over a long Windows name to replace it
    // is worse than starting from nothing.
    auto levels = st->ctx->ctl->GetDeviceLevels();
    std::wstring current;
    for (const auto& d : levels)
        if (d.id == st->rows[(size_t)index].channelId && d.displayName != d.name)
            current = d.displayName;
    SetWindowTextW(GetDlgItem(hwnd, kRenameEdit), current.c_str());
    std::wstring hint = L"Renaming: " + st->rows[(size_t)index].windowsName;
    SetWindowTextW(GetDlgItem(hwnd, kRenameHint), hint.c_str());
}

void ApplyRename(HWND hwnd, MixerTabState* st) {
    if (st->selected < 0 || st->selected >= (int)st->rows.size()) {
        SetWindowTextW(GetDlgItem(hwnd, kRenameHint), L"Click a device above to rename it.");
        return;
    }
    wchar_t buf[128] = {};
    GetWindowTextW(GetDlgItem(hwnd, kRenameEdit), buf, 128);
    const Row& row = st->rows[(size_t)st->selected];
    // The container is looked up fresh: it, not the endpoint id, is what the
    // name is anchored to.
    std::wstring container;
    for (const auto& d : st->ctx->ctl->GetDeviceLevels())
        if (d.id == row.channelId) { container = d.containerId; break; }
    std::wstring endpointId = row.channelId;
    st->ctx->ctl->SetDeviceAlias(endpointId, container, row.windowsName, buf);
    // The name is a sort key, so the list is rebuilt rather than patched: a
    // device renamed "AAA" belongs where AAA sorts, not where it used to sit.
    Rebuild(hwnd, st);
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i].channelId == endpointId) { SelectDevice(hwnd, st, (int)i); break; }
    SetWindowTextW(GetDlgItem(hwnd, kRenameHint),
                   buf[0] ? L"Name saved. It follows this device when Windows changes its id."
                          : L"Name cleared.");
}

void Refresh(HWND hwnd, MixerTabState* st) {
    auto chans = st->ctx->ctl->GetChannels();
    size_t r = 0;
    for (const auto& c : chans) {
        for (int pass = 0; pass < 2 && r < st->rows.size(); ++pass, ++r) {
            bool personal = (pass == 0);
            int base = kRowBase + (int)r * kRowStride;
            int want = (int)((personal ? c.pvol : c.svol) * 100.0f + 0.5f);
            HWND slider = GetDlgItem(hwnd, base + 1);
            // Never fight the control the user is holding.
            if (GetCapture() != slider && SendMessageW(slider, TBM_GETPOS, 0, 0) != want)
                SendMessageW(slider, TBM_SETPOS, TRUE, want);
            HWND edit = GetDlgItem(hwnd, base + 2);
            if (GetFocus() != edit) {
                wchar_t cur[16] = {};
                GetWindowTextW(edit, cur, 16);
                if (_wtoi(cur) != want) {
                    wchar_t num[16];
                    swprintf(num, 16, L"%d", want);
                    SetWindowTextW(edit, num);
                }
            }
            bool muted = personal ? c.pmute : c.smute;
            if (muted != st->rows[r].muted) {
                st->rows[r].muted = muted;
                HWND icon = GetDlgItem(hwnd, base + 4);
                SetWindowTextW(icon, muted ? kGlyphMuted : kGlyphLive);
                InvalidateRect(icon, nullptr, TRUE);
            }
        }
    }
    RefreshDeviceRows(hwnd, st);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (MixerTabState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    try {
        switch (msg) {
        case WM_CREATE: {
            st = new MixerTabState;
            st->ctx = (UiContext*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->m.Init(hwnd);
            Build(hwnd, st);
            return 0;
        }
        case WM_SIZE:
            if (st) LayoutScrolled(hwnd, st);
            return 0;

        case WM_VSCROLL: {
            if (!st || lp) break;            // a trackbar's WM_VSCROLL carries lp
            SCROLLINFO si = { sizeof si };
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wp)) {
            case SB_LINEUP:   pos -= st->rowH; break;
            case SB_LINEDOWN: pos += st->rowH; break;
            case SB_PAGEUP:   pos -= (int)si.nPage; break;
            case SB_PAGEDOWN: pos += (int)si.nPage; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            default: break;
            }
            ScrollTo(hwnd, st, pos);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!st) break;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            ScrollTo(hwnd, st, st->scrollY - (delta / WHEEL_DELTA) * st->rowH * 3);
            return 0;
        }

        case WM_HSCROLL: {
            if (!st) break;
            HWND track = (HWND)lp;
            int id = GetDlgCtrlID(track);
            if (RowFieldFromId(id) == 1)
                SetLevel(hwnd, st, RowIndexFromId(id),
                         (int)SendMessageW(track, TBM_GETPOS, 0, 0), true, false);
            return 0;
        }

        case WM_NOTIFY: {
            if (!st) break;
            auto* hdr = (NMHDR*)lp;
            if (hdr->code == UDN_DELTAPOS) {
                // The spinner is the fine control: one percent per press.
                auto* ud = (NMUPDOWN*)lp;
                int id = GetDlgCtrlID(hdr->hwndFrom);
                if (RowFieldFromId(id) == 3)
                    SetLevel(hwnd, st, RowIndexFromId(id), ud->iPos + ud->iDelta, false, false);
                return 0;
            }
            if (st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeTrackbarCustomDraw(lp, *t, &r))
                    return r;
            }
            break;
        }

        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wp), code = HIWORD(wp);
            int field = RowFieldFromId(id), index = RowIndexFromId(id);
            if ((field == kFieldMinus || field == kFieldPlus) && code == BN_CLICKED) {
                wchar_t cur[16] = {};
                GetWindowTextW(GetDlgItem(hwnd, kRowBase + index * kRowStride + 2), cur, 16);
                int now = _wtoi(cur);
                SetLevel(hwnd, st, index, now + (field == kFieldPlus ? st->step : -st->step),
                         false, false);
                return 0;
            }
            if (field == 4 && code == STN_CLICKED) { ToggleMute(hwnd, st, index); return 0; }
            if (field == 0 && code == STN_CLICKED) { SelectDevice(hwnd, st, index); return 0; }
            if (id == kRenameBtn && code == BN_CLICKED) { ApplyRename(hwnd, st); return 0; }
            if (id == kAddChannel && code == BN_CLICKED) { AddChannelFromBuilder(hwnd, st); return 0; }
            if (id == kAddSonar && code == BN_CLICKED) { AddSonarChannels(hwnd, st); return 0; }
            // An owner-draw button keeps its tick in a prop and does not
            // toggle itself, so the state is read and flipped here rather
            // than asked for with BM_GETCHECK.
            if ((id == kShowHidden || id == kShowOffline) && code == BN_CLICKED) {
                const bool now = !(bool)(intptr_t)GetPropW((HWND)lp, L"Checked");
                SetPropW((HWND)lp, L"Checked", (HANDLE)(intptr_t)(now ? 1 : 0));
                (id == kShowHidden ? st->showHidden : st->showOffline) = now;
                Rebuild(hwnd, st);
                return 0;
            }
            if (field == 2 && code == EN_KILLFOCUS) {
                wchar_t buf[16] = {};
                GetWindowTextW((HWND)lp, buf, 16);
                SetLevel(hwnd, st, index, _wtoi(buf), false, true);
                return 0;
            }
            break;
        }

        case WM_CONTEXTMENU: {
            // Right-click anywhere on a device row — name, slider, battery —
            // reaches the same menu, because aiming at one small label is not
            // how anyone finds a feature.
            if (!st) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            HWND from = (HWND)wp;
            int index = -1;
            if (from && from != hwnd) index = RowIndexFromId(GetDlgCtrlID(from));
            if (pt.x == -1 && pt.y == -1) {          // keyboard menu key
                RECT rc;
                GetWindowRect(from ? from : hwnd, &rc);
                pt = { rc.left + 20, rc.top + 10 };
            }
            if (index < 0) {
                // Fall back to WHERE the click landed. This is the path a
                // channel row always takes -- its labels are plain STATICs
                // and never see the mouse -- and it is also what makes the
                // gaps between a row's controls part of that row.
                POINT client = pt;
                ScreenToClient(hwnd, &client);
                for (size_t i = 0; i < st->rowTop.size(); ++i) {
                    if (st->rowTop[i] == INT_MIN) continue;   // not drawn
                    if (client.y >= st->rowTop[i] && client.y < st->rowTop[i] + st->rowH) {
                        index = (int)i;
                        break;
                    }
                }
            }
            ShowRowMenu(hwnd, st, index, pt);
            return 0;
        }

        case WM_DRAWITEM: {
            if (!st) break;
            auto* di = (DRAWITEMSTRUCT*)lp;
            if (di && di->CtlType == ODT_BUTTON &&
                (bool)(intptr_t)GetPropW(di->hwndItem, L"IsCheckbox")) {
                const ThemeState* t = st->ctx->theme ? st->ctx->theme() : nullptr;
                const ThemeColors c = t ? t->colors : ThemePalette(ThemeKind::Light);
                DrawOwnerCheckbox(di, c.dark, c.bg, c.surface, c.border, c.text);
                return TRUE;
            }
            int index = RowIndexFromId((int)di->CtlID);
            if (RowFieldFromId((int)di->CtlID) != kFieldMeter) break;
            if (index < 0 || index >= (int)st->rows.size()) break;
            const ThemeState* t = st->ctx->theme ? st->ctx->theme() : nullptr;
            COLORREF bg = t ? t->colors.bg : GetSysColor(COLOR_BTNFACE);
            COLORREF trough = t ? t->colors.surface : GetSysColor(COLOR_WINDOW);
            COLORREF lit = t ? t->colors.good : RGB(0, 160, 0);
            COLORREF hot = t ? t->colors.bad : RGB(200, 0, 0);

            RECT rc = di->rcItem;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(di->hDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            // A short bar, vertically centred in the row.
            RECT bar = rc;
            int h = (rc.bottom - rc.top) / 3;
            if (h < 4) h = 4;
            bar.top = rc.top + ((rc.bottom - rc.top) - h) / 2;
            bar.bottom = bar.top + h;
            HBRUSH troughBrush = CreateSolidBrush(trough);
            FillRect(di->hDC, &bar, troughBrush);
            DeleteObject(troughBrush);

            float peak = st->rows[(size_t)index].peak;
            if (peak > 0.0f) {
                // Square root, so the quiet end is visible. A linear bar
                // spends almost all of its length on the loud half, and the
                // question this answers is usually "is ANYTHING arriving".
                float shown = sqrtf(peak < 0.0f ? 0.0f : (peak > 1.0f ? 1.0f : peak));
                RECT fill = bar;
                fill.right = bar.left + (LONG)((bar.right - bar.left) * shown);
                HBRUSH b = CreateSolidBrush(peak > 0.98f ? hot : lit);
                FillRect(di->hDC, &fill, b);
                DeleteObject(b);
            }
            return TRUE;
        }

        case MainWindow::kRebuildMsg:
            if (st) Rebuild(hwnd, st);
            return 0;

        case MainWindow::kRefreshMsg:
            if (st) Refresh(hwnd, st);
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (st && st->ctx->theme) {
                if (const ThemeState* t = st->ctx->theme()) {
                    int id = GetDlgCtrlID((HWND)lp);
                    int field = RowFieldFromId(id), index = RowIndexFromId(id);
                    if (msg == WM_CTLCOLORSTATIC && t->colors.dark) {
                        if (id == kHeading || id == kDeviceHeading || id == kInputHeading) {
                            SetTextColor((HDC)wp, t->colors.accent);
                            SetBkColor((HDC)wp, t->colors.bg);
                            return (LRESULT)t->bgBrush;
                        }
                        // Red or green speaker, so a muted row is visible down
                        // the column without reading it.
                        if (field == 4 && index >= 0 && index < (int)st->rows.size()) {
                            SetTextColor((HDC)wp, st->rows[(size_t)index].muted ? t->colors.bad
                                                                                : t->colors.good);
                            SetBkColor((HDC)wp, t->colors.bg);
                            return (LRESULT)t->bgBrush;
                        }
                        // The selected device's name carries the accent, so it
                        // is obvious which row the rename box is pointed at; a
                        // hidden row shown by Show hidden reads dim, so the two
                        // states are never confused.
                        if (field == 0 && index >= 0 && index < (int)st->rows.size()) {
                            if (index == st->selected) {
                                SetTextColor((HDC)wp, t->colors.accent);
                                SetBkColor((HDC)wp, t->colors.bg);
                                return (LRESULT)t->bgBrush;
                            }
                            if (st->rows[(size_t)index].hidden) {
                                SetTextColor((HDC)wp, t->colors.muted);
                                SetBkColor((HDC)wp, t->colors.bg);
                                return (LRESULT)t->bgBrush;
                            }
                        }
                        // A battery worth acting on reads red; the rest are
                        // quiet, so the column only speaks when it matters.
                        if (field == kFieldBattery && index >= 0 && index < (int)st->rows.size()) {
                            wchar_t cell[16] = {};
                            GetWindowTextW((HWND)lp, cell, 16);
                            int pct = _wtoi(cell);
                            SetTextColor((HDC)wp, (cell[0] && pct <= 20) ? t->colors.bad
                                                                         : t->colors.muted);
                            SetBkColor((HDC)wp, t->colors.bg);
                            return (LRESULT)t->bgBrush;
                        }
                        if ((field == kFieldStatus || id == kRenameHint || id == kRenameLabel) &&
                            index != -1) {
                            SetTextColor((HDC)wp, t->colors.muted);
                            SetBkColor((HDC)wp, t->colors.bg);
                            return (LRESULT)t->bgBrush;
                        }
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
            if (st && st->glyphFont) DeleteObject(st->glyphFont);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
    } catch (...) {}   // no-crash rule
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND CreateMixerTab(HWND parent, UiContext* ctx) {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VSCROLL, 0, 0, 100, 100,
                           parent, nullptr, inst, ctx);
}

} // namespace mdxm
