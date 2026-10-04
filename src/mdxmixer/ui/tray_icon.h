#pragma once
// Shell_NotifyIcon wrapper. The tray icon is always present (spec Program shape);
// its callback message arrives on the owner window as kTrayMessage.
#include <windows.h>

namespace mdxm {

class TrayIcon {
public:
    static constexpr UINT kTrayMessage = WM_APP + 1;
    bool Add(HWND owner, HICON icon, const wchar_t* tip);
    void Remove();

private:
    HWND m_owner = nullptr;
    bool m_added = false;
};

} // namespace mdxm
