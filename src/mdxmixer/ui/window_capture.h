#pragma once
// Screenshot our own window, from inside our own process.
//
// Two things make this work where an outside capture does not, both learned
// from MDropDX12's window_diag:
//
//   * PW_RENDERFULLCONTENT is what paints owner-drawn content. Without it the
//     custom-drawn tab strip, the faders and the glyphs come back blank and the
//     capture is clean and wrong.
//   * It has to run IN-PROCESS. PrintWindow driven from another process renders
//     child controls unreliably — rows come back missing or stale — and
//     screen-scraping instead captures whatever happens to be on top, which is
//     both wrong and a privacy problem.
//
// The window does not need to be focused or unobscured, so this works while the
// app sits in the background.
#include <windows.h>
#include <string>

namespace mdxm {

// Writes a PNG of hwnd's client area. Returns false and leaves no file on
// failure.
bool CaptureWindowToPng(HWND hwnd, const std::wstring& path);

} // namespace mdxm
