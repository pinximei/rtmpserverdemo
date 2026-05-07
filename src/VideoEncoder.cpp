#include "VideoEncoder.h"
#include "FlvMuxer.h"   // SplitAnnexB

#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <propvarutil.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

VideoEncoder::VideoEncoder() {}
VideoEncoder::~VideoEncoder() { Shutdown(); }

HRESULT VideoEncoder::Init(int width, int height, int fps, int bps, ID3D11Device* dev) {
    m_width = width;
    m_height = height;
    m_frameRate = fps;
    m_bitrate = bps;

    HRESULT hr = E_FAIL;
    if (dev) {
        hr = CreateMftHw(dev);
        if (FAILED(hr)) {
            DLOG("hw encoder MFT setup failed 0x%08X, fallback to software", (unsigned)hr);
            m_mft.Reset();
            m_codecApi.Reset();
            m_devMgr.Reset();
            m_d3d.Reset();
            m_ctx.Reset();
        }
    }
    if (!m_mft) {
        hr = CreateMftSw();
        if (FAILED(hr)) {
            DLOG("software encoder also failed 0x%08X", (unsigned)hr);
            return hr;
        }
    }

    HR_RETURN(ConfigureCommon());

    // Begin streaming
    HR_LOG(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    HR_LOG(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    DLOG("video encoder ready: %s, %dx%d @ %dfps, %d bps", m_hardware ? "HW" : "SW",
         width, height, fps, bps);
    return S_OK;
}

void VideoEncoder::Shutdown() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    m_mft.Reset();
    m_codecApi.Reset();
    m_devMgr.Reset();
    m_eventGen.Reset();
    m_d3d.Reset();
    m_ctx.Reset();
    m_stagingNv12.Reset();
    m_outQueue.clear();
    m_sps.clear();
    m_pps.clear();
}

HRESULT VideoEncoder::CreateMftHw(ID3D11Device* dev) {
    m_d3d = dev;
    dev->GetImmediateContext(&m_ctx);

    HR_RETURN(MFCreateDXGIDeviceManager(&m_devMgrToken, &m_devMgr));
    HR_RETURN(m_devMgr->ResetDevice(dev, m_devMgrToken));

    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &outInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return E_FAIL;
    }

    HRESULT actHr = E_FAIL;
    for (UINT32 i = 0; i < count; ++i) {
        if (FAILED(actHr)) {
            actHr = activates[i]->ActivateObject(IID_PPV_ARGS(&m_mft));
            if (FAILED(actHr)) m_mft.Reset();
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    if (FAILED(actHr)) return actHr;

    // We don't drive an async event loop in this demo. If the MFT is
    // async-only, abandon hardware path and fall back to software.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_mft->GetAttributes(&attrs))) {
        UINT32 async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
            DLOG("hw MFT is async-only; falling back to software encoder");
            return E_FAIL;
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    // Bind D3D manager
    HR_RETURN(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_devMgr.Get()));

    // Codec API parameters live on the MFT's attribute store.
    HR_RETURN(m_mft->GetAttributes(&m_codecApi));
    m_mft.As(&m_codec);

    m_hardware = true;
    return S_OK;
}

HRESULT VideoEncoder::CreateMftSw() {
    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HR_RETURN(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &outInfo, &activates, &count));
    if (count == 0) {
        if (activates) CoTaskMemFree(activates);
        return E_FAIL;
    }
    HRESULT actHr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_mft));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    HR_RETURN(actHr);

    m_mft->GetAttributes(&m_codecApi);
    m_mft.As(&m_codec);  // ICodecAPI is the proper interface for codec params
    m_hardware = false;
    m_eventBased = false;
    return S_OK;
}

HRESULT VideoEncoder::ConfigureCommon() {
    DWORD inIds[1] = { 0 }, outIds[1] = { 0 };
    DWORD nIn = 0, nOut = 0;
    HRESULT hr = m_mft->GetStreamIDs(1, inIds, 1, outIds);
    if (hr == E_NOTIMPL) { m_inputStreamId = 0; m_outputStreamId = 0; }
    else { m_inputStreamId = inIds[0]; m_outputStreamId = outIds[0]; }

    // Set output type FIRST (some MFTs require this)
    ComPtr<IMFMediaType> outType;
    HR_RETURN(MFCreateMediaType(&outType));
    HR_RETURN(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    HR_RETURN(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    HR_RETURN(outType->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate));
    HR_RETURN(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, m_width, m_height));
    HR_RETURN(MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, m_frameRate, 1));
    HR_RETURN(MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    HR_RETURN(outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    HR_RETURN(outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));

    HR_RETURN(m_mft->SetOutputType(m_outputStreamId, outType.Get(), 0));

    ComPtr<IMFMediaType> inType;
    HR_RETURN(MFCreateMediaType(&inType));
    HR_RETURN(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    HR_RETURN(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    HR_RETURN(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, m_width, m_height));
    HR_RETURN(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, m_frameRate, 1));
    HR_RETURN(MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    HR_RETURN(inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    HR_RETURN(m_mft->SetInputType(m_inputStreamId, inType.Get(), 0));

    // ICodecAPI is the canonical interface for these parameters; both HW and
    // SW MFTs implement it. IMFAttributes::SetUINT32 happens to work on some
    // HW MFTs, but the SW MFT silently ignores those keys.
    if (m_codec) {
        auto setU32 = [&](const GUID& key, ULONG val) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_UI4; v.ulVal = val;
            HRESULT hr = m_codec->SetValue(&key, &v);
            VariantClear(&v);
            return hr;
        };
        auto setBool = [&](const GUID& key, BOOL val) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_BOOL; v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
            HRESULT hr = m_codec->SetValue(&key, &v);
            VariantClear(&v);
            return hr;
        };
        setBool(CODECAPI_AVLowLatencyMode, TRUE);
        setU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setU32(CODECAPI_AVEncCommonMeanBitRate, (ULONG)m_bitrate);
        setU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        // 1-sec GOP gives new players a max ~1s wait for first frame.
        setU32(CODECAPI_AVEncMPVGOPSize, (ULONG)m_frameRate);
        setBool(CODECAPI_AVEncH264CABACEnable, TRUE);
    }
    return S_OK;
}

HRESULT VideoEncoder::EncodeFrame(ID3D11Texture2D* nv12, int64_t tsHns, bool keyReq) {
    if (m_hardware) HR_RETURN(FeedSampleHw(nv12, tsHns, keyReq));
    else            HR_RETURN(FeedSampleSw(nv12, tsHns, keyReq));
    return DrainOutputs();
}

void VideoEncoder::Drain() {
    if (!m_mft) return;
    m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    DrainOutputs();
}

HRESULT VideoEncoder::FeedSampleHw(ID3D11Texture2D* nv12, int64_t tsHns, bool keyReq) {
    if (keyReq && m_codec) {
        VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = 1;
        m_codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
    }
    ComPtr<IMFSample> sample;
    HR_RETURN(MFCreateSample(&sample));
    ComPtr<IMFMediaBuffer> buf;
    HR_RETURN(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buf));
    HR_RETURN(sample->AddBuffer(buf.Get()));
    HR_RETURN(sample->SetSampleTime(tsHns));
    HR_RETURN(sample->SetSampleDuration(10000000 / m_frameRate));
    if (keyReq) {
        // Standard MF mechanism — works on encoders whose ICodecAPI doesn't
        // honor AVEncVideoForceKeyFrame. Setting CleanPoint on the input
        // sample tells the encoder this frame must be coded as IDR.
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    HRESULT hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        DrainOutputs();
        hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
    }
    return hr;
}

HRESULT VideoEncoder::FeedSampleSw(ID3D11Texture2D* nv12, int64_t tsHns, bool keyReq) {
    if (keyReq && m_codec) {
        VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = 1;
        m_codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
    }
    // Copy texture to staging, lock, and pack into a system-memory sample.
    if (!m_stagingNv12) {
        D3D11_TEXTURE2D_DESC d{};
        nv12->GetDesc(&d);
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Device> dev;
        nv12->GetDevice(&dev);
        HR_RETURN(dev->CreateTexture2D(&d, nullptr, &m_stagingNv12));
        if (!m_ctx) dev->GetImmediateContext(&m_ctx);
        if (!m_d3d) m_d3d = dev;
    }
    m_ctx->CopyResource(m_stagingNv12.Get(), nv12);
    D3D11_MAPPED_SUBRESOURCE map{};
    HR_RETURN(m_ctx->Map(m_stagingNv12.Get(), 0, D3D11_MAP_READ, 0, &map));

    UINT ySize = (UINT)m_width * m_height;
    UINT uvSize = ySize / 2;
    UINT total = ySize + uvSize;

    ComPtr<IMFMediaBuffer> buf;
    HR_RETURN(MFCreateMemoryBuffer(total, &buf));
    BYTE* dst = nullptr; DWORD maxLen = 0, curLen = 0;
    HR_RETURN(buf->Lock(&dst, &maxLen, &curLen));
    const BYTE* src = (const BYTE*)map.pData;
    for (int r = 0; r < m_height; ++r)     memcpy(dst + r * m_width,         src + r * map.RowPitch, m_width);
    for (int r = 0; r < m_height / 2; ++r) memcpy(dst + ySize + r * m_width, src + (m_height + r) * map.RowPitch, m_width);
    buf->Unlock();
    buf->SetCurrentLength(total);
    m_ctx->Unmap(m_stagingNv12.Get(), 0);

    ComPtr<IMFSample> sample;
    HR_RETURN(MFCreateSample(&sample));
    HR_RETURN(sample->AddBuffer(buf.Get()));
    HR_RETURN(sample->SetSampleTime(tsHns));
    HR_RETURN(sample->SetSampleDuration(10000000 / m_frameRate));
    if (keyReq) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    HRESULT hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        DrainOutputs();
        hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
    }
    return hr;
}

HRESULT VideoEncoder::DrainOutputs() {
    while (true) {
        MFT_OUTPUT_STREAM_INFO si{};
        HRESULT hr = m_mft->GetOutputStreamInfo(m_outputStreamId, &si);
        if (FAILED(hr)) return hr;

        bool providesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                              MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = m_outputStreamId;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        if (!providesSamples) {
            HR_RETURN(MFCreateSample(&sample));
            HR_RETURN(MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (1 << 20), &buf));
            HR_RETURN(sample->AddBuffer(buf.Get()));
            out.pSample = sample.Get();
        }

        DWORD status = 0;
        hr = m_mft->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // re-set output type as required
            ComPtr<IMFMediaType> nt;
            UINT32 i = 0;
            while (SUCCEEDED(m_mft->GetOutputAvailableType(m_outputStreamId, i, &nt))) {
                GUID sub{}; nt->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_H264) {
                    m_mft->SetOutputType(m_outputStreamId, nt.Get(), 0);
                    break;
                }
                ++i;
                nt.Reset();
            }
            continue;
        }
        if (FAILED(hr)) return hr;

        if (out.pEvents) { out.pEvents->Release(); out.pEvents = nullptr; }

        ComPtr<IMFSample> outSample = providesSamples ? out.pSample : sample.Get();
        if (providesSamples && out.pSample) {
            outSample.Attach(out.pSample);  // takes ownership
        }
        if (!outSample) continue;

        ComPtr<IMFMediaBuffer> outBuf;
        outSample->ConvertToContiguousBuffer(&outBuf);
        BYTE* p = nullptr; DWORD ml = 0, cl = 0;
        if (FAILED(outBuf->Lock(&p, &ml, &cl))) continue;

        EncodedVideoPacket pkt;
        pkt.annexB.assign(p, p + cl);
        LONGLONG t = 0;
        outSample->GetSampleTime(&t);
        pkt.timestampHns = t;

        UINT32 isKey = 0;
        outSample->GetUINT32(MFSampleExtension_CleanPoint, &isKey);

        // Inspect NALU contents: keyframe iff payload contains an IDR (type 5)
        // OR an SPS (type 7) sequence (the SW MFT typically prefixes a keyframe
        // with SPS+PPS but does not always set MFSampleExtension_CleanPoint).
        auto nalus = flv::SplitAnnexB(pkt.annexB.data(), pkt.annexB.size());
        bool hasIdr = false, hasSps = false, hasPps = false;
        for (const auto& n : nalus) {
            if (n.empty()) continue;
            int t = n[0] & 0x1F;
            if (t == 5) hasIdr = true;
            else if (t == 7) { hasSps = true; if (m_sps.empty()) m_sps = n; }
            else if (t == 8) { hasPps = true; if (m_pps.empty()) m_pps = n; }
        }
        pkt.keyframe = (isKey != 0) || hasIdr || (hasSps && hasPps);

        outBuf->Unlock();

        if ((hasSps || hasPps) && !m_sps.empty() && !m_pps.empty() && !m_loggedHeader) {
            DLOG("encoder: SPS %zu, PPS %zu cached", m_sps.size(), m_pps.size());
            m_loggedHeader = true;
        }

        m_outQueue.push_back(std::move(pkt));
    }
}

bool VideoEncoder::PullPacket(EncodedVideoPacket& out) {
    if (m_outQueue.empty()) return false;
    out = std::move(m_outQueue.front());
    m_outQueue.erase(m_outQueue.begin());
    return true;
}
