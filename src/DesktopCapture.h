#pragma once

#include "Common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <chrono>

// DXGI Desktop Duplication capture.
//   * Owns the D3D11 device (single device shared with VideoEncoder & MF MFT
//     via SetSharedDevice; encoder will pull frames as ID3D11Texture2D NV12).
//   * Captures the primary monitor's BGRA back buffer.
//   * Converts BGRA -> NV12 on GPU using ID3D11VideoProcessor (DXVA path).
//   * Output: ComPtr<ID3D11Texture2D> NV12 (1 sample), with timestamp.

struct CapturedFrame {
    ComPtr<ID3D11Texture2D> nv12;   // shared NV12 texture for encoder/MFT
    int64_t timestampHns = 0;       // 100-ns units (monotonic; capture-rel)
    int width = 0;
    int height = 0;
    bool isNewFrame = true;         // false if we re-emitted last frame
};

class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();

    // Initialize device + duplication. Picks adapter 0 / output 0.
    HRESULT Init();
    void Shutdown();

    // One-shot acquire-and-convert. Blocks up to timeoutMs for next frame.
    // Returns S_FALSE if no new frame appeared within timeout (out is filled
    // with last frame and isNewFrame=false).
    HRESULT AcquireFrame(CapturedFrame& out, uint32_t timeoutMs);

    int Width() const  { return m_width; }
    int Height() const { return m_height; }

    ID3D11Device*        Device()  const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_ctx.Get(); }

private:
    HRESULT CreateDevice();
    HRESULT CreateDuplication();
    HRESULT EnsureSize(int w, int h);
    HRESULT EnsureVideoProcessor(int w, int h);
    HRESULT BgraToNv12(ID3D11Texture2D* src);

    ComPtr<ID3D11Device>         m_device;
    ComPtr<ID3D11DeviceContext>  m_ctx;
    ComPtr<IDXGIOutputDuplication> m_dup;

    // Video processor for BGRA->NV12
    ComPtr<ID3D11VideoDevice>    m_videoDevice;
    ComPtr<ID3D11VideoContext>   m_videoCtx;
    ComPtr<ID3D11VideoProcessor> m_videoProc;
    ComPtr<ID3D11VideoProcessorEnumerator> m_videoEnum;

    ComPtr<ID3D11Texture2D> m_nv12;     // current output (D3D11_USAGE_DEFAULT, BIND_RENDER_TARGET via VPE)
    ComPtr<ID3D11Texture2D> m_lastBgra; // for re-emit when frame did not change

    std::chrono::steady_clock::time_point m_t0;
    int m_width = 0;
    int m_height = 0;
    bool m_haveLast = false;

    static const int kTargetWidth  = 1920;
    static const int kTargetHeight = 1080;
};
