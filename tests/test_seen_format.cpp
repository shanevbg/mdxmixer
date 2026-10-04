#include "test_framework.h"
#include "device/seen_format.h"

using namespace mdxm;

namespace {
// Today and yesterday as the helper spells them, built the same way it does,
// so the test does not go stale at midnight or across a month boundary.
std::wstring DayOffset(int days) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    FILETIME ft = {};
    SystemTimeToFileTime(&now, &ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t += (ULONGLONG)((long long)days * 24ll * 60ll * 60ll * 10000000ll);
    ft.dwHighDateTime = (DWORD)(t >> 32);
    ft.dwLowDateTime  = (DWORD)(t & 0xFFFFFFFF);
    SYSTEMTIME s = {};
    FileTimeToSystemTime(&ft, &s);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04d-%02d-%02d", s.wYear, s.wMonth, s.wDay);
    return buf;
}
int ThisYear() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    return now.wYear;
}
} // namespace

MDXM_TEST_CASE(Seen_TodayAndYesterdayAreNamed) {
    // A date is the wrong unit for something seen an hour ago, and the two
    // most recent are the two anyone is actually choosing between.
    CHECK(FriendlySeen(DayOffset(0) + L" 13:04") == L"Today 13:04");
    CHECK(FriendlySeen(DayOffset(-1) + L" 09:25") == L"Ystrdy 09:25");
}

MDXM_TEST_CASE(Seen_TheTimeIsAlwaysKept) {
    // Three of Shane's four Sony pairings were last seen on the same date and
    // are told apart only by the hour, so a day without a time would merge
    // exactly the rows this column exists to separate.
    const std::wstring a = FriendlySeen(DayOffset(0) + L" 09:25");
    const std::wstring b = FriendlySeen(DayOffset(0) + L" 15:44");
    CHECK(a != b);
    CHECK(a.find(L"09:25") != std::wstring::npos);
    CHECK(b.find(L"15:44") != std::wstring::npos);
}

MDXM_TEST_CASE(Seen_ThisYearDropsTheYearAndOlderKeepsIt) {
    wchar_t thisYear[32], older[32];
    swprintf(thisYear, 32, L"%04d-01-02 21:03", ThisYear());
    swprintf(older, 32, L"%04d-08-23 21:03", ThisYear() - 2);
    // Four digits and a dash on every row all saying the same thing, in a
    // column already competing for width with the device name.
    CHECK(FriendlySeen(thisYear) == L"01-02 21:03");
    // Unless the year is the entire point: two identically-named pairings a
    // year apart.
    CHECK(FriendlySeen(older) == older);
}

MDXM_TEST_CASE(Seen_NothingRecordedReadsAsUnknown) {
    CHECK(FriendlySeen(L"") == L"unknown");
    CHECK(FriendlySeen(L"-") == L"unknown");
    // Too short to carry a date: handed back rather than sliced into.
    CHECK(FriendlySeen(L"2026") == L"2026");
}

MDXM_TEST_CASE(Seen_AbsoluteFormSortsByStringComparison) {
    // The whole reason the stored form is left alone: the list orders on it,
    // and "Today" would sort below "Ystrdy" and both above every real date.
    // 2026-10-03 16:07 UTC and an hour earlier.
    SYSTEMTIME a = { 2026, 10, 6, 3, 16, 7, 0, 0 };
    SYSTEMTIME b = { 2026, 10, 6, 3, 15, 7, 0, 0 };
    FILETIME fa = {}, fb = {};
    CHECK(SystemTimeToFileTime(&a, &fa) != 0);
    CHECK(SystemTimeToFileTime(&b, &fb) != 0);
    const auto pack = [](FILETIME f) {
        return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
    };
    const std::wstring sa = AbsoluteSeen(pack(fa));
    const std::wstring sb = AbsoluteSeen(pack(fb));
    CHECK(!sa.empty() && !sb.empty());
    CHECK(sa > sb);              // later sighting sorts first under `>`
    CHECK(sa.size() == 16);      // "YYYY-MM-DD HH:MM"
}

MDXM_TEST_CASE(Seen_NeverRecordedHasNoAbsoluteForm) {
    // 0 means "no record", not 1601 — a fabricated date beside a device this
    // machine has never seen would be worse than an empty cell.
    CHECK(AbsoluteSeen(0).empty());
}

MDXM_TEST_CASE(Seen_BatteryCellIsEmptyWhenThereIsNoFigure) {
    // Ordinary rather than an error: nothing wired can ever report one.
    CHECK(BatteryCell(-1).empty());
    CHECK(BatteryCell(0) == L"0%");
    CHECK(BatteryCell(82) == L"82%");
}
