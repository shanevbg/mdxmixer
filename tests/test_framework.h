#pragma once
// Minimal test registry. No external deps. Wide-string project, narrow test names are fine.
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace mdxm { namespace test {

struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); } };
inline int g_failures = 0;
inline const char* g_current = "";

#define MDXM_TEST_CASE(Name) \
    static void Test_##Name(); \
    static mdxm::test::Registrar reg_##Name(#Name, &Test_##Name); \
    static void Test_##Name()

#define CHECK(cond) do { if (!(cond)) { \
    ++mdxm::test::g_failures; \
    std::printf("FAIL %s: %s (%s:%d)\n", mdxm::test::g_current, #cond, __FILE__, __LINE__); } } while (0)

#define CHECK_NEAR(a, b, eps) do { double va=(double)(a), vb=(double)(b); if (std::fabs(va-vb) > (eps)) { \
    ++mdxm::test::g_failures; \
    std::printf("FAIL %s: %s=%g vs %s=%g (%s:%d)\n", mdxm::test::g_current, #a, va, #b, vb, __FILE__, __LINE__); } } while (0)

// Returns process exit code. audioTests: run tests whose name starts with "Audio_" (need cables).
inline int RunAll(bool audioTests) {
    int run = 0;
    for (auto& t : Registry()) {
        bool isAudio = std::string(t.name).rfind("Audio_", 0) == 0;
        if (isAudio != audioTests) continue;
        g_current = t.name;
        // Announced BEFORE it runs, and flushed. The runner used to print only
        // on completion, so a test that hung left the previous test's result as
        // the last line and the hang pointed at the wrong name.
        std::printf("run  %s\n", t.name);
        std::fflush(stdout);
        int before = g_failures;
        try { t.fn(); }
        catch (const std::exception& e) { ++g_failures; std::printf("FAIL %s: exception %s\n", t.name, e.what()); }
        catch (...) { ++g_failures; std::printf("FAIL %s: unknown exception\n", t.name); }
        ++run;
        if (g_failures == before) std::printf("ok   %s\n", t.name);
    }
    std::printf("%d tests, %d failures\n", run, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}} // namespace mdxm::test
