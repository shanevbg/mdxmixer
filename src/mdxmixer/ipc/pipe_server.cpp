#include "pipe_server.h"
#include "app/thread_guard.h"
#include <windows.h>
#include <process.h>
#include <atomic>
#include <mutex>
#include <queue>

namespace mdxm {

namespace {
constexpr DWORD kBufBytes = 64 * 1024;

// Wait for a thread WITHOUT starving inter-thread SendMessage.
//
// The app marshals every IPC request onto the UI thread with SendMessage, and
// the UI thread is also the one that calls Stop(). WaitForSingleObject does not
// dispatch sent messages, so the client thread parks in SendMessage forever
// while Stop parks waiting for that same thread: a permanent deadlock on exit.
// MsgWaitForMultipleObjectsEx lets the system deliver the sent message while we
// wait, so the handler returns and the thread can finish.
// Returns true when the thread ended, false on timeout.
bool WaitThreadPumping(HANDLE thread, DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD remaining = now >= deadline ? 0 : (DWORD)(deadline - now);
        DWORD w = MsgWaitForMultipleObjectsEx(1, &thread, remaining, QS_SENDMESSAGE,
                                              MWMO_INPUTAVAILABLE);
        if (w == WAIT_OBJECT_0) return true;
        if (w == WAIT_TIMEOUT) return false;
        // WAIT_OBJECT_0 + 1: a sent message arrived. It is dispatched inside the
        // wait; peeking keeps the queue moving without eating posted messages.
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    }
}
} // namespace

struct PipeClientContext {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    HANDLE hThread = nullptr;
    HANDLE hOutEvent = nullptr;              // auto-reset: outgoing queued
    std::queue<std::wstring> outQueue;
    std::mutex outMutex;
    std::atomic<bool> subscribed{false};
    PipeServer::Impl* server = nullptr;
    std::atomic<bool> finished{false};
    // Set when Stop() gave up waiting for this thread: the context is
    // deliberately leaked rather than freed under a live thread.
    std::atomic<bool> orphaned{false};
};

struct PipeServer::Impl {
    Handler handler;
    std::wstring pipeName;
    HANDLE hShutdown = nullptr;              // manual-reset
    HANDLE hListening = nullptr;             // manual-reset: first pipe instance exists
    HANDLE hAcceptThread = nullptr;
    std::vector<PipeClientContext*> clients; // guarded by clientsMutex
    mutable std::mutex clientsMutex;
    std::atomic<bool> running{false};

    static void AcceptBody(void* p) { ((Impl*)p)->AcceptLoop(); }
    static void ClientBody(void* p) {
        auto* ctx = (PipeClientContext*)p;
        ctx->server->ClientLoop(ctx);
    }

    // No-crash rule: SEH + catch(...) on every thread entry (thread_guard.h).
    static unsigned __stdcall AcceptThread(void* p) {
        RunGuarded(&AcceptBody, p);
        return 0;
    }

    static unsigned __stdcall ClientThread(void* p) {
        auto* ctx = (PipeClientContext*)p;
        RunGuarded(&ClientBody, ctx);
        ctx->finished = true;
        return 0;
    }

    void AcceptLoop() {
        HANDLE hConnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (WaitForSingleObject(hShutdown, 0) != WAIT_OBJECT_0) {
            HANDLE hPipe = CreateNamedPipeW(pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES, kBufBytes, kBufBytes, 0, nullptr);
            if (hPipe == INVALID_HANDLE_VALUE) break;
            SetEvent(hListening);            // Start() unblocks: a client can connect now

            OVERLAPPED ov = {};
            ResetEvent(hConnectEvent);
            ov.hEvent = hConnectEvent;
            BOOL ok = ConnectNamedPipe(hPipe, &ov);
            DWORD gle = GetLastError();
            if (!ok && gle == ERROR_IO_PENDING) {
                HANDLE waits[2] = { hShutdown, hConnectEvent };
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) { CancelIo(hPipe); CloseHandle(hPipe); break; }
                DWORD tx = 0;
                if (!GetOverlappedResult(hPipe, &ov, &tx, FALSE)) { CloseHandle(hPipe); continue; }
            } else if (!ok && gle != ERROR_PIPE_CONNECTED) {
                CloseHandle(hPipe);
                continue;
            }

            auto* ctx = new PipeClientContext;
            ctx->hPipe = hPipe;
            ctx->hOutEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ctx->server = this;
            ctx->hThread = (HANDLE)_beginthreadex(nullptr, 0, &ClientThread, ctx, 0, nullptr);
            if (!ctx->hThread) {
                CloseHandle(ctx->hOutEvent);
                CloseHandle(hPipe);
                delete ctx;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                SweepFinishedLocked();
                clients.push_back(ctx);
            }
        }
        CloseHandle(hConnectEvent);
    }

    void ClientLoop(PipeClientContext* ctx) {
        std::vector<wchar_t> buf(kBufBytes / sizeof(wchar_t));
        HANDLE hReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool alive = true;
        while (alive && WaitForSingleObject(hShutdown, 0) != WAIT_OBJECT_0) {
            OVERLAPPED ov = {};
            ResetEvent(hReadEvent);
            ov.hEvent = hReadEvent;
            DWORD rd = 0;
            BOOL ok = ReadFile(ctx->hPipe, buf.data(), kBufBytes, &rd, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                for (;;) {
                    HANDLE waits[3] = { hShutdown, hReadEvent, ctx->hOutEvent };
                    DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
                    if (w == WAIT_OBJECT_0) { CancelIo(ctx->hPipe); alive = false; break; }
                    if (w == WAIT_OBJECT_0 + 2) { DrainOut(ctx); continue; }
                    if (!GetOverlappedResult(ctx->hPipe, &ov, &rd, FALSE)) alive = false;
                    break;
                }
                if (!alive) break;
            } else if (!ok) {
                break;   // client went away
            }
            if (rd < sizeof(wchar_t)) continue;
            std::wstring msg(buf.data(), rd / sizeof(wchar_t));
            while (!msg.empty() && msg.back() == L'\0') msg.pop_back();
            bool wantSub = ctx->subscribed.load();
            std::vector<std::wstring> replies;
            try { if (handler) replies = handler(msg, &wantSub); } catch (...) {}
            ctx->subscribed = wantSub;
            for (const auto& r : replies) {
                std::lock_guard<std::mutex> lock(ctx->outMutex);
                ctx->outQueue.push(r);
            }
            DrainOut(ctx);
        }
        DisconnectNamedPipe(ctx->hPipe);
        CloseHandle(ctx->hPipe);
        ctx->hPipe = INVALID_HANDLE_VALUE;
        CloseHandle(hReadEvent);
    }

    void DrainOut(PipeClientContext* ctx) {
        for (;;) {
            std::wstring msg;
            {
                std::lock_guard<std::mutex> lock(ctx->outMutex);
                if (ctx->outQueue.empty()) return;
                msg = std::move(ctx->outQueue.front());
                ctx->outQueue.pop();
            }
            DWORD wr = 0;
            OVERLAPPED ov = {};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = WriteFile(ctx->hPipe, msg.c_str(),
                                (DWORD)((msg.size() + 1) * sizeof(wchar_t)), &wr, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                GetOverlappedResult(ctx->hPipe, &ov, &wr, TRUE);
            CloseHandle(ov.hEvent);
        }
    }

    // clientsMutex held.
    void SweepFinishedLocked() {
        for (size_t i = clients.size(); i-- > 0;) {
            if (clients[i]->finished.load()) {
                WaitForSingleObject(clients[i]->hThread, 1000);
                CloseHandle(clients[i]->hThread);
                CloseHandle(clients[i]->hOutEvent);
                delete clients[i];
                clients.erase(clients.begin() + (ptrdiff_t)i);
            }
        }
    }
};

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(Handler handler, std::wstring* err, const std::wstring& pipeName) {
    Stop();
    m_impl = new Impl;
    m_impl->handler = std::move(handler);
    m_impl->pipeName = pipeName;
    m_impl->hShutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->hListening = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->hAcceptThread = (HANDLE)_beginthreadex(nullptr, 0, &Impl::AcceptThread, m_impl, 0, nullptr);
    if (!m_impl->hAcceptThread) {
        if (err) *err = L"accept thread creation failed";
        Stop();
        return false;
    }
    if (WaitForSingleObject(m_impl->hListening, 3000) != WAIT_OBJECT_0) {
        if (err) *err = L"pipe did not start listening (another instance running?)";
        Stop();
        return false;
    }
    m_impl->running = true;
    return true;
}

void PipeServer::Stop() {
    if (!m_impl) return;
    SetEvent(m_impl->hShutdown);
    // Nudge the accept loop off its pending ConnectNamedPipe by connecting once.
    HANDLE nudge = CreateFileW(m_impl->pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (nudge != INVALID_HANDLE_VALUE) CloseHandle(nudge);
    if (m_impl->hAcceptThread) {
        WaitThreadPumping(m_impl->hAcceptThread, 5000);
        CloseHandle(m_impl->hAcceptThread);
    }
    bool anyOrphaned = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
        for (auto* ctx : m_impl->clients) {
            if (!WaitThreadPumping(ctx->hThread, 3000)) {
                // The thread is wedged somewhere we do not control (a stuck
                // handler, a driver, a peer that stopped reading). Freeing its
                // context here is a use-after-free the moment it wakes: it would
                // touch ctx->outMutex, ctx->hPipe and ctx->finished on released
                // memory. Leak the context instead — a bounded, quiet loss at
                // shutdown beats heap corruption.
                ctx->orphaned = true;
                anyOrphaned = true;
                continue;
            }
            CloseHandle(ctx->hThread);
            CloseHandle(ctx->hOutEvent);
            delete ctx;
        }
        m_impl->clients.clear();
    }
    if (anyOrphaned) {
        // An orphaned client thread still reads Impl (its shutdown event, its
        // mutex), so the Impl has to outlive it too. Leak both together.
        m_impl = nullptr;
        return;
    }
    CloseHandle(m_impl->hShutdown);
    if (m_impl->hListening) CloseHandle(m_impl->hListening);
    delete m_impl;
    m_impl = nullptr;
}

void PipeServer::Broadcast(const std::wstring& msg) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
    for (auto* ctx : m_impl->clients) {
        if (ctx->finished.load() || !ctx->subscribed.load()) continue;
        {
            std::lock_guard<std::mutex> qlock(ctx->outMutex);
            ctx->outQueue.push(msg);
        }
        SetEvent(ctx->hOutEvent);
    }
}

int PipeServer::ClientCount() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
    int n = 0;
    for (auto* ctx : m_impl->clients)
        if (!ctx->finished.load()) ++n;
    return n;
}

} // namespace mdxm
