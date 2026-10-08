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
    // A balloon over the tray icon. For something the user has to be told while
    // the window is hidden -- a device asking for access is the case this exists
    // for, because the dialog behind it opens without a taskbar entry to click.
    void Balloon(const wchar_t* title, const wchar_t* text);

private:
    HWND m_owner = nullptr;
    bool m_added = false;
};

} // namespace mdxm
