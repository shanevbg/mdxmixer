#pragma once
// Level-gated logging to log/mdxmixer.log beside the exe; 5 MB rotate to .old.
// Levels: 0 off, 1 error, 2 info (default), 3 debug.
#include <string>

namespace mdxm {

void LogInit(const std::wstring& exeDir, int level);
void LogSetLevel(int level);
void Log(int level, const wchar_t* fmt, ...);

} // namespace mdxm
