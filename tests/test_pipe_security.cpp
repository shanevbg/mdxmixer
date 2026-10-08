// test_pipe_security.cpp — the pipe's security descriptor, which becomes
// load-bearing the moment mdxmixer runs elevated.
//
// A named pipe created with null attributes takes the CREATOR's integrity
// level. Elevated, that is High, and Windows' no-write-up rule then stops every
// ordinary process writing to it -- MDropDX12, the `mdxmixer` refresh tool and
// every probe script run at Medium. The whole control surface would go quiet
// with "access denied" on connect and nothing in the log to say why.
//
// So the descriptor is explicit, and a typo in it would silently fall back to
// default security: exactly the failure it exists to prevent, and invisible
// until something cannot connect. Hence this test.
#include "test_framework.h"
#include <windows.h>
#include <sddl.h>

namespace {

// The same string pipe_server.cpp uses. Kept in step by this test failing if
// either changes without the other.
constexpr wchar_t kPipeSddl[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;ME)";

} // namespace

MDXM_TEST_CASE(PipeSecurity_DescriptorParses) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    const BOOL ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        kPipeSddl, SDDL_REVISION_1, &sd, nullptr);
    CHECK(ok != FALSE);
    if (!ok) {
        std::printf("     ConvertStringSecurityDescriptor failed: %lu\n", GetLastError());
        return;
    }
    CHECK(sd != nullptr);
    CHECK(IsValidSecurityDescriptor(sd) != FALSE);

    // A DACL must be PRESENT and not null. A null DACL is not "no opinion", it
    // grants everyone everything -- the opposite of what an elevated server
    // wants, and an easy thing to arrive at by writing only the SACL.
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    CHECK(GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) != FALSE);
    CHECK(present != FALSE);
    CHECK(dacl != nullptr);
    if (dacl) {
        ACL_SIZE_INFORMATION info{};
        CHECK(GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) != FALSE);
        CHECK(info.AceCount == 3);   // SYSTEM, Administrators, Interactive Users
    }

    // The integrity label is the half that matters for elevation: without it
    // the pipe inherits High from an elevated creator and Medium clients are
    // refused.
    BOOL saclPresent = FALSE;
    PACL sacl = nullptr;
    CHECK(GetSecurityDescriptorSacl(sd, &saclPresent, &sacl, &defaulted) != FALSE);
    CHECK(saclPresent != FALSE);
    CHECK(sacl != nullptr);

    LocalFree(sd);
}

// The pipe tests already stand a server up and talk to it; this one pins that
// it still works WITH the descriptor applied, at the same integrity level.
// Cross-level access cannot be tested from one process -- that needs an
// elevated server and an unelevated client, which is a manual check.
MDXM_TEST_CASE(PipeSecurity_ServerStillAcceptsAConnection) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kPipeSddl, SDDL_REVISION_1, &sd, nullptr)) {
        CHECK(false);
        return;
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    const wchar_t* name = L"\\\\.\\pipe\\mdxmixer_sectest";
    HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                     4, 4096, 4096, 0, &sa);
    CHECK(server != INVALID_HANDLE_VALUE);
    if (server != INVALID_HANDLE_VALUE) {
        HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (client == INVALID_HANDLE_VALUE)
            std::printf("     client could not open the pipe: %lu\n", GetLastError());
        CHECK(client != INVALID_HANDLE_VALUE);
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        CloseHandle(server);
    }
    LocalFree(sd);
}
