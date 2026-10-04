#include "app/format_to.h"
#include <windows.h>
#include "app/log.h"

#include <atomic>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

// mdx12 loads format strings out of its own resource table; mdxmixer has no
// string resources, so the module's own instance stands in and the resource
// form simply finds nothing. Ported rather than trimmed so the two files stay
// comparable.
static HINSTANCE api_orig_hinstance = GetModuleHandleW(nullptr);

// Declared here rather than by including utility.h, deliberately. utility.h
// pulls in d3dx9compat.h, whose stub bodies ignore their parameters and are
// therefore C4100s at /W4. Every module that formats a string now links this
// one, so including utility.h here would push that warning into every native
// test suite that touches a bounded format, and the milk2-probe harness builds
// those at /W4 /WX on purpose. A formatting primitive should be a leaf.
//
// Not "the dead DX9 layer" -- issue 17 deleted that, and this header is what
// survived it. It still supplies the D3DX math and handle types the DX12 code
// genuinely uses (D3DXVECTOR, D3DXMATRIX, D3DXHANDLE and the constant-table
// descriptors, ~200 uses), so it is not waiting to be removed.
//
// These three must match utility.h. The levels are the documented ladder
// (0 Off, 1 Error, 2 Warn, 3 Info, 4 Verbose) and have not moved.
// mdx12's two logging symbols, answered by mdxmixer's own log. Kept as these
// names so the body below is the original, truncation warnings and all.
static int g_debugLogLevel = 3;
static void DebugLogW(const wchar_t* msg, int level) {
    // mdx12's levels run the other way round from mdxmixer's: there, higher is
    // more verbose; here, 1 is an error and 3 is verbose. Warn and Error are
    // the only two used from this file.
    ::mdxm::Log(level >= 2 ? 2 : 1, L"%s", msg);
}
const int kLogError = 1;
const int kLogWarn = 2;

namespace {

// The largest format any string resource can supply. engine.rc's longest entry
// is 1548 characters; 4096 leaves headroom without putting a big array on the
// render thread's stack for every call.
const int kMaxResourceFormat = 4096;

// A truncation used to be a crash, so it was impossible to miss. Now it is a
// shorter string, which is easy to miss -- hence the log line. Rate-limited
// because several of these sites run every frame, and a per-frame WARN would
// bury debug.log rather than inform it.
std::atomic<unsigned long long> g_lastTruncationLogTick{0};

void ReportTruncation(const wchar_t* what, const wchar_t* fmt) {
  if (g_debugLogLevel < kLogWarn)
    return;
  const unsigned long long now = GetTickCount64();
  unsigned long long last = g_lastTruncationLogTick.load(std::memory_order_relaxed);
  if (last != 0 && now - last < 1000)
    return;
  if (!g_lastTruncationLogTick.compare_exchange_strong(last, now, std::memory_order_relaxed))
    return;

  // Built with the CRT directly rather than with FormatTo, so a truncation in
  // the reporting path cannot recurse into the thing being reported. DebugLogW
  // takes a ready-made string and formats with fwprintf, so it does not re-enter
  // either.
  wchar_t line[512];
  _snwprintf_s(line, _countof(line), _TRUNCATE,
               L"%s: output truncated to fit its buffer; format was \"%s\"\n",
               what, fmt ? fmt : L"(null)");
  DebugLogW(line, kLogWarn);
}

// The narrow reporter widens first, so both halves share one rate limiter and
// one log format. Truncating the widened copy is fine: this is a diagnostic.
void ReportTruncationA(const char* what, const char* src) {
  if (g_debugLogLevel < kLogWarn)
    return;
  wchar_t wwhat[64], wsrc[256];
  _snwprintf_s(wwhat, _countof(wwhat), _TRUNCATE, L"%hs", what);
  _snwprintf_s(wsrc, _countof(wsrc), _TRUNCATE, L"%hs", src ? src : "(null)");
  ReportTruncation(wwhat, wsrc);
}

}  // namespace

int fmt_detail::VFormatInto(wchar_t* dest, int cchDest, const wchar_t* fmt, va_list ap) {
  if (cchDest <= 0) return -1;
  if (!fmt) {
    dest[0] = L'\0';
    return -1;
  }
  // _TRUNCATE is what separates this from the _s family: it fills the buffer,
  // terminates it and returns -1, instead of invoking the invalid-parameter
  // handler and taking the process down with __fastfail.
  const int n = _vsnwprintf_s(dest, (size_t)cchDest, _TRUNCATE, fmt, ap);
  if (n < 0) {
    dest[cchDest - 1] = L'\0';       // belt and braces; _TRUNCATE already does this
    ReportTruncation(L"FormatTo", fmt);
  }
  return n;
}

int fmt_detail::VFormatIntoA(char* dest, int cchDest, const char* fmt, va_list ap) {
  if (cchDest <= 0) return -1;
  if (!fmt) {
    dest[0] = '\0';
    return -1;
  }
  const int n = _vsnprintf_s(dest, (size_t)cchDest, _TRUNCATE, fmt, ap);
  if (n < 0) {
    dest[cchDest - 1] = '\0';
    wchar_t wfmt[256];
    _snwprintf_s(wfmt, _countof(wfmt), _TRUNCATE, L"%hs", fmt);
    ReportTruncation(L"FormatToA", wfmt);
  }
  return n;
}

int fmt_detail::VFormatResInto(wchar_t* dest, int cchDest, int id, va_list ap) {
  if (cchDest <= 0) return -1;
  wchar_t fmt[kMaxResourceFormat];
  if (LoadStringW(api_orig_hinstance, id, fmt, kMaxResourceFormat) == 0) {
    // A missing id means a broken build, not an overlong string. Render nothing
    // rather than whatever happened to be in the buffer -- the same choice
    // wasabi.cpp makes, for the same reason.
    dest[0] = L'\0';
    if (g_debugLogLevel >= kLogError) {
      wchar_t line[128];
      _snwprintf_s(line, _countof(line), _TRUNCATE,
                   L"FormatResTo: string resource %d is missing from engine.rc\n", id);
      DebugLogW(line, kLogError);
    }
    return -1;
  }
  return fmt_detail::VFormatInto(dest, cchDest, fmt, ap);
}

int fmt_detail::VFormatIntoLocale(wchar_t* dest, int cchDest, const wchar_t* fmt,
                                  _locale_t loc, va_list ap) {
  if (cchDest <= 0) return -1;
  if (!fmt) {
    dest[0] = L'\0';
    return -1;
  }
  const int n = _vsnwprintf_s_l(dest, (size_t)cchDest, _TRUNCATE, fmt, loc, ap);
  if (n < 0) {
    dest[cchDest - 1] = L'\0';
    ReportTruncation(L"FormatToLocale", fmt);
  }
  return n;
}

bool fmt_detail::CopyInto(wchar_t* dest, int cchDest, const wchar_t* src) {
  if (cchDest <= 0) return false;
  if (!src) {
    // lstrcpyW tolerates a null source by faulting; several converted sites
    // passed a pointer that is legitimately null. An empty string is the
    // useful answer and is what they already behaved as if they got.
    dest[0] = L'\0';
    return true;
  }
  const errno_t e = wcsncpy_s(dest, (size_t)cchDest, src, _TRUNCATE);
  if (e == STRUNCATE) {
    ReportTruncation(L"CopyTo", src);
    return false;
  }
  return true;
}

bool fmt_detail::CopyIntoA(char* dest, int cchDest, const char* src) {
  if (cchDest <= 0) return false;
  if (!src) {
    dest[0] = '\0';
    return true;
  }
  const errno_t e = strncpy_s(dest, (size_t)cchDest, src, _TRUNCATE);
  if (e == STRUNCATE) {
    ReportTruncationA("CopyToA", src);
    return false;
  }
  return true;
}

bool fmt_detail::AppendInto(wchar_t* dest, int cchDest, const wchar_t* src) {
  if (cchDest <= 0) return false;
  if (!src) return true;
  const errno_t e = wcsncat_s(dest, (size_t)cchDest, src, _TRUNCATE);
  if (e == STRUNCATE) {
    ReportTruncation(L"AppendTo", src);
    return false;
  }
  return true;
}

bool fmt_detail::AppendIntoA(char* dest, int cchDest, const char* src) {
  if (cchDest <= 0) return false;
  if (!src) return true;
  const errno_t e = strncat_s(dest, (size_t)cchDest, src, _TRUNCATE);
  if (e == STRUNCATE) {
    ReportTruncationA("AppendToA", src);
    return false;
  }
  return true;
}
