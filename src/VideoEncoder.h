#pragma once

#include "Common.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <atomic>
#include <vector>

// ICodecAPI is declared transitively via codecapi.h on most SDKs, but its
// forward declaration here keeps the build robust if some SDK reorganizes.
struct ICodecAPI;

// MF H.264 encoder. Tries hardware MFT (NVENC/QSV/AMF) first; falls back to
// software MFT. Input: NV12 ID3D11Texture2D (hw) or NV12 buffer (sw).
//
// Output mode: byte stream (Annex-B) so we can split NAL units cleanly.

struct EncodedVideoPacket {
    std::vector<uint8_t> annexB;  // raw NAL units in Annex-B format
    int64_t timestampHns = 0;     // 100-ns
    bool keyframe = false;
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    // Pass the same D3D11 device the capture uses. The encoder will install
    // an MF DXGI device manager pointing at it so hw MFT can read NV12
    // textures directly. If null, software encoder is used.
    HRESULT Init(int width, int height, int frameRate,
                 int bitrateBitsPerSecond,
                 ID3D11Device* deviceForHw);
    void Shutdown();

    bool IsHardware() const { return m_hardware; }

    // Encode one frame (NV12 texture). The encoder may produce 0 or 1+ output
    // packets — pull them via PullPacket() until it returns false.
    HRESULT EncodeFrame(ID3D11Texture2D* nv12, int64_t timestampHns, bool keyframeRequest);

    // Drain encoder (call EncodeFrame(null,...) won't be allowed; instead use
    // this at end-of-stream).
    void Drain();

    bool PullPacket(EncodedVideoPacket& out);

    // SPS / PPS extracted from first IDR (Annex-B). Empty until first key out.
    const std::vector<uint8_t>& SPS() const { return m_sps; }
    const std::vector<uint8_t>& PPS() const { return m_pps; }

private:
    HRESULT CreateMftHw(ID3D11Device* dev);
    HRESULT CreateMftSw();
    HRESULT ConfigureCommon();
    HRESULT FeedSampleHw(ID3D11Texture2D* nv12, int64_t tsHns, bool keyReq);
    HRESULT FeedSampleSw(ID3D11Texture2D* nv12, int64_t tsHns, bool keyReq);
    HRESULT DrainOutputs();

    ComPtr<IMFTransform> m_mft;
    ComPtr<IMFAttributes> m_codecApi;
    ComPtr<ICodecAPI> m_codec;
    ComPtr<IMFDXGIDeviceManager> m_devMgr;
    UINT m_devMgrToken = 0;
    ComPtr<ID3D11Device> m_d3d;
    ComPtr<ID3D11DeviceContext> m_ctx;

    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;

    int m_width = 0;
    int m_height = 0;
    int m_frameRate = 60;
    int m_bitrate = 0;
    bool m_hardware = false;
    bool m_eventBased = false;
    ComPtr<IMFMediaEventGenerator> m_eventGen;

    std::vector<EncodedVideoPacket> m_outQueue;
    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
    bool m_loggedHeader = false;

    // For software encoder we need a CPU staging copy of NV12.
    ComPtr<ID3D11Texture2D> m_stagingNv12;
};
