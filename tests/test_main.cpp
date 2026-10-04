#include <windows.h>
#include <stdlib.h>
#include <crtdbg.h>
#ifdef MDXM_TEST
#include "test_framework.h"
#include <cstring>

int main(int argc, char** argv) {
    // Unbuffered: when a test hangs, the output up to that point has to have
    // reached the pipe already or there is nothing to diagnose from.
    setvbuf(stdout, nullptr, _IONBF, 0);
    // A debug-CRT assertion -- an out-of-bounds vector index, say -- pops a
    // MODAL DIALOG by default, and a suite run from a script then hangs for
    // ever with no clue which test did it. Twice today that cost a round of
    // looking in the wrong place. Send those reports to stderr instead, so an
    // assertion fails the run loudly and the process still exits.
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    _set_error_mode(_OUT_TO_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    bool audio = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--audio") == 0) audio = true;
    return mdxm::test::RunAll(audio);
}
#endif
