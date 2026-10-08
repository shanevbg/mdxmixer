#include "ui/topmost.h"
#include <string>

namespace mdxm {

bool IsFollowedTopmostApp(HWND hwnd) {
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    // QUERY_LIMITED_INFORMATION, not QUERY_INFORMATION: the limited right is
    // granted for a process at the same integrity level without any special
    // privilege, and the full one is not. Asking for more than is needed is
    // how this would come back empty on a machine where it matters.
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const bool got = QueryFullProcessImageNameW(proc, 0, path, &len) != 0;
    CloseHandle(proc);
    if (!got) return false;
    std::wstring name(path, len);
    const size_t slash = name.find_last_of(L'\\');
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);
    return name == L"mdropdx12.exe";
}

// The first TOPMOST window above `hwnd` that actually covers part of it.
//
// GW_HWNDPREV walks toward the front and stops at the first hit, so this is a
// few iterations rather than an enumeration of the desktop.
//
// Only topmost windows count. Once `hwnd` holds the bit, nothing below the
// band can obscure it, so an ordinary window appearing over it means the bit
// is missing rather than that someone out-ranked us. Windows of THIS process
// are skipped: the mixer sitting over its own overlay is the user's
// arrangement, not a theft.
static HWND FirstCovering(HWND hwnd, const RECT& mine) {
    const DWORD selfPid = GetCurrentProcessId();
    for (HWND h = GetWindow(hwnd, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV)) {
        if (!IsWindowVisible(h)) continue;
        if (!(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) continue;
        RECT rc = {}, hit = {};
        if (!GetWindowRect(h, &rc) || !IntersectRect(&hit, &rc, &mine)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == selfPid) continue;
        return h;
    }
    return nullptr;
}

bool CoveredByFollowedTopmost(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    RECT mine = {};
    if (!GetWindowRect(hwnd, &mine)) return false;
    return IsFollowedTopmostApp(FirstCovering(hwnd, mine));
}

void ReassertTopmost(HWND hwnd, bool adaptive) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return;

    RECT mine = {};
    if (!GetWindowRect(hwnd, &mine)) return;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool inBand = (ex & WS_EX_TOPMOST) != 0;

    const HWND fore = GetForegroundWindow();
    DWORD forePid = 0;
    if (fore) GetWindowThreadProcessId(fore, &forePid);

    HWND above = FirstCovering(hwnd, mine);

    bool byForeground = false;
    if (above && fore) {
        DWORD abovePid = 0;
        GetWindowThreadProcessId(above, &abovePid);
        byForeground = (above == fore) || (forePid && abovePid == forePid);
    }
    const bool byFollowed = adaptive && above && IsFollowedTopmostApp(above);
    const TopmostAction plan =
        PlanTopmost(inBand, above != nullptr, byForeground, byFollowed);
    if (plan == TopmostAction::None) return;

    // Below the followed window rather than over it. Placing a window after a
    // TOPMOST one puts it in the band too, so this both gets us up here and
    // keeps us out of mdx12's way in a single call.
    const HWND insertAfter =
        (plan == TopmostAction::BelowFollowed) ? above : HWND_TOPMOST;

    // CLICK-THROUGH COMES OFF FOR THE CALL.
    //
    // A window carrying WS_EX_TRANSPARENT cannot be promoted into the band:
    // SetWindowPos returns TRUE, GetLastError is 0, and the bit does not
    // change. MDropDX12 measured which bit matters -- drop LAYERED only,
    // refused; drop TRANSPARENT only, granted -- and both come off here
    // because dropping both was measured to work too and it keeps this
    // identical to the overlay's own style pass, which already does this.
    const LONG_PTR bare = ex & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
    const bool wasLayered = (ex & WS_EX_LAYERED) != 0;
    BYTE alpha = 255;
    COLORREF key = 0;
    DWORD lflags = 0;
    if (wasLayered) GetLayeredWindowAttributes(hwnd, &key, &alpha, &lflags);

    if (bare != ex) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, bare);
    SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (bare != ex) {
        // OR the original bits back onto whatever the promotion left, so the
        // topmost bit it just set is not written away again.
        const LONG_PTR now = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          now | (ex & (WS_EX_LAYERED | WS_EX_TRANSPARENT)));
    }
    if (wasLayered)
        SetLayeredWindowAttributes(hwnd, key, alpha, lflags ? lflags : LWA_ALPHA);
}

} // namespace mdxm
