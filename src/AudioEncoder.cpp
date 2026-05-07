#include "AudioEncoder.h"
#include "FlvMuxer.h"

#include <wmcodecdsp.h>
#include <mferror.h>
#include <algorithm>
#include <climits>
#include <cstdlib>

AudioEncoder::AudioEncoder() {}
AudioEncoder::~AudioEncoder() { Shutdown(); }

HRESULT AudioEncoder::Init(int sampleRate, int channels, int bitratePerSec) {
    if (sampleRate != 44100 && sampleRate != 48000) {
        // AAC MFT supports only 44.1k / 48k. Coerce caller to one of these.
        DLOG("AudioEncoder: unsupported sample rate %d, must be 44100 or 48000", sampleRate);
        return E_INVALIDARG;
    }
    if (channels != 1 && channels != 2) {
        DLOG("AudioEncoder: unsupported channels %d", channels);
        return E_INVALIDARG;
    }
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrate = bitratePerSec;

    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HR_RETURN(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
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

    DWORD inIds[1]={0}, outIds[1]={0};
    HRESULT hr = m_mft->GetStreamIDs(1, inIds, 1, outIds);
    if (hr == E_NOTIMPL) { m_inputStreamId = 0; m_outputStreamId = 0; }
    else { m_inputStreamId = inIds[0]; m_outputStreamId = outIds[0]; }

    // Input: PCM s16
    ComPtr<IMFMediaType> inType;
    HR_RETURN(MFCreateMediaType(&inType));
    HR_RETURN(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    HR_RETURN(inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    HR_RETURN(inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    HR_RETURN(inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    HR_RETURN(inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    HR_RETURN(inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2));
    HR_RETURN(inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 2));
    HR_RETURN(inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    HR_RETURN(m_mft->SetInputType(m_inputStreamId, inType.Get(), 0));

    // Output: AAC LC at desired bitrate. Iterate available output types and
    // pick the closest one (do NOT modify the type — MS AAC encoder rejects
    // overrides of byte rate / channels / sample rate).
    ComPtr<IMFMediaType> outType;
    int wantBytes = bitratePerSec / 8;
    int bestDiff = INT_MAX;
    UINT32 idx = 0;
    while (true) {
        ComPtr<IMFMediaType> avail;
        HRESULT eh = m_mft->GetOutputAvailableType(m_outputStreamId, idx, &avail);
        if (eh == MF_E_NO_MORE_TYPES) break;
        if (FAILED(eh)) return eh;
        UINT32 ch=0, sr=0, bps=0;
        avail->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        avail->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        avail->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
        if ((int)ch == channels && (int)sr == sampleRate) {
            int d = std::abs((int)bps - wantBytes);
            if (d < bestDiff) {
                bestDiff = d;
                outType = avail;
            }
        }
        ++idx;
    }
    if (!outType) return E_FAIL;
    HR_RETURN(m_mft->SetOutputType(m_outputStreamId, outType.Get(), 0));

    // Build AudioSpecificConfig (2 bytes for AAC LC).
    // Some MFTs expose MF_MT_USER_DATA = HEAACWAVEINFO (12 bytes prefix) + ASC
    UINT8* udata = nullptr; UINT32 udsize = 0;
    if (SUCCEEDED(outType->GetAllocatedBlob(MF_MT_USER_DATA, &udata, &udsize))) {
        if (udsize > 12) {
            m_asc.assign(udata + 12, udata + udsize);
        }
        CoTaskMemFree(udata);
    }
    if (m_asc.empty()) {
        // Build manually: AAC LC, sample rate idx, channel config
        m_asc = flv::BuildAudioSpecificConfig(2 /*AAC LC*/, sampleRate, channels);
    }
    DLOG("AAC encoder ready: %d Hz, %d ch, %d bps, ASC %zu bytes",
         sampleRate, channels, bitratePerSec, m_asc.size());

    HR_LOG(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    HR_LOG(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    return S_OK;
}

void AudioEncoder::Shutdown() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    m_mft.Reset();
    m_outQueue.clear();
    m_asc.clear();
}

HRESULT AudioEncoder::EncodeFloat(const float* samples, int frames, int64_t tsHns) {
    if (!m_mft || frames <= 0) return E_FAIL;
    int total = frames * m_channels;
    m_pcmScratch.resize((size_t)total);
    for (int i = 0; i < total; ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        m_pcmScratch[i] = (int16_t)(s * 32767.0f);
    }
    UINT32 byteSize = (UINT32)(total * sizeof(int16_t));

    ComPtr<IMFMediaBuffer> buf;
    HR_RETURN(MFCreateMemoryBuffer(byteSize, &buf));
    BYTE* dst = nullptr; DWORD ml=0, cl=0;
    HR_RETURN(buf->Lock(&dst, &ml, &cl));
    memcpy(dst, m_pcmScratch.data(), byteSize);
    buf->Unlock();
    buf->SetCurrentLength(byteSize);

    ComPtr<IMFSample> sample;
    HR_RETURN(MFCreateSample(&sample));
    HR_RETURN(sample->AddBuffer(buf.Get()));
    HR_RETURN(sample->SetSampleTime(tsHns));
    int64_t durHns = (int64_t)frames * 10000000 / m_sampleRate;
    HR_RETURN(sample->SetSampleDuration(durHns));

    HRESULT hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        DrainOutputs();
        hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;

    return DrainOutputs();
}

HRESULT AudioEncoder::DrainOutputs() {
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
            DWORD bufSize = si.cbSize ? si.cbSize : 65536;
            HR_RETURN(MFCreateMemoryBuffer(bufSize, &buf));
            HR_RETURN(sample->AddBuffer(buf.Get()));
            out.pSample = sample.Get();
        }

        DWORD status = 0;
        hr = m_mft->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (FAILED(hr)) return hr;
        if (out.pEvents) { out.pEvents->Release(); out.pEvents = nullptr; }

        ComPtr<IMFSample> outSample = providesSamples ? out.pSample : sample.Get();
        if (providesSamples && out.pSample) outSample.Attach(out.pSample);
        if (!outSample) continue;

        ComPtr<IMFMediaBuffer> outBuf;
        outSample->ConvertToContiguousBuffer(&outBuf);
        BYTE* p = nullptr; DWORD ml=0, cl=0;
        if (FAILED(outBuf->Lock(&p, &ml, &cl))) continue;

        EncodedAudioPacket pkt;
        pkt.aac.assign(p, p + cl);
        LONGLONG t = 0;
        outSample->GetSampleTime(&t);
        pkt.timestampHns = t;
        outBuf->Unlock();

        m_outQueue.push_back(std::move(pkt));
    }
}

bool AudioEncoder::PullPacket(EncodedAudioPacket& out) {
    if (m_outQueue.empty()) return false;
    out = std::move(m_outQueue.front());
    m_outQueue.erase(m_outQueue.begin());
    return true;
}
