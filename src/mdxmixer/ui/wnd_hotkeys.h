#pragma once
// The Hotkeys window: what keys exist, what each one moves, and whether
// Windows actually gave it to us.
//
// The first subclass of the ported ToolWindow, and the reason it was ported —
// every window added after this one gets the same frame, the same theme, the
// same geometry persistence and the same pin and font buttons without asking.
#include "ui/tool_window.h"
#include "ipc/mixer_control.h"

namespace mdxm {

class HotkeysWindow : public ToolWindow {
public:
    HotkeysWindow(ToolHost* host, IMixerControl* ctl);

protected:
    TOOLWINDOW_META(L"Hotkeys", L"mdxmixerHotkeysWnd", L"HotkeysWnd",
                    9100, 9101, 9102, 460, 320)

    void    DoBuildControls() override;
    LRESULT DoCommand(HWND hWnd, int id, int code, LPARAM lParam) override;
    LRESULT DoNotify(HWND hWnd, NMHDR* pnm) override;

private:
    void RefreshList();
    void EditBinding(int index);          // -1 adds a new one
    int  SelectedRow() const;

    IMixerControl* m_ctl = nullptr;
    std::vector<HotkeyBinding> m_bindings;
    HWND m_list = nullptr;
};

} // namespace mdxm
