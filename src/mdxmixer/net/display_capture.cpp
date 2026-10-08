#include "net/display_capture.h"
#include "app/log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <shlwapi.h>     // SHCreateMemStream
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace mdxm {

namespace {

template <class T>
void Release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// The `n` out of "\\.\DISPLAYn". 0 when the name is not that shape, which is a
// skip rather than a guess: there is no VIDEOn stream for a display we cannot
// name, and picking an index instead would aim the picture at the wrong monitor.
int DeviceNumberOf(const wchar_t* deviceName) {
    const wchar_t* prefix = L"\\\\.\\DISPLAY";
    const size_t len = wcslen(prefix);
    if (!deviceName || wcsncmp(deviceName, prefix, len) != 0) return 0;
    return _wtoi(deviceName + len);
}

} // namespace

// One duplicated output, and the textures it reuses. Kept out of the header so
// no D3D type reaches anything that includes it.
struct DisplayCapture::Output {
    int deviceNumber = 0;
    IDXGIOutputDuplication* dup = nullptr;
    UINT width = 0, height = 0;
    // The mip pyramid the downscale runs through, and the small staging texture
    // the chosen level is read back from. Both sized once.
    ID3D11Texture2D* mips = nullptr;
    // GenerateMips works on a VIEW, not on the texture: the texture is the
    // storage, the view is what the pipeline is allowed to read.
    ID3D11ShaderResourceView* mipsView = nullptr;
    ID3D11Texture2D* staging = nullptr;
    UINT mipLevels = 0, chosenMip = 0, mipW = 0, mipH = 0;
    int builtForMaxEdge = 0;
    // The dimensions the mip chain was built FOR, taken from the acquired
    // frame rather than the desktop rectangle: a rotated display reports the
    // rotated size, and a mip texture built at the wrong size makes
    // CopySubresourceRegion silently invalid -> a garbage thumbnail.
    UINT srcW = 0, srcH = 0;
    bool formatWarned = false;   // an unsupported (e.g. HDR) format, logged once

    ~Output() {
        Release(staging);
        Release(mipsView);
        Release(mips);
        Release(dup);
    }
};

DisplayCapture::~DisplayCapture() { Shutdown(); }

bool DisplayCapture::Init(std::wstring* err) {
    Shutdown();
    m_lost = false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    // BGRA because that is what duplication hands back and what WIC wants; no
    // feature level is requested, because nothing here needs one.
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                         D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr) || !device) {
        if (err) *err = L"no Direct3D 11 device for screen capture";
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(&adapter))) {
        Release(adapter);
        Release(dxgiDevice);
        Release(context);
        Release(device);
        if (err) *err = L"could not reach the display adapter";
        return false;
    }

    for (UINT i = 0;; ++i) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) break;
        DXGI_OUTPUT_DESC desc = {};
        output->GetDesc(&desc);
        const int number = DeviceNumberOf(desc.DeviceName);
        if (number < 1 || number > kMaxDisplayNumber) {
            // No stream name to put it on. Counted, so "a screen is missing from
            // the phone" has an answer other than silence.
            ++m_skipped;
            Release(output);
            continue;
        }
        IDXGIOutput1* output1 = nullptr;
        IDXGIOutputDuplication* dup = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1)) &&
            SUCCEEDED(output1->DuplicateOutput(device, &dup)) && dup) {
            auto* o = new Output;
            o->deviceNumber = number;
            o->dup = dup;
            o->width = (UINT)(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
            o->height = (UINT)(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            m_outputs.push_back(o);
        } else {
            // A display we could name but not duplicate -- protected content, a
            // driver that refuses, the output already captured elsewhere. Counted
            // like an out-of-range one, so "a screen is missing from the phone"
            // has an answer rather than silence.
            ++m_skipped;
        }
        Release(output1);
        Release(output);
    }
    Release(adapter);
    Release(dxgiDevice);

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        // The capture thread calls CoInitializeEx; if this still fails there is no
        // encoder, and a frame nobody can encode is not worth capturing.
        for (auto* o : m_outputs) delete o;
        m_outputs.clear();
        Release(context);
        Release(device);
        if (err) *err = L"no WIC imaging factory for the JPEG encoder";
        return false;
    }

    if (m_outputs.empty()) {
        Release(wic);
        Release(context);
        Release(device);
        if (err) *err = L"no display could be duplicated";
        return false;
    }
    m_device = device;
    m_context = context;
    m_wic = wic;
    Log(2, L"display capture: %zu display(s), %llu skipped", m_outputs.size(),
        (unsigned long long)m_skipped);
    return true;
}

void DisplayCapture::Shutdown() {
    for (auto* o : m_outputs) delete o;
    m_outputs.clear();
    auto* wic = (IWICImagingFactory*)m_wic;
    auto* context = (ID3D11DeviceContext*)m_context;
    auto* device = (ID3D11Device*)m_device;
    Release(wic);
    Release(context);
    Release(device);
    m_wic = nullptr;
    m_context = nullptr;
    m_device = nullptr;
}

std::vector<CapturedFrame> DisplayCapture::Poll(int maxEdge, int quality) {
    std::vector<CapturedFrame> out;
    auto* device = (ID3D11Device*)m_device;
    auto* context = (ID3D11DeviceContext*)m_context;
    auto* wic = (IWICImagingFactory*)m_wic;
    if (!device || !context || !wic || m_lost) return out;
    if (maxEdge < 16) maxEdge = 16;
    if (quality < 10) quality = 10;
    if (quality > 95) quality = 95;

    for (auto* o : m_outputs) {
        DXGI_OUTDUPL_FRAME_INFO info = {};
        IDXGIResource* resource = nullptr;
        // Zero timeout: this is a poll, and a display that has not changed must
        // not hold the thread.
        const HRESULT hr = o->dup->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            Release(resource);
            continue;
        }
        if (FAILED(hr)) {
            // ACCESS_LOST is the ordinary one: a UAC prompt, a mode change, a
            // session switch. The caller re-initialises; nothing here is an error
            // worth stopping the audio for.
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED)
                m_lost = true;
            Release(resource);
            continue;
        }
        // AccumulatedFrames == 0 means only the pointer moved, which is not a
        // change worth a JPEG.
        if (info.AccumulatedFrames == 0 && info.TotalMetadataBufferSize == 0) {
            Release(resource);
            o->dup->ReleaseFrame();
            continue;
        }

        ID3D11Texture2D* frame = nullptr;
        if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frame);
        Release(resource);
        if (!frame) {
            o->dup->ReleaseFrame();
            continue;
        }
        // The acquired surface's OWN dimensions and format -- not the desktop
        // rectangle. A rotated display hands back a rotated surface, and an HDR
        // one a wide float format that CopySubresourceRegion cannot turn into a
        // BGRA8 mip. Driving the pyramid from here is what keeps a rotated panel
        // from coming back garbled.
        D3D11_TEXTURE2D_DESC fd = {};
        frame->GetDesc(&fd);
        if (fd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            if (!o->formatWarned) {
                Log(1, L"display %d is not BGRA8 (format %u); not captured",
                    o->deviceNumber, (unsigned)fd.Format);
                o->formatWarned = true;
            }
            Release(frame);
            o->dup->ReleaseFrame();
            continue;
        }

        // Build (or rebuild) the mip pyramid and the staging texture. Rebuilt
        // when maxEdge changes OR the source size does (a rotation, a mode
        // change). THE MIP CHAIN IS THE POINT: the GPU does the downscale, so
        // what crosses the bus is the small level rather than a whole frame.
        if (!o->mips || o->builtForMaxEdge != maxEdge ||
            o->srcW != fd.Width || o->srcH != fd.Height) {
            Release(o->staging);
            Release(o->mipsView);
            Release(o->mips);
            o->srcW = fd.Width;
            o->srcH = fd.Height;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = fd.Width;
            td.Height = fd.Height;
            td.MipLevels = 0;                 // 0 = the full chain
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &o->mips)) && o->mips) {
                D3D11_TEXTURE2D_DESC made = {};
                o->mips->GetDesc(&made);
                o->mipLevels = made.MipLevels;
                // The largest level whose long edge still fits.
                o->chosenMip = 0;
                o->mipW = fd.Width;
                o->mipH = fd.Height;
                for (UINT level = 0; level < o->mipLevels; ++level) {
                    const UINT w = fd.Width >> level ? fd.Width >> level : 1;
                    const UINT h = fd.Height >> level ? fd.Height >> level : 1;
                    o->chosenMip = level;
                    o->mipW = w;
                    o->mipH = h;
                    if ((int)(w > h ? w : h) <= maxEdge) break;
                }
                D3D11_TEXTURE2D_DESC sd = {};
                sd.Width = o->mipW;
                sd.Height = o->mipH;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                device->CreateTexture2D(&sd, nullptr, &o->staging);
                D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
                vd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = o->mipLevels;
                device->CreateShaderResourceView(o->mips, &vd, &o->mipsView);
                o->builtForMaxEdge = maxEdge;
            }
        }

        if (o->mips && o->staging && o->mipsView) {
            context->CopySubresourceRegion(o->mips, 0, 0, 0, 0, frame, 0, nullptr);
            context->GenerateMips(o->mipsView);
            context->CopySubresourceRegion(o->staging, 0, 0, 0, 0, o->mips,
                                           o->chosenMip, nullptr);
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (SUCCEEDED(context->Map(o->staging, 0, D3D11_MAP_READ, 0, &map))) {
                IWICBitmap* bitmap = nullptr;
                if (SUCCEEDED(wic->CreateBitmapFromMemory(
                        o->mipW, o->mipH, GUID_WICPixelFormat32bppBGRA, map.RowPitch,
                        map.RowPitch * o->mipH, (BYTE*)map.pData, &bitmap)) && bitmap) {
                    IStream* stream = SHCreateMemStream(nullptr, 0);
                    IWICBitmapEncoder* encoder = nullptr;
                    IWICBitmapFrameEncode* frameEnc = nullptr;
                    IPropertyBag2* props = nullptr;
                    if (stream &&
                        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                                     &encoder)) &&
                        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                        SUCCEEDED(encoder->CreateNewFrame(&frameEnc, &props))) {
                        PROPBAG2 bag = {};
                        bag.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                        VARIANT v;
                        VariantInit(&v);
                        v.vt = VT_R4;
                        v.fltVal = (float)quality / 100.0f;
                        props->Write(1, &bag, &v);
                        if (SUCCEEDED(frameEnc->Initialize(props)) &&
                            SUCCEEDED(frameEnc->WriteSource(bitmap, nullptr)) &&
                            SUCCEEDED(frameEnc->Commit()) &&
                            SUCCEEDED(encoder->Commit())) {
                            // Out of the memory stream and into a plain buffer: the
                            // sender wants bytes, not a COM object.
                            STATSTG st = {};
                            if (SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) &&
                                st.cbSize.QuadPart > 0 &&
                                st.cbSize.QuadPart < 8 * 1024 * 1024) {
                                CapturedFrame cf;
                                cf.deviceNumber = o->deviceNumber;
                                cf.jpeg.resize((size_t)st.cbSize.QuadPart);
                                LARGE_INTEGER zero = {};
                                stream->Seek(zero, STREAM_SEEK_SET, nullptr);
                                ULONG read = 0;
                                if (SUCCEEDED(stream->Read(cf.jpeg.data(),
                                                           (ULONG)cf.jpeg.size(), &read)) &&
                                    read == cf.jpeg.size()) {
                                    out.push_back(std::move(cf));
                                }
                            }
                        }
                    }
                    Release(props);
                    Release(frameEnc);
                    Release(encoder);
                    Release(stream);
                    Release(bitmap);
                }
                context->Unmap(o->staging, 0);
            }
        }
        Release(frame);
        o->dup->ReleaseFrame();
    }
    return out;
}

} // namespace mdxm
