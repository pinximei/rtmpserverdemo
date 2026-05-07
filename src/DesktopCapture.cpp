#include "DesktopCapture.h"

#include <dxgi1_6.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

DesktopCapture::DesktopCapture() {}
DesktopCapture::~DesktopCapture() { Shutdown(); }

HRESULT DesktopCapture::Init() {
    HR_RETURN(CreateDevice());
    HR_RETURN(CreateDuplication());
    m_t0 = std::chrono::steady_clock::now();
    DLOG("DesktopCapture init OK %dx%d", m_width, m_height);
    return S_OK;
}

void DesktopCapture::Shutdown() {
    m_videoProc.Reset();
    m_videoEnum.Reset();
    m_videoCtx.Reset();
    m_videoDevice.Reset();
    m_nv12.Reset();
    m_lastBgra.Reset();
    m_dup.Reset();
    m_ctx.Reset();
    m_device.Reset();
}

HRESULT DesktopCapture::CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL chosen{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
                                   &m_device, &chosen, &m_ctx);
    if (FAILED(hr)) {
        DLOG("D3D11CreateDevice fail 0x%08X, retry without VIDEO_SUPPORT", (unsigned)hr);
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
                               &m_device, &chosen, &m_ctx);
    }
    HR_RETURN(hr);

    // Enable multithread protection (encoder runs on different threads via MF).
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    HR_RETURN(m_device.As(&m_videoDevice));
    HR_RETURN(m_ctx.As(&m_videoCtx));
    return S_OK;
}

HRESULT DesktopCapture::CreateDuplication() {
    ComPtr<IDXGIDevice> dxgiDevice;
    HR_RETURN(m_device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    HR_RETURN(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIOutput> output;
    HR_RETURN(adapter->EnumOutputs(0, &output));
    ComPtr<IDXGIOutput1> output1;
    HR_RETURN(output.As(&output1));

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    m_width  = od.DesktopCoordinates.right  - od.DesktopCoordinates.left;
    m_height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    DLOG("desktop %dx%d", m_width, m_height);

    HR_RETURN(output1->DuplicateOutput(m_device.Get(), &m_dup));
    return S_OK;
}

HRESULT DesktopCapture::EnsureSize(int w, int h) {
    if (m_nv12) {
        D3D11_TEXTURE2D_DESC d{};
        m_nv12->GetDesc(&d);
        if ((int)d.Width == w && (int)d.Height == h) return S_OK;
        m_nv12.Reset();
        m_videoProc.Reset();
        m_videoEnum.Reset();
    }
    D3D11_TEXTURE2D_DESC nvd{};
    nvd.Width = w;
    nvd.Height = h;
    nvd.MipLevels = 1;
    nvd.ArraySize = 1;
    nvd.Format = DXGI_FORMAT_NV12;
    nvd.SampleDesc.Count = 1;
    nvd.Usage = D3D11_USAGE_DEFAULT;
    nvd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HR_RETURN(m_device->CreateTexture2D(&nvd, nullptr, &m_nv12));
    return S_OK;
}

HRESULT DesktopCapture::EnsureVideoProcessor(int w, int h) {
    if (m_videoProc) return S_OK;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = 60;
    cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = w;
    cd.InputHeight = h;
    cd.OutputFrameRate.Numerator = 60;
    cd.OutputFrameRate.Denominator = 1;
    cd.OutputWidth = w;
    cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HR_RETURN(m_videoDevice->CreateVideoProcessorEnumerator(&cd, &m_videoEnum));
    HR_RETURN(m_videoDevice->CreateVideoProcessor(m_videoEnum.Get(), 0, &m_videoProc));

    // Output color: BT.709 / Limited (matches H.264 default for 720p+; 1080p ok)
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.Usage = 0;       // 0 = playback
    outCs.RGB_Range = 0;   // not used for NV12 output
    outCs.YCbCr_Matrix = 1;// 1 = BT.709
    outCs.YCbCr_xvYCC = 0;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoCtx->VideoProcessorSetOutputColorSpace(m_videoProc.Get(), &outCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.RGB_Range = 0;    // 0 = full
    inCs.YCbCr_Matrix = 1;
    m_videoCtx->VideoProcessorSetStreamColorSpace(m_videoProc.Get(), 0, &inCs);

    DLOG("video processor %dx%d ready", w, h);
    return S_OK;
}

HRESULT DesktopCapture::BgraToNv12(ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    int w = (int)sd.Width;
    int h = (int)sd.Height;

    HR_RETURN(EnsureSize(w, h));
    HR_RETURN(EnsureVideoProcessor(w, h));

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = 0;
    ComPtr<ID3D11VideoProcessorInputView> iv;
    HR_RETURN(m_videoDevice->CreateVideoProcessorInputView(src, m_videoEnum.Get(), &ivd, &iv));

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> ov;
    HR_RETURN(m_videoDevice->CreateVideoProcessorOutputView(m_nv12.Get(), m_videoEnum.Get(), &ovd, &ov));

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.ppPastSurfaces = nullptr;
    stream.pInputSurface = iv.Get();
    stream.ppFutureSurfaces = nullptr;

    HR_RETURN(m_videoCtx->VideoProcessorBlt(m_videoProc.Get(), ov.Get(), 0, 1, &stream));
    return S_OK;
}

HRESULT DesktopCapture::AcquireFrame(CapturedFrame& out, uint32_t timeoutMs) {
    if (!m_dup) {
        // Try to (re)create duplication. If it still fails, ask caller to retry.
        HRESULT cr = CreateDuplication();
        if (FAILED(cr)) {
            // Throttle retry attempts via timeout signal.
            return DXGI_ERROR_WAIT_TIMEOUT;
        }
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = m_dup->AcquireNextFrame(timeoutMs, &info, &resource);
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - m_t0).count() * 10;  // -> 100ns

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame. Re-emit last frame so encoder timeline stays smooth.
        if (m_haveLast && m_nv12) {
            out.nv12 = m_nv12;
            out.timestampHns = now;
            out.width = m_width;
            out.height = m_height;
            out.isNewFrame = false;
            return S_FALSE;
        }
        return DXGI_ERROR_WAIT_TIMEOUT;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        DLOG("duplication access lost, recreating");
        m_dup.Reset();
        HRESULT cr = CreateDuplication();
        if (FAILED(cr)) return cr;
        return DXGI_ERROR_WAIT_TIMEOUT;
    }
    if (FAILED(hr)) {
        DLOG("AcquireNextFrame fail 0x%08X", (unsigned)hr);
        return hr;
    }

    ComPtr<ID3D11Texture2D> bgra;
    if (resource) {
        if (FAILED(resource.As(&bgra))) {
            m_dup->ReleaseFrame();
            return E_FAIL;
        }
    }

    if (info.LastPresentTime.QuadPart == 0 && m_haveLast && m_nv12) {
        // No actual update; release and re-emit last
        m_dup->ReleaseFrame();
        out.nv12 = m_nv12;
        out.timestampHns = now;
        out.width = m_width;
        out.height = m_height;
        out.isNewFrame = false;
        return S_FALSE;
    }

    HRESULT cr = BgraToNv12(bgra.Get());
    m_dup->ReleaseFrame();
    if (FAILED(cr)) {
        DLOG("BgraToNv12 fail 0x%08X", (unsigned)cr);
        return cr;
    }
    m_haveLast = true;

    out.nv12 = m_nv12;
    out.timestampHns = now;
    out.width = m_width;
    out.height = m_height;
    out.isNewFrame = true;
    return S_OK;
}
