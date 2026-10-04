#include "tray_icon.h"
#include <shellapi.h>
#include <cwchar>

namespace mdxm {

bool TrayIcon::Add(HWND owner, HICON icon, const wchar_t* tip) {
    Remove();
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icon;
    wcsncpy_s(nid.szTip, tip, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    m_owner = owner;
    m_added = true;
    return true;
}

void TrayIcon::Remove() {
    if (!m_added) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_owner;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_added = false;
}

} // namespace mdxm
