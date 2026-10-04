#pragma once
// "last seen" as a person reads it.
//
// PORTED from MDropDX12's seen_format.h, header-only and dependency-free for
// the same reason it is there: a date helper with two boundary cases in it is
// exactly the thing that should be tested rather than eyeballed.
//
// The one adaptation is the input. mdx12 carries "last seen" as a string on
// the wire; mdxmixer carries it as the UTC FILETIME that DeviceLevel already
// holds, so AbsoluteSeen() makes mdx12's string form out of it and
// FriendlySeen() is the original, unchanged.
#include <windows.h>
#include <string>

namespace mdxm {

// A UTC FILETIME as the sortable "YYYY-MM-DD HH:MM" mdx12 stores, in LOCAL
// time. Empty for 0, which means "never recorded" rather than 1601.
inline std::wstring AbsoluteSeen(unsigned long long utcFileTime) {
    if (utcFileTime == 0) return std::wstring();
    FILETIME utc = { (DWORD)(utcFileTime & 0xFFFFFFFF), (DWORD)(utcFileTime >> 32) };
    FILETIME local = {};
    if (!FileTimeToLocalFileTime(&utc, &local)) return std::wstring();
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&local, &st)) return std::wstring();
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// "2026-08-31 13:04" as "Today 13:04", and yesterday's as "Ystrdy 13:04".
//
// Shane asked for this on mdx12's failover allowlist: a date is the wrong
// unit for something seen an hour ago, and the two most recent are the two
// anyone is actually choosing between.
//
// The TIME is kept, never just the day. Three of his four Sony pairings were
// last seen on the same date and are told apart only by the hour, so "Today"
// alone would merge exactly the rows this column exists to separate.
//
// Display only. The value it is given stays the absolute, sortable
// "YYYY-MM-DD HH:MM", because the list orders by string comparison on that --
// rewriting the stored form would put Today below Ystrdy and both above every
// real date.
inline std::wstring FriendlySeen(const std::wstring& seen) {
    if (seen.empty() || seen == L"-") return L"unknown";
    if (seen.size() < 10) return seen;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t today[16], yesterday[16];
    swprintf(today, 16, L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);

    // Yesterday via FILETIME arithmetic rather than by decrementing the day,
    // which would have to know about month lengths and leap years.
    FILETIME ft = {};
    SystemTimeToFileTime(&now, &ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 24ull * 60ull * 60ull * 10000000ull;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    ft.dwLowDateTime  = (DWORD)(t & 0xFFFFFFFF);
    SYSTEMTIME prev = {};
    if (!FileTimeToSystemTime(&ft, &prev)) return seen;
    swprintf(yesterday, 16, L"%04d-%02d-%02d", prev.wYear, prev.wMonth, prev.wDay);

    const std::wstring day = seen.substr(0, 10);
    const std::wstring rest = seen.substr(10);   // " HH:MM", space included
    if (day == today)     return L"Today" + rest;
    if (day == yesterday) return L"Ystrdy" + rest;

    // A sighting from THIS year drops the year: "08-23 21:03".
    //
    // Four digits and a dash on every row, all saying the same thing, in a
    // column already competing for width with the device name -- and the year
    // is only ever news when it is not the current one. Shane asked for it
    // directly. An older sighting keeps its year, which is the case where the
    // year is the entire point: two identically-named pairings a year apart.
    if (seen.compare(0, 4, today, 4) == 0) return seen.substr(5);
    return seen;
}

// A battery percentage as it is shown anywhere in this window.
//
// Empty for a negative reading, which is the ordinary case rather than an
// error: plenty of devices report nothing at all, and nothing wired -- every
// speaker, every Sonar channel -- can ever report anything.
inline std::wstring BatteryCell(int percent) {
    if (percent < 0) return std::wstring();
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", percent);
    return buf;
}

} // namespace mdxm
