#pragma once

#include <windows.h>
#include <stdarg.h>
#include <locale.h>

// Bounded string formatting whose size cannot be stated wrongly, and which
// cannot kill the process (issues 145 and 147).
//
// Two separate defects converged on this header.
//
// _CRT_NON_CONFORMING_SWPRINTFS used to be defined in every configuration,
// which let both spellings of swprintf compile side by side:
//
//     swprintf(dest, count, fmt, ...)   bounded
//     swprintf(dest, fmt, ...)          UNBOUNDED -- no count at all
//
// 149 sites used the second. The overruns that reached users were not exotic:
// a long `img=` line in sprites.ini put 771 characters into a wchar_t[512],
// and a preset naming a sampler with a 200-character identifier ran 546 bytes
// past a MAX_PATH filename buffer.
//
// The other 433 sites had already been "fixed" by moving to swprintf_s and
// sprintf_s, which is worse. Measured, not assumed, on this project's own /MT
// Release configuration:
//
//     swprintf_s(wchar_t[8], L"%s", L"0123456789ABCDEF")
//         -> never returns. Process exits 0xC0000409 via __fastfail.
//
//     _snwprintf_s(wchar_t[8], 8, _TRUNCATE, L"%s", L"0123456789ABCDEF")
//         -> returns -1, writes "0123456", null-terminated, process lives.
//
// The _s family invokes the invalid-parameter handler when the result does not
// fit, and _set_invalid_parameter_handler is installed NOWHERE in this program,
// so the default handler terminates it. A string too long for its box must not
// be able to kill the render thread of a program whose stated contract is never
// to crash. That is why none of these is built on swprintf_s.
//
// The count is DEDUCED from the destination array -- the same constraint issue
// 19 applied to wasabiApiLangString, for the same reason. A caller cannot pass
// a pointer (deduction fails) and cannot state a length (there is no parameter
// for one).
//
// ON TRUNCATION these write as much as fits, null-terminate, return -1, and log
// once per second at WARN. They never assert, throw, or terminate, in any
// configuration -- Debug included, because a modal assert on the render thread
// is its own kind of failure. The log line is what keeps a truncation from
// being silent: replacing a loud crash with a quiet wrong answer would trade
// one defect for a less findable one.

namespace fmt_detail {
  int VFormatInto(wchar_t* dest, int cchDest, const wchar_t* fmt, va_list ap);
  int VFormatResInto(wchar_t* dest, int cchDest, int id, va_list ap);
  int VFormatIntoLocale(wchar_t* dest, int cchDest, const wchar_t* fmt, _locale_t loc, va_list ap);
  int VFormatIntoA(char* dest, int cchDest, const char* fmt, va_list ap);

  bool CopyInto(wchar_t* dest, int cchDest, const wchar_t* src);
  bool CopyIntoA(char* dest, int cchDest, const char* src);
  bool AppendInto(wchar_t* dest, int cchDest, const wchar_t* src);
  bool AppendIntoA(char* dest, int cchDest, const char* src);
}

// Format into `dest`, truncating to fit. Returns the character count written,
// or -1 if the result was truncated or the format was null.
template <size_t N>
inline int FormatTo(wchar_t (&dest)[N], _In_z_ _Printf_format_string_ const wchar_t* fmt, ...) {
  static_assert(N > 1, "FormatTo: destination must hold a character and a terminator");
  static_assert(N <= 0x7fffffff, "FormatTo: destination exceeds the CRT's int count");
  va_list ap;
  va_start(ap, fmt);
  const int n = fmt_detail::VFormatInto(dest, (int)N, fmt, ap);
  va_end(ap);
  return n;
}

// As FormatTo, for a caller that already holds a va_list -- i.e. a varargs
// function of its own forwarding to this one. Only AudioCapture's AudioErr
// needs it, which is why there is no narrow counterpart.
template <size_t N>
inline int FormatToV(wchar_t (&dest)[N], _In_z_ const wchar_t* fmt, va_list ap) {
  static_assert(N > 1, "FormatToV: destination must hold a character and a terminator");
  static_assert(N <= 0x7fffffff, "FormatToV: destination exceeds the CRT's int count");
  return fmt_detail::VFormatInto(dest, (int)N, fmt, ap);
}

// The narrow-character form. Used by the diagnostic paths that build char
// buffers for OutputDebugStringA and the CSV metric dumps; sprintf_s carries
// exactly the same fatal truncation as swprintf_s.
template <size_t N>
inline int FormatToA(char (&dest)[N], _In_z_ _Printf_format_string_ const char* fmt, ...) {
  static_assert(N > 1, "FormatToA: destination must hold a character and a terminator");
  static_assert(N <= 0x7fffffff, "FormatToA: destination exceeds the CRT's int count");
  va_list ap;
  va_start(ap, fmt);
  const int n = fmt_detail::VFormatIntoA(dest, (int)N, fmt, ap);
  va_end(ap);
  return n;
}

// Format into `dest` using string resource `id` AS THE FORMAT, truncating to
// fit. Returns -1 if truncated or if the resource is missing.
//
// The resource is fetched and consumed in one step deliberately. Every one of
// these call sites used to stage the format through a local wchar_t fmt[64]
// first, and wasabiApiLangString truncates silently -- engine.cpp's
// "successfully created VS0/VS1" format had seven characters of slack against
// its four %d arguments, so a longer translation would have been cut mid-%d,
// leaving a bare '%' and turning a merely-trusted format into undefined
// behaviour. With no staging buffer there is nothing to cut.
//
// NOTE what this does NOT check: that the resource's specifiers match the
// arguments supplied. C++ cannot see inside a resource. That is checked
// instead by private/tools/rc-format-check, which reads engine.rc and every
// call site -- run it when you edit either.
template <size_t N>
inline int FormatResTo(wchar_t (&dest)[N], int id, ...) {
  static_assert(N > 1, "FormatResTo: destination must hold a character and a terminator");
  static_assert(N <= 0x7fffffff, "FormatResTo: destination exceeds the CRT's int count");
  va_list ap;
  va_start(ap, id);
  const int n = fmt_detail::VFormatResInto(dest, (int)N, id, ap);
  va_end(ap);
  return n;
}

// As FormatTo, but formatting in an explicit locale. Argument order follows the
// CRT's own _swprintf_l, so a converted call reads the same as it did.
//
// Only ConfigStore needs this, and its reason is worth keeping next to the
// declaration: settings.ini floats are written and read back in the C locale on
// purpose, because a machine set to a comma decimal separator would otherwise
// write "1,500000" and read it back as 1. The writer and the reader
// (_swscanf_l) have to agree, so the locale cannot be dropped just to reuse the
// simpler overload.
template <size_t N>
inline int FormatToLocale(wchar_t (&dest)[N], _In_z_ const wchar_t* fmt, _locale_t loc, ...) {
  static_assert(N > 1, "FormatToLocale: destination must hold a character and a terminator");
  static_assert(N <= 0x7fffffff, "FormatToLocale: destination exceeds the CRT's int count");
  va_list ap;
  va_start(ap, loc);
  const int n = fmt_detail::VFormatIntoLocale(dest, (int)N, fmt, loc, ap);
  va_end(ap);
  return n;
}

// ---------------------------------------------------------------------------
// Copying and appending (issue 149)
//
// The same two defects as the formatters, in the family that FILLS the buffers
// the formatters read. #145 and #147 bounded the consumers; these are the
// sources, and in at least one case the copy happens first, so bounding the
// consumer never helped:
//
//   lstrcpyW / wcscpy / lstrcatW    no size at all. engine_messages.cpp copied
//                                   an IPC-supplied `text=` parameter of any
//                                   length into a wchar_t[512] member.
//   wcscpy_s / wcscat_s             MEASURED: 16 characters into a wchar_t[8]
//                                   exits 0xC0000409 via __fastfail, exactly
//                                   like swprintf_s. Not safer, just louder
//                                   about being unsafe.
//   wcsncpy / strncpy               bounded but do NOT null-terminate when the
//                                   source fills the buffer.
//
// CopyTo and AppendTo deduce the size, truncate, ALWAYS null-terminate, return
// false when they had to truncate, and cannot terminate the process. A null
// source is an empty string rather than a crash, which several call sites were
// relying on lstrcpyW not to do.

// Copies `src` into `dest`, truncating to fit. Returns false if truncated.
template <size_t N>
inline bool CopyTo(wchar_t (&dest)[N], const wchar_t* src) {
  static_assert(N > 1, "CopyTo: destination must hold a character and a terminator");
  return fmt_detail::CopyInto(dest, (int)N, src);
}

template <size_t N>
inline bool CopyToA(char (&dest)[N], const char* src) {
  static_assert(N > 1, "CopyToA: destination must hold a character and a terminator");
  return fmt_detail::CopyIntoA(dest, (int)N, src);
}

// Appends `src` to whatever `dest` already holds, truncating to fit. Returns
// false if truncated. `dest` must already be null-terminated.
template <size_t N>
inline bool AppendTo(wchar_t (&dest)[N], const wchar_t* src) {
  static_assert(N > 1, "AppendTo: destination must hold a character and a terminator");
  return fmt_detail::AppendInto(dest, (int)N, src);
}

template <size_t N>
inline bool AppendToA(char (&dest)[N], const char* src) {
  static_assert(N > 1, "AppendToA: destination must hold a character and a terminator");
  return fmt_detail::AppendIntoA(dest, (int)N, src);
}
