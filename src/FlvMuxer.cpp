#include "FlvMuxer.h"
#include "RtmpProtocol.h"

#include <cstring>

namespace flv {

static int SampleRateIndex(int hz) {
    switch (hz) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000:  return 11;
        case 7350:  return 12;
    }
    return 4; // default 44100
}

std::vector<uint8_t> BuildAvcDecoderConfigRecord(const std::vector<uint8_t>& sps,
                                                 const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> r;
    r.push_back(0x01);                          // configurationVersion
    if (sps.size() >= 4) {
        r.push_back(sps[1]);                    // AVCProfileIndication
        r.push_back(sps[2]);                    // profile_compatibility
        r.push_back(sps[3]);                    // AVCLevelIndication
    } else {
        r.push_back(0x42); r.push_back(0x00); r.push_back(0x1F); // baseline 3.1 fallback
    }
    r.push_back(0xFF);                          // 6 bits reserved + 2 bits lengthSizeMinusOne (3 = 4-byte)
    r.push_back(0xE1);                          // 3 bits reserved + 5 bits numOfSPS (=1)
    uint16_t spsLen = (uint16_t)sps.size();
    r.push_back((uint8_t)(spsLen >> 8));
    r.push_back((uint8_t)(spsLen & 0xFF));
    r.insert(r.end(), sps.begin(), sps.end());
    r.push_back(0x01);                          // numOfPPS
    uint16_t ppsLen = (uint16_t)pps.size();
    r.push_back((uint8_t)(ppsLen >> 8));
    r.push_back((uint8_t)(ppsLen & 0xFF));
    r.insert(r.end(), pps.begin(), pps.end());
    return r;
}

std::vector<uint8_t> BuildVideoTagAvcSeqHeader(const std::vector<uint8_t>& sps,
                                                const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> r;
    r.push_back((1 << 4) | 7);  // keyframe + AVC
    r.push_back(0);             // AVC sequence header
    r.push_back(0); r.push_back(0); r.push_back(0); // composition time = 0
    auto cfg = BuildAvcDecoderConfigRecord(sps, pps);
    r.insert(r.end(), cfg.begin(), cfg.end());
    return r;
}

std::vector<uint8_t> BuildVideoTagAvcNalu(const std::vector<std::vector<uint8_t>>& nalus,
                                          bool keyframe) {
    std::vector<uint8_t> r;
    r.push_back(((keyframe ? 1 : 2) << 4) | 7);
    r.push_back(1);             // NALU
    r.push_back(0); r.push_back(0); r.push_back(0); // composition time = 0 (no B frames)
    for (const auto& n : nalus) {
        uint32_t len = (uint32_t)n.size();
        r.push_back((uint8_t)((len >> 24) & 0xFF));
        r.push_back((uint8_t)((len >> 16) & 0xFF));
        r.push_back((uint8_t)((len >> 8) & 0xFF));
        r.push_back((uint8_t)(len & 0xFF));
        r.insert(r.end(), n.begin(), n.end());
    }
    return r;
}

std::vector<uint8_t> BuildAudioSpecificConfig(int objectType, int sampleRateHz, int channelConfig) {
    int sri = SampleRateIndex(sampleRateHz);
    // 5 bits objectType | 4 bits sampleRateIdx | 4 bits channelConfig | 3 bits zero
    uint16_t v = 0;
    v |= ((uint16_t)(objectType & 0x1F) << 11);
    v |= ((uint16_t)(sri & 0x0F) << 7);
    v |= ((uint16_t)(channelConfig & 0x0F) << 3);
    std::vector<uint8_t> r(2);
    r[0] = (uint8_t)((v >> 8) & 0xFF);
    r[1] = (uint8_t)(v & 0xFF);
    return r;
}

static uint8_t AacAudioFormatByte() {
    // SoundFormat=10 (AAC), SoundRate=3 (44.1kHz+; AAC always reports 3 in FLV),
    // SoundSize=1 (16-bit samples), SoundType=1 (stereo).
    return (10 << 4) | (3 << 2) | (1 << 1) | 1;
}

std::vector<uint8_t> BuildAudioTagAacSeqHeader(const std::vector<uint8_t>& asc) {
    std::vector<uint8_t> r;
    r.push_back(AacAudioFormatByte());
    r.push_back(0); // AAC sequence header
    r.insert(r.end(), asc.begin(), asc.end());
    return r;
}

std::vector<uint8_t> BuildAudioTagAacRaw(const uint8_t* data, size_t size) {
    std::vector<uint8_t> r;
    r.reserve(size + 2);
    r.push_back(AacAudioFormatByte());
    r.push_back(1); // AAC raw
    r.insert(r.end(), data, data + size);
    return r;
}

std::vector<std::vector<uint8_t>> SplitAnnexB(const uint8_t* data, size_t size) {
    std::vector<std::vector<uint8_t>> nalus;
    size_t i = 0;
    auto match_start = [&](size_t pos, size_t* sclen) -> bool {
        if (pos + 3 < size && data[pos] == 0 && data[pos+1] == 0 &&
            data[pos+2] == 0 && data[pos+3] == 1) { *sclen = 4; return true; }
        if (pos + 2 < size && data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) {
            *sclen = 3; return true;
        }
        return false;
    };
    while (i < size) {
        size_t sclen = 0;
        if (!match_start(i, &sclen)) { ++i; continue; }
        size_t start = i + sclen;
        size_t j = start;
        while (j < size) {
            size_t scl2 = 0;
            if (match_start(j, &scl2)) break;
            ++j;
        }
        if (j > start) nalus.emplace_back(data + start, data + j);
        i = j;
    }
    return nalus;
}

std::vector<uint8_t> BuildOnMetaData(int width, int height, double fps,
                                     int videoBitrateKbps,
                                     int audioBitrateKbps,
                                     int audioSampleRate, int audioChannels) {
    std::vector<uint8_t> p;
    rtmp::AmfEncodeString(p, "onMetaData");

    rtmp::AmfObject o;
    o.emplace_back("duration",        rtmp::AmfValue::Number(0));
    o.emplace_back("width",           rtmp::AmfValue::Number(width));
    o.emplace_back("height",          rtmp::AmfValue::Number(height));
    o.emplace_back("videodatarate",   rtmp::AmfValue::Number(videoBitrateKbps));
    o.emplace_back("framerate",       rtmp::AmfValue::Number(fps));
    o.emplace_back("videocodecid",    rtmp::AmfValue::Number(7)); // AVC
    o.emplace_back("audiodatarate",   rtmp::AmfValue::Number(audioBitrateKbps));
    o.emplace_back("audiosamplerate", rtmp::AmfValue::Number(audioSampleRate));
    o.emplace_back("audiosamplesize", rtmp::AmfValue::Number(16));
    o.emplace_back("stereo",          rtmp::AmfValue::Bool(audioChannels == 2));
    o.emplace_back("audiocodecid",    rtmp::AmfValue::Number(10)); // AAC
    o.emplace_back("encoder",         rtmp::AmfValue::Str("RtmpDesktopStreamer"));
    rtmp::AmfEncodeEcmaArray(p, o);

    return p;
}

} // namespace flv
