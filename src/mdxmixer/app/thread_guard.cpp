#include "thread_guard.h"
#include "app/log.h"
#include <windows.h>

namespace mdxm {

namespace {
// Separate function: the C++ try/catch needs unwinding, which MSVC will not
// allow in the same function as __try/__except.
bool RunCatchingCpp(GuardedBody body, void* ctx) {
    try {
        body(ctx);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

bool RunGuarded(GuardedBody body, void* ctx) {
    __try {
        return RunCatchingCpp(body, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;   // access violation and friends stop here, not at the process
    }
}

int SehCrashFilter(::_EXCEPTION_POINTERS* info, const wchar_t* context) {
    // A fault with no trace is the thing this exists to prevent, so the record
    // comes first and the swallowing second.
    unsigned long code = 0;
    const void* addr = nullptr;
    if (info && info->ExceptionRecord) {
        code = info->ExceptionRecord->ExceptionCode;
        addr = info->ExceptionRecord->ExceptionAddress;
    }
    Log(1, L"contained a fault in %s: code 0x%08X at %p",
        context ? context : L"(unnamed)", code, addr);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace mdxm
