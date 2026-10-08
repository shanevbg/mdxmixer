#include "pipe_server.h"
#include "app/thread_guard.h"
#include "ipc/protocol.h"   // kPushIntervalDefaultMs — the rate a subscriber gets unasked
#include <windows.h>
#include <sddl.h>
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
// The pipe's security, and it is load-bearing from the moment mdxmixer runs
// elevated.
//
// A named pipe created with null attributes takes the CREATOR's integrity
// level. Elevated, that is High -- and Windows' no-write-up rule then stops
// every ordinary process from writing to it. MDropDX12, the `mdxmixer` refresh
// tool and anything else driving this program all run at Medium, so the entire
// control surface would go quiet the moment elevation was turned on, with
// "access denied" on connect and nothing in the log to explain it.
//
// So the label is set explicitly to Medium, which is where the clients live,
// and the DACL names who may use it rather than relying on a default that
// changes with the token:
//
//   SY  Local System          all access
//   BA  Built-in Administrators   all access
//   IU  Interactive Users      all access -- the person at the keyboard, which
//                              is who every client of this pipe belongs to
//   ML  Medium, no-write-up    a process below Medium cannot write to it
//
// Deliberately NOT a null DACL: that reads as "no restrictions" and grants
// everyone everything, which is the opposite of what an elevated server wants.
constexpr wchar_t kPipeSddl[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;ME)";

// Builds the descriptor once; the caller passes the result to every
// CreateNamedPipe. Null on failure, which falls back to default security --
// correct for an unelevated build and merely restrictive for an elevated one.
class PipeSecurity {
public:
    PipeSecurity() {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kPipeSddl, SDDL_REVISION_1, &m_sd, nullptr))
            m_sd = nullptr;
        m_sa.nLength = sizeof(m_sa);
        m_sa.lpSecurityDescriptor = m_sd;
        m_sa.bInheritHandle = FALSE;
    }
    ~PipeSecurity() { if (m_sd) LocalFree(m_sd); }
    SECURITY_ATTRIBUTES* Get() { return m_sd ? &m_sa : nullptr; }

private:
    PSECURITY_DESCRIPTOR m_sd = nullptr;
    SECURITY_ATTRIBUTES m_sa{};
};

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
    // This client's own push rate, and when it is next owed one. See the
    // AnySubscriberDue / BroadcastDue / MarkPushed trio in the header.
    //
    // nextDueMs == 0 means "owed one now", which is what a client that has
    // just subscribed should get rather than waiting out an interval it was
    // not present for.
    std::atomic<unsigned> intervalMs{ (unsigned)kPushIntervalDefaultMs };
    std::atomic<unsigned> nextDueMs{0};
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
        // Built once for the life of the loop: every instance of the pipe gets
        // the same descriptor, and the SDDL is parsed once rather than per
        // connection.
        PipeSecurity security;
        HANDLE hConnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (WaitForSingleObject(hShutdown, 0) != WAIT_OBJECT_0) {
            HANDLE hPipe = CreateNamedPipeW(pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES, kBufBytes, kBufBytes, 0, security.Get());
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
            // -1 means "the client did not ask", which leaves the rate it
            // already had -- a client may subscribe once and never mention a
            // rate again.
            int wantInterval = -1;
            std::vector<std::wstring> replies;
            try { if (handler) replies = handler(msg, &wantSub, &wantInterval); } catch (...) {}
            const bool wasSubscribed = ctx->subscribed.exchange(wantSub);
            if (wantInterval > 0) ctx->intervalMs = (unsigned)wantInterval;
            // Subscribing is itself a reason to be owed a push: the first one
            // carries the whole device list, and waiting out an interval the
            // client was not present for is just a slower start.
            if (wantSub && !wasSubscribed) ctx->nextDueMs = 0;
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

// A subscriber is due when it has never been pushed to (nextDue == 0) or its
// own interval has elapsed. Unsigned subtraction, so the 49-day GetTickCount
// wrap is handled rather than hoped about.
static bool DueNow(unsigned nextDueMs, unsigned nowMs) {
    return nextDueMs == 0 || (int)(nowMs - nextDueMs) >= 0;
}

bool PipeServer::AnySubscriberDue(unsigned nowMs) const {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
    for (auto* ctx : m_impl->clients)
        if (!ctx->finished.load() && ctx->subscribed.load() &&
            DueNow(ctx->nextDueMs.load(), nowMs))
            return true;
    return false;
}

void PipeServer::BroadcastDue(const std::wstring& msg, unsigned nowMs) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
    for (auto* ctx : m_impl->clients) {
        if (ctx->finished.load() || !ctx->subscribed.load()) continue;
        if (!DueNow(ctx->nextDueMs.load(), nowMs)) continue;
        {
            std::lock_guard<std::mutex> qlock(ctx->outMutex);
            ctx->outQueue.push(msg);
        }
        SetEvent(ctx->hOutEvent);
    }
}

void PipeServer::MarkPushed(unsigned nowMs) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
    for (auto* ctx : m_impl->clients) {
        if (ctx->finished.load() || !ctx->subscribed.load()) continue;
        if (!DueNow(ctx->nextDueMs.load(), nowMs)) continue;
        // Advance from the PREVIOUS due time, not from now.
        //
        // The tick is 100 ms, so "now + 250" always lands between two ticks
        // and waits for the later one: measured, a client on the default 250
        // was pushed to every 332 ms. Carrying the phase forward puts the due
        // times on 250, 500, 750 and the pushes on 300, 500, 800 -- individual
        // gaps of 200 and 300, averaging the 250 that was asked for.
        //
        // The clamp is what the naive version was protecting against: a client
        // that was not pushed to for a while -- nothing subscribed, the app in
        // the tray, the machine asleep -- must not then be owed a burst of
        // catch-up pushes it has no use for.
        const unsigned interval = ctx->intervalMs.load();
        const unsigned prev = ctx->nextDueMs.load();
        unsigned next = prev ? prev + interval : nowMs + interval;
        if ((int)(next - nowMs) <= 0) next = nowMs + interval;   // too far behind: resync
        if (next == 0) next = 1;            // 0 is reserved for "owed now"
        ctx->nextDueMs = next;
    }
}

bool PipeServer::HasSubscribers() const {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->clientsMutex);
    for (auto* ctx : m_impl->clients)
        if (!ctx->finished.load() && ctx->subscribed.load()) return true;
    return false;
}

} // namespace mdxm
