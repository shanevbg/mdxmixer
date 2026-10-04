#include "capture_stream.h"
#include "app/thread_guard.h"
#include "device/endpoints.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <process.h>
#include <atomic>
#include <vector>

#pragma comment(lib, "avrt.lib")

namespace mdxm {

struct CaptureStream::Impl {
    std::wstring endpointId;
    bool loopback = false;
    OnFrames cb;

    HANDLE hThread = nullptr;
    HANDLE hStop = nullptr;        // manual-reset
    HANDLE hReady = nullptr;       // manual-reset: init finished (ok or not)
    HANDLE hAudioEvent = nullptr;  // auto-reset, WASAPI event callback
    std::atomic<bool> ok{false};
    std::atomic<bool> invalidated{false};
    std::wstring error;
    uint32_t rate = 0;

    static void RunBody(void* p) { ((Impl*)p)->Run(); }

    static unsigned __stdcall ThreadMain(void* p) {
        auto* self = (Impl*)p;
        // No-crash rule: RunGuarded is SEH + catch(...), so an access violation
        // inside a driver callback kills this stream, not the process.
        if (!RunGuarded(&RunBody, self) && self->error.empty())
            self->error = L"capture thread faulted";
        SetEvent(self->hReady);   // in case init faulted before signaling
        return 0;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumr = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* wfx = nullptr;
        DWORD avrtTask = 0;
        HANDLE hAvrt = nullptr;

        auto fail = [&](const wchar_t* what, HRESULT hr) {
            wchar_t buf[64];
            swprintf(buf, 64, L" (hr=0x%08X)", (unsigned)hr);
            error = std::wstring(what) + buf;
            SetEvent(hReady);
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)&enumr);
        if (SUCCEEDED(hr)) hr = enumr->GetDevice(endpointId.c_str(), &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&wfx);
        StreamFormat fmt;
        if (SUCCEEDED(hr)) {
            fmt = ParseMixFormat(wfx);
            if (fmt.sample == StreamFormat::Sample::Unsupported) {
                error = L"unsupported mix format — refusing to read garbage bytes";
                SetEvent(hReady);
                goto cleanup;
            }
            rate = fmt.rate;
            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, wfx, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(hAudioEvent);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) { fail(L"capture init failed", hr); goto cleanup; }

        hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrtTask);
        ok = true;
        SetEvent(hReady);

        {
            std::vector<float> scratch;
            const uint16_t srcCh = fmt.channels ? fmt.channels : 2;
            HANDLE waits[2] = { hStop, hAudioEvent };
            for (;;) {
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5);   // 5 ms timeout: loopback
                if (w == WAIT_OBJECT_0) break;                           // events can be unreliable
                UINT32 packet = 0;
                hr = capture->GetNextPacketSize(&packet);
                while (SUCCEEDED(hr) && packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (FAILED(hr)) break;
                    if (scratch.size() < (size_t)frames * 2) scratch.resize((size_t)frames * 2);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        memset(scratch.data(), 0, (size_t)frames * 2 * sizeof(float));
                    } else if (fmt.sample == StreamFormat::Sample::F32) {
                        const float* src = (const float*)data;
                        for (UINT32 i = 0; i < frames; ++i) {
                            float l = src[i * srcCh];
                            float r = srcCh >= 2 ? src[i * srcCh + 1] : l;
                            scratch[i * 2] = l;
                            scratch[i * 2 + 1] = r;
                        }
                    } else { // I16
                        const int16_t* src = (const int16_t*)data;
                        for (UINT32 i = 0; i < frames; ++i) {
                            float l = src[i * srcCh] / 32768.0f;
                            float r = srcCh >= 2 ? src[i * srcCh + 1] / 32768.0f : l;
                            scratch[i * 2] = l;
                            scratch[i * 2 + 1] = r;
                        }
                    }
                    if (cb) cb(scratch.data(), frames);
                    capture->ReleaseBuffer(frames);
                    hr = capture->GetNextPacketSize(&packet);
                }
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            }
        }

    cleanup:
        if (client) client->Stop();
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        if (capture) capture->Release();
        if (wfx) CoTaskMemFree(wfx);
        if (client) client->Release();
        if (dev) dev->Release();
        if (enumr) enumr->Release();
        CoUninitialize();
    }
};

CaptureStream::~CaptureStream() { Stop(); }

bool CaptureStream::Start(const std::wstring& endpointId, bool loopback, OnFrames cb, std::wstring* err) {
    Stop();
    m_impl = new Impl;
    m_impl->endpointId = endpointId;
    m_impl->loopback = loopback;
    m_impl->cb = std::move(cb);
    m_impl->hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->hReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_impl->hAudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->hThread = (HANDLE)_beginthreadex(nullptr, 0, &Impl::ThreadMain, m_impl, 0, nullptr);
    if (!m_impl->hThread) {
        if (err) *err = L"thread creation failed";
        Stop();
        return false;
    }
    WaitForSingleObject(m_impl->hReady, 10000);
    if (!m_impl->ok) {
        if (err) *err = m_impl->error.empty() ? L"capture start timed out" : m_impl->error;
        Stop();
        return false;
    }
    m_rate = m_impl->rate;
    return true;
}

void CaptureStream::Stop() {
    if (!m_impl) return;
    if (m_impl->hStop) SetEvent(m_impl->hStop);
    if (m_impl->hThread) {
        if (WaitForSingleObject(m_impl->hThread, 5000) != WAIT_OBJECT_0) {
            // Wedged in a driver call. Freeing the Impl now is a use-after-free
            // the moment it returns: it would write self->error, SetEvent a
            // closed handle, and release COM pointers out of freed memory. Leak
            // it — the timeout exists for exactly this, and a leak at teardown
            // beats heap corruption.
            m_impl = nullptr;
            m_rate = 0;
            return;
        }
        CloseHandle(m_impl->hThread);
    }
    if (m_impl->hStop) CloseHandle(m_impl->hStop);
    if (m_impl->hReady) CloseHandle(m_impl->hReady);
    if (m_impl->hAudioEvent) CloseHandle(m_impl->hAudioEvent);
    delete m_impl;
    m_impl = nullptr;
}

bool CaptureStream::Invalidated() const { return m_impl && m_impl->invalidated.load(); }

} // namespace mdxm
