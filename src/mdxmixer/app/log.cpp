#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mdxm {

namespace {
std::mutex g_mutex;
std::wstring g_path;
int g_level = 2;
constexpr long kRotateBytes = 5 * 1024 * 1024;
}

void LogInit(const std::wstring& exeDir, int level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
    std::wstring dir = exeDir + L"\\log";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_path = dir + L"\\mdxmixer.log";
}

void LogSetLevel(int level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

void Log(int level, const wchar_t* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty() || level > g_level) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > (DWORD)kRotateBytes) {
        std::wstring old = g_path + L".old";
        DeleteFileW(old.c_str());
        MoveFileW(g_path.c_str(), old.c_str());
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%02d:%02d:%02d.%03d [%d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fwprintf(f, L"\n");
    fclose(f);
}

} // namespace mdxm
