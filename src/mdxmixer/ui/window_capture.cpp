#include "window_capture.h"
#include <objbase.h>
#include <wincodec.h>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace mdxm {

namespace {

// WIC rather than GDI+: it is already a COM citizen in a process that runs COM
// anyway, and it needs no startup/shutdown token threaded through the app.
bool SavePixelsToPng(const std::vector<BYTE>& bgra, UINT w, UINT h, const std::wstring& path) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    bool ok = false;
    IWICBitmapEncoder* encoder = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)bgra.size(), (BYTE*)bgra.data())) &&
            SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit()))
            ok = true;
    }
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

} // namespace

bool CaptureWindowToPng(HWND hwnd, const std::wstring& path) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;       // top-down, so the rows need no flipping
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        // PW_CLIENTONLY (1) | PW_RENDERFULLCONTENT (2). The second is what
        // paints owner-drawn content; fall back without it rather than fail.
        ok = PrintWindow(hwnd, mem, 1 | 2) != 0;
        if (!ok) ok = PrintWindow(hwnd, mem, 1) != 0;
        if (ok) {
            std::vector<BYTE> pixels((size_t)w * h * 4);
            memcpy(pixels.data(), bits, pixels.size());
            // PrintWindow leaves alpha at zero; PNG would save it fully
            // transparent and the image would look empty in every viewer.
            for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;
            ok = SavePixelsToPng(pixels, (UINT)w, (UINT)h, path);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

} // namespace mdxm
