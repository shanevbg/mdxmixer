#include "test_framework.h"
#include "ipc/pipe_server.h"
#include <windows.h>
#include <thread>

// Tests never share the production pipe name: a running mdxmixer holds it, and
// the client would connect to the app instead of the server under test.
static const wchar_t kTestPipe[]  = L"\\\\.\\pipe\\mdxmixer_test_echo";
static const wchar_t kTestPipe2[] = L"\\\\.\\pipe\\mdxmixer_test_stop";

using namespace mdxm;

namespace {
std::wstring PipeRequest(HANDLE h, const std::wstring& msg) {
    DWORD wr = 0;
    WriteFile(h, msg.c_str(), (DWORD)((msg.size() + 1) * sizeof(wchar_t)), &wr, nullptr);
    wchar_t buf[4096];
    DWORD rd = 0;
    if (!ReadFile(h, buf, sizeof buf, &rd, nullptr)) return L"";
    return std::wstring(buf, rd / sizeof(wchar_t) - 1);   // strip trailing null
}
} // namespace

MDXM_TEST_CASE(Pipe_EchoAndBroadcastRoundTrip) {
    PipeServer srv;
    std::wstring err;
    bool started = srv.Start([](const std::wstring& msg, bool* wantSub) {
        if (msg == L"MDXM_SUBSCRIBE=1") { *wantSub = true; return std::vector<std::wstring>{L"MDXM_OK"}; }
        return std::vector<std::wstring>{L"ECHO|" + msg};
    }, &err, kTestPipe);
    CHECK(started);

    HANDLE h = CreateFileW(kTestPipe, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    CHECK(PipeRequest(h, L"MDXM_PING") == L"ECHO|MDXM_PING");
    CHECK(PipeRequest(h, L"MDXM_SUBSCRIBE=1") == L"MDXM_OK");

    srv.Broadcast(L"MDXM_CHAN|id=game|pvol=0.5");
    wchar_t buf[4096]; DWORD rd = 0;
    CHECK(ReadFile(h, buf, sizeof buf, &rd, nullptr));      // push arrives without a request
    CHECK(std::wstring(buf, rd / sizeof(wchar_t) - 1) == L"MDXM_CHAN|id=game|pvol=0.5");

    CloseHandle(h);
    Sleep(100);
    srv.Stop();
    CHECK(srv.ClientCount() == 0);
}

namespace {
LRESULT CALLBACK MarshalWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}
} // namespace

MDXM_TEST_CASE(Pipe_StopDoesNotDeadlockOnUiMarshal) {
    // The app marshals every IPC request onto the UI thread with SendMessage,
    // and the UI thread is the one that calls Stop() on exit. WaitForSingleObject
    // does not dispatch inter-thread sent messages, so a naive Stop() blocks the
    // full join timeout with the client thread parked inside SendMessage — and
    // then frees that thread's context while it is still live.
    const wchar_t* kCls = L"mdxmixerTestMarshalWnd";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MarshalWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kCls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    CHECK(hwnd != nullptr);

    PipeServer srv;
    std::wstring err;
    CHECK(srv.Start([hwnd](const std::wstring&, bool*) {
        SendMessageW(hwnd, WM_APP + 77, 0, 0);          // blocks until this thread pumps
        return std::vector<std::wstring>{L"MDXM_OK"};
    }, &err, kTestPipe2));

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread client([ready] {
        HANDLE h = CreateFileW(kTestPipe2, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            const wchar_t msg[] = L"MDXM_PING";
            DWORD wr = 0;
            WriteFile(h, msg, sizeof(msg), &wr, nullptr);
            SetEvent(ready);
            wchar_t buf[256];
            DWORD rd = 0;
            ReadFile(h, buf, sizeof buf, &rd, nullptr);   // may fail once the server stops
            CloseHandle(h);
        } else {
            SetEvent(ready);
        }
    });
    WaitForSingleObject(ready, 3000);
    Sleep(200);                                          // handler is now parked in SendMessage

    ULONGLONG t0 = GetTickCount64();
    srv.Stop();                                          // must pump sent messages, not stall
    ULONGLONG elapsed = GetTickCount64() - t0;
    std::printf("stop took %llu ms\n", (unsigned long long)elapsed);
    CHECK(elapsed < 1500);                               // the broken path burns the full 3 s join
    CHECK(srv.ClientCount() == 0);

    client.join();
    CloseHandle(ready);
    DestroyWindow(hwnd);
    UnregisterClassW(kCls, GetModuleHandleW(nullptr));
}
