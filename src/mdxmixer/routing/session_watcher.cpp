#include "session_watcher.h"
#include "app/thread_guard.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <objbase.h>
#include <atomic>
#include <thread>
#include <vector>

namespace mdxm {

namespace {

// One registration's worth of COM. Created, used and released entirely on the
// watcher thread, so there is no apartment to cross.
class SessionNotification : public IAudioSessionNotification {
public:
    explicit SessionNotification(SessionWatcher::Callback* cb) : m_cb(cb) {}

    // The IAudioSessionControl handed in is NOT touched -- see the header. All
    // this says is "something started"; the receiver re-sweeps on its own
    // thread, where asking the API is safe.
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
        // A REAL SEH guard, not only try/catch, for the reason
        // device_watcher.cpp gives: this arrives on an audio-stack thread, and
        // an access violation escaping it is turned into a fatal 0xC000041D by
        // Windows with nothing in our own log to show for it.
        if (m_cb && *m_cb)
            RunGuarded([](void* p) { (*(SessionWatcher::Callback*)p)(); }, m_cb);
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IAudioSessionNotification)) {
            *ppv = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

private:
    // Points at the Impl's copy of the callback, which outlives every
    // registration: the notification object can be kept alive by the audio
    // stack for a moment after we release it, and a callback it owned by value
    // would then be destroyed underneath that.
    SessionWatcher::Callback* m_cb;
    std::atomic<ULONG> m_refs{1};
};

} // namespace

struct SessionWatcher::Impl {
    SessionWatcher::Callback cb;
    std::thread worker;
    HANDLE stopEvent = nullptr;
    HANDLE rebindEvent = nullptr;
    // Set once the FIRST registration pass has finished, so Start can return
    // "registered" rather than "a thread has been started which will get there".
    // Without it there is a window at startup where an app that begins playing
    // is missed with nothing to show it happened -- and the app layer's log
    // line would claim a watcher that was not yet watching.
    HANDLE readyEvent = nullptr;
    std::atomic<int> registered{0};

    // Worker thread only.
    struct Reg { IAudioSessionManager2* mgr; SessionNotification* note; };
    std::vector<Reg> regs;

    void Register() {
        IMMDeviceEnumerator* enumr = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&enumr)))
            return;
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                IAudioSessionManager2* mgr = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                            nullptr, (void**)&mgr))) {
                    // THE ENUMERATOR CALL IS LOAD-BEARING, and it is not
                    // obvious: on Windows, RegisterSessionNotification returns
                    // S_OK and then never delivers OnSessionCreated unless
                    // GetSessionEnumerator has been called at least once on
                    // the same manager. The result is discarded; asking is the
                    // whole point.
                    IAudioSessionEnumerator* warmUp = nullptr;
                    if (SUCCEEDED(mgr->GetSessionEnumerator(&warmUp)) && warmUp)
                        warmUp->Release();
                    auto* note = new SessionNotification(&cb);
                    if (SUCCEEDED(mgr->RegisterSessionNotification(note))) {
                        regs.push_back({ mgr, note });
                    } else {
                        note->Release();
                        mgr->Release();
                    }
                }
                dev->Release();
            }
            coll->Release();
        }
        enumr->Release();
        registered.store((int)regs.size());
    }

    void Unregister() {
        // Unregister BEFORE releasing the manager, and release the
        // notification only after the manager has let go of it.
        for (auto& r : regs) {
            if (r.mgr) {
                r.mgr->UnregisterSessionNotification(r.note);
                r.mgr->Release();
            }
            if (r.note) r.note->Release();
        }
        regs.clear();
        registered.store(0);
    }

    // The watcher thread: an MTA of its own, so neither the registration nor
    // the objects behind it ever cross into the UI thread's STA.
    void Run() {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            Register();
            SetEvent(readyEvent);   // Start is waiting on this
            for (;;) {
                HANDLE waits[2] = { stopEvent, rebindEvent };
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) break;
                if (w == WAIT_OBJECT_0 + 1) {
                    ResetEvent(rebindEvent);
                    Unregister();
                    Register();
                    continue;
                }
                break;   // a failed wait is not something to spin on
            }
            Unregister();
        } catch (...) {
            Unregister();
            SetEvent(readyEvent);   // never leave Start waiting out its timeout
        }
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }
};

SessionWatcher::~SessionWatcher() { Stop(); }

bool SessionWatcher::Start(Callback onSessionCreated) {
    Stop();
    auto* impl = new Impl();
    impl->cb = std::move(onSessionCreated);
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->rebindEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->stopEvent || !impl->rebindEvent || !impl->readyEvent) {
        if (impl->stopEvent) CloseHandle(impl->stopEvent);
        if (impl->rebindEvent) CloseHandle(impl->rebindEvent);
        if (impl->readyEvent) CloseHandle(impl->readyEvent);
        delete impl;
        return false;
    }
    m_impl = impl;
    impl->worker = std::thread([impl] { impl->Run(); });
    // Wait for the first registration pass, so this returns REGISTERED. The
    // enumeration it is waiting on is the same one the engine runs several
    // times during startup anyway; the timeout is a backstop against an audio
    // stack that has stopped answering, not an expected path -- the watcher
    // thread carries on either way.
    WaitForSingleObject(impl->readyEvent, 5000);
    return true;
}

void SessionWatcher::Stop() {
    if (!m_impl) return;
    Impl* impl = m_impl;
    m_impl = nullptr;
    SetEvent(impl->stopEvent);
    if (impl->worker.joinable()) impl->worker.join();
    CloseHandle(impl->stopEvent);
    CloseHandle(impl->rebindEvent);
    CloseHandle(impl->readyEvent);
    delete impl;
}

void SessionWatcher::Rebind() {
    if (m_impl) SetEvent(m_impl->rebindEvent);
}

int SessionWatcher::Registered() const {
    return m_impl ? m_impl->registered.load() : 0;
}

} // namespace mdxm
