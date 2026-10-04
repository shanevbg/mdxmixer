#pragma once
// The spec's no-crash rule, in one place: "top-level SEH plus std::exception
// handling on every thread".
//
// catch(...) under /EHsc does NOT catch an access violation — a faulting audio
// or pipe thread takes the whole process down with it. RunGuarded wraps a body
// in a real SEH frame AND a C++ catch, so any thread entry point becomes
// crash-proof by calling it.
//
// The body is a plain function pointer, not std::function: MSVC forbids
// __try/__except in a function that needs C++ unwinding, so the guard must own
// no objects with destructors.
struct _EXCEPTION_POINTERS;   // <windows.h>, at global scope

namespace mdxm {

using GuardedBody = void (*)(void* ctx);

// Returns true when the body ran to completion, false when a fault or an
// exception was contained.
bool RunGuarded(GuardedBody body, void* ctx);

// The filter half of the same rule, for a body that cannot move into
// RunGuarded — a window procedure, where the frame has to stay where Windows
// calls it. Use as `__except (SehCrashFilter(GetExceptionInformation(),
// L"what was running"))`: it records the code and the address, names the
// context, and swallows the fault.
//
// Ported alongside MDropDX12's tool_window.cpp, which is written against the
// equivalent in its seh_guard.h. Keeping the same spelling is what lets fixes
// be read across between the two.
int SehCrashFilter(::_EXCEPTION_POINTERS* info, const wchar_t* context);

} // namespace mdxm
