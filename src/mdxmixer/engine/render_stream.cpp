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
    // The event stopped firing without anything returning an error. Kept
    // apart from `invalidated` because the two are genuinely different
    // diagnoses and the log should be able to say which happened: a device
    // that was REMOVED under us, against one that is still listed and still
    // active and has simply stopped clocking. The second is what a Bluetooth
    // headset does across a Modern Standby resume, and it was invisible.
    std::atomic<bool> stalled{false};
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
            // A BOUNDED wait, and the bound is the whole fix for a measured
            // failure.
            //
            // This was INFINITE, while capture_stream.cpp has always used a
            // 5 ms timeout because "events can be unreliable". That asymmetry
            // cost Shane a morning of silence on 2026-10-04. Coming out of
            // Modern Standby, the Bluetooth headset's endpoint stayed listed,
            // stayed DEVICE_STATE_ACTIVE, and kept answering its volume and
            // its meter -- while its render event simply stopped being
            // signalled. Nothing returned an error, so `invalidated` was never
            // set, and all four places in engine.cpp that ask Invalidated()
            // were blind by construction. This thread sat here forever.
            //
            // What that looked like from outside, from MDXM_DIAG at the time:
            //
            //     MDXM_RING|id=sonar|depth=96000|drops=57654426|underruns=0
            //     MDXM_DIAGDEV|personal={...Razer}|fallback=0
            //
            // depth at the ring's full 96000-frame capacity, 57.6 million
            // frames dropped -- about twenty minutes of audio -- and
            // underruns=0, which is the line that names the fault: the mix
            // thread never once ran to find the ring empty, because MixPull is
            // driven from THIS loop. The capture end was on a Sonar virtual
            // endpoint, which survived the resume and went on filling a ring
            // nothing drained.
            //
            // The device period here is ~10 ms, so this event should fire
            // about a hundred times a second. Two consecutive 500 ms timeouts
            // is a full second of silence from that: a stall, not a hiccup.
            // Breaking out rather than only flagging it releases the device,
            // exactly as an invalidation does, and the engine restarts the
            // stream when it sees Dead().
            //
            // A false positive costs a sub-second gap while the stream
            // re-prefills and restarts. A false negative cost twenty minutes.
            constexpr DWORD kWaitMs = 500;
            constexpr int kStallTimeouts = 2;
            int timeouts = 0;
            for (;;) {
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, kWaitMs);
                if (w == WAIT_OBJECT_0) break;          // Stop()
                if (w == WAIT_TIMEOUT) {
                    if (++timeouts >= kStallTimeouts) { stalled = true; break; }
                    continue;
                }
                timeouts = 0;
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
bool RenderStream::Stalled() const { return m_impl && m_impl->stalled.load(); }

} // namespace mdxm
