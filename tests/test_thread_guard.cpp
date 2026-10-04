#include "test_framework.h"
#include "app/thread_guard.h"
#include <stdexcept>

using namespace mdxm;

namespace {
int g_ran = 0;

void BodyOk(void*) { g_ran = 1; }

void BodyFaults(void*) {
    g_ran = 2;
    volatile int* p = nullptr;
    *p = 1;              // access violation: SEH, NOT a C++ exception
}

void BodyThrows(void*) {
    g_ran = 3;
    throw std::runtime_error("boom");
}
} // namespace

MDXM_TEST_CASE(Guard_RunsBodyAndReportsClean) {
    g_ran = 0;
    CHECK(RunGuarded(&BodyOk, nullptr));   // true = body completed without fault
    CHECK(g_ran == 1);
}

MDXM_TEST_CASE(Guard_SwallowsAccessViolation) {
    // The spec's no-crash rule: "top-level SEH plus std::exception handling on
    // every thread". catch(...) under /EHsc does NOT catch an access violation,
    // so a thread body needs a real SEH frame or the process dies.
    g_ran = 0;
    CHECK(!RunGuarded(&BodyFaults, nullptr));
    CHECK(g_ran == 2);                     // body started, fault was contained
}

MDXM_TEST_CASE(Guard_SwallowsCppException) {
    g_ran = 0;
    CHECK(!RunGuarded(&BodyThrows, nullptr));
    CHECK(g_ran == 3);
}
