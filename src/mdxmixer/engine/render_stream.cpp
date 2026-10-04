#include "render_stream.h"
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

struct RenderStream::Impl {
    std::wstring endpointId;
    PullFrames cb;

    HANDLE hThread = nullptr;
    HANDLE hStop = nullptr;
    HANDLE hReady = nullptr;
    HANDLE hAudioEvent = nullptr;
    std::atomic<bool> ok{false};
    std::atomic<bool> invalidated{false};
    std::wstring error;
    uint32_t rate = 0;

    static void RunBody(void* p) { ((Impl*)p)->Run(); }

    static unsigned __stdcall ThreadMain(void* p) {
        auto* self = (Impl*)p;
        // No-crash rule: SEH + catch(...) — a faulting mix callback must not
        // take the process down (see thread_guard.h).
        if (!RunGuarded(&RunBody, self) && self->error.empty())
            self->error = L"render thread faulted";
        SetEvent(self->hReady);
        return 0;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumr = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* wfx = nullptr;
        DWORD avrtTask = 0;
        HANDLE hAvrt = nullptr;
        UINT32 bufFrames = 0;
        StreamFormat fmt;

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
        if (SUCCEEDED(hr)) {
            fmt = ParseMixFormat(wfx);
            if (fmt.sample == StreamFormat::Sample::Unsupported) {
                error = L"unsupported mix format — refusing to write garbage bytes";
                SetEvent(hReady);
                goto cleanup;
            }
            rate = fmt.rate;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    100000, 0, wfx, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(hAudioEvent);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufFrames);
        if (SUCCEEDED(hr)) {   // prefill one full buffer of silence before Start
            BYTE* data = nullptr;
            hr = render->GetBuffer(bufFrames, &data);
            if (SUCCEEDED(hr)) hr = render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) { fail(L"render init failed", hr); goto cleanup; }

        hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &avrtTask);
        ok = true;
        SetEvent(hReady);

        {
            std::vector<float> scratch((size_t)bufFrames * 2, 0.0f);
            const uint16_t dstCh = fmt.channels ? fmt.channels : 2;
            HANDLE waits[2] = { hStop, hAudioEvent };
            for (;;) {
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) break;
                UINT32 padding = 0;
                hr = client->GetCurrentPadding(&padding);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
                if (FAILED(hr)) continue;
                UINT32 n = bufFrames - padding;
                if (n == 0) continue;
                BYTE* data = nullptr;
                hr = render->GetBuffer(n, &data);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
                if (FAILED(hr)) continue;
                if (cb) cb(scratch.data(), n);
                else memset(scratch.data(), 0, (size_t)n * 2 * sizeof(float));
                // Convert stereo float scratch to the device's format/channel count.
                // >2ch: L/R into the first pair, the rest silent. Mono: (L+R)/2.
                if (fmt.sample == StreamFormat::Sample::F32) {
                    float* dst = (float*)data;
                    if (dstCh == 2) {
                        memcpy(dst, scratch.data(), (size_t)n * 2 * sizeof(float));
                    } else if (dstCh == 1) {
                        for (UINT32 i = 0; i < n; ++i)
                            dst[i] = 0.5f * (scratch[i*2] + scratch[i*2+1]);
                    } else {
                        memset(dst, 0, (size_t)n * dstCh * sizeof(float));
                        for (UINT32 i = 0; i < n; ++i) {
                            dst[i*dstCh]     = scratch[i*2];
                            dst[i*dstCh + 1] = scratch[i*2+1];
                        }
                    }
                } else { // I16
                    int16_t* dst = (int16_t*)data;
                    auto toI16 = [](float v) {
                        if (v > 1.0f) v = 1.0f;
                        if (v < -1.0f) v = -1.0f;
                        return (int16_t)(v * 32767.0f);
                    };
                    if (dstCh == 1) {
                        for (UINT32 i = 0; i < n; ++i)
                            dst[i] = toI16(0.5f * (scratch[i*2] + scratch[i*2+1]));
                    } else {
                        memset(dst, 0, (size_t)n * dstCh * sizeof(int16_t));
                        for (UINT32 i = 0; i < n; ++i) {
                            dst[i*dstCh]     = toI16(scratch[i*2]);
                            dst[i*dstCh + 1] = toI16(scratch[i*2+1]);
                        }
                    }
                }
                hr = render->ReleaseBuffer(n, 0);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { invalidated = true; break; }
            }
        }

    cleanup:
        if (client) client->Stop();
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        if (render) render->Release();
        if (wfx) CoTaskMemFree(wfx);
        if (client) client->Release();
        if (dev) dev->Release();
        if (enumr) enumr->Release();
        CoUninitialize();
    }
};

RenderStream::~RenderStream() { Stop(); }

bool RenderStream::Start(const std::wstring& endpointId, PullFrames cb, std::wstring* err) {
    Stop();
    m_impl = new Impl;
    m_impl->endpointId = endpointId;
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
        if (err) *err = m_impl->error.empty() ? L"render start timed out" : m_impl->error;
        Stop();
        return false;
    }
    m_rate = m_impl->rate;
    return true;
}

void RenderStream::Stop() {
    if (!m_impl) return;
    if (m_impl->hStop) SetEvent(m_impl->hStop);
    if (m_impl->hThread) {
        if (WaitForSingleObject(m_impl->hThread, 5000) != WAIT_OBJECT_0) {
            // Wedged in a driver call — see the matching note in
            // CaptureStream::Stop. Leak rather than free under a live thread.
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

bool RenderStream::Invalidated() const { return m_impl && m_impl->invalidated.load(); }

} // namespace mdxm
