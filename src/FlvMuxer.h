#pragma once

#include <cstdint>
#include <vector>

// Helpers to build FLV-style tag bodies that go directly into RTMP video/audio
// messages. RTMP video/audio payloads ARE the FLV "tag data" portion (no FLV
// header / no tag size suffix).
//
// Video tag (H.264):
//   byte0 : (FrameType<<4)|CodecID  -- FrameType: 1 keyframe / 2 inter
//                                      CodecID: 7 = AVC
//   byte1 : AVCPacketType  0=seq header, 1=NALU, 2=end
//   byte2-4: composition time (signed BE24, 0 if no B-frames)
//   data... : if seq header => AVCDecoderConfigurationRecord
//             if NALU => one or more (NALU length 4 BE)|NALU bytes
//
// Audio tag (AAC):
//   byte0 : (SoundFormat<<4)|(SoundRate<<2)|(SoundSize<<1)|SoundType
//           SoundFormat=10 (AAC), SoundRate=3 (44k+; AAC always uses this),
//           SoundSize=1 (16-bit), SoundType=1 (stereo)
//   byte1 : AACPacketType  0=AudioSpecificConfig, 1=AAC raw
//   data...

namespace flv {

// Build AVCDecoderConfigurationRecord from one SPS + one PPS (most common case).
std::vector<uint8_t> BuildAvcDecoderConfigRecord(const std::vector<uint8_t>& sps,
                                                 const std::vector<uint8_t>& pps);

// Build a video tag body containing one access unit consisting of NALUs.
// `nalus` is a list of raw NAL units (no Annex-B start code, no length prefix).
std::vector<uint8_t> BuildVideoTagAvcNalu(const std::vector<std::vector<uint8_t>>& nalus,
                                          bool keyframe);

// Build the "sequence header" video tag (AVC decoder config).
std::vector<uint8_t> BuildVideoTagAvcSeqHeader(const std::vector<uint8_t>& sps,
                                                const std::vector<uint8_t>& pps);

// Build AudioSpecificConfig (2 bytes for 2-byte AAC LC config; 5 bytes if SBR).
// objectType: 2 = AAC LC. sampleRateIdx see table. channelConfig 1=mono,2=stereo
std::vector<uint8_t> BuildAudioSpecificConfig(int objectType,
                                              int sampleRateHz,
                                              int channelConfig);

std::vector<uint8_t> BuildAudioTagAacSeqHeader(const std::vector<uint8_t>& asc);
std::vector<uint8_t> BuildAudioTagAacRaw(const uint8_t* data, size_t size);

// Split Annex-B stream into separate NALUs.
std::vector<std::vector<uint8_t>> SplitAnnexB(const uint8_t* data, size_t size);

// Build the onMetaData script-data tag payload.
std::vector<uint8_t> BuildOnMetaData(int width, int height, double fps,
                                     int videoBitrateKbps,
                                     int audioBitrateKbps,
                                     int audioSampleRate, int audioChannels);

} // namespace flv
