#pragma once

#include "Common.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <vector>

// AAC LC encoder via Media Foundation MFT.
// Input PCM: 16-bit, mono or stereo, 44100 or 48000 Hz.
// Output: AAC raw frames, plus AudioSpecificConfig from media type.

struct EncodedAudioPacket {
    std::vector<uint8_t> aac;     // raw AAC frame (no ADTS)
    int64_t timestampHns = 0;
};

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    HRESULT Init(int sampleRate, int channels, int bitratePerSec);
    void Shutdown();

    // Feed float32 interleaved PCM (size = frames*channels). Internally
    // converted to int16. timestampHns is for the FIRST sample.
    HRESULT EncodeFloat(const float* samples, int frames, int64_t timestampHns);

    bool PullPacket(EncodedAudioPacket& out);

    // 2 bytes for AAC LC. Empty until Init succeeded.
    const std::vector<uint8_t>& AudioSpecificConfig() const { return m_asc; }

    int SampleRate() const { return m_sampleRate; }
    int Channels()   const { return m_channels; }

private:
    HRESULT DrainOutputs();

    ComPtr<IMFTransform> m_mft;
    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;

    int m_sampleRate = 0;
    int m_channels = 0;
    int m_bitrate = 0;

    std::vector<uint8_t> m_asc;
    std::vector<int16_t> m_pcmScratch;
    std::vector<EncodedAudioPacket> m_outQueue;
};
