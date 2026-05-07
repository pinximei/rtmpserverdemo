#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <variant>

// RTMP 1.0 protocol primitives:
//  - 1536-byte handshake (S0/S1/S2 simplified, no FMS digest)
//  - Chunk stream encode/decode
//  - AMF0 encode/decode (subset: number, string, boolean, null, object, ecma-array)
//
// All multi-byte ints in RTMP are big-endian unless noted.

namespace rtmp {

// Message Type IDs (AMF0 control / data flow)
enum MessageType : uint8_t {
    MSG_SET_CHUNK_SIZE   = 1,
    MSG_ABORT            = 2,
    MSG_ACK              = 3,
    MSG_USER_CONTROL     = 4,
    MSG_WINDOW_ACK_SIZE  = 5,
    MSG_SET_PEER_BW      = 6,
    MSG_AUDIO            = 8,
    MSG_VIDEO            = 9,
    MSG_AMF3_DATA        = 15,
    MSG_AMF3_CMD         = 17,
    MSG_AMF0_DATA        = 18,
    MSG_AMF0_CMD         = 20,
};

// One fully-decoded message ready for app layer
struct Message {
    uint8_t  typeId = 0;
    uint32_t streamId = 0;        // message stream id
    uint32_t timestamp = 0;       // absolute (ms)
    std::vector<uint8_t> payload;
};

// AMF0 value
struct AmfValue;
using AmfObject = std::vector<std::pair<std::string, AmfValue>>;

struct AmfValue {
    enum Type { T_NUMBER, T_BOOL, T_STRING, T_OBJECT, T_NULL, T_ECMA_ARRAY };
    Type type = T_NULL;
    double num = 0.0;
    bool   b = false;
    std::string str;
    AmfObject obj;          // also used for ecma array

    static AmfValue Number(double v) { AmfValue x; x.type=T_NUMBER; x.num=v; return x; }
    static AmfValue Bool(bool v)     { AmfValue x; x.type=T_BOOL;   x.b=v;   return x; }
    static AmfValue Str(std::string v){ AmfValue x; x.type=T_STRING;x.str=std::move(v); return x; }
    static AmfValue Null()           { AmfValue x; x.type=T_NULL;   return x; }
    static AmfValue Obj(AmfObject v) { AmfValue x; x.type=T_OBJECT; x.obj=std::move(v); return x; }
    static AmfValue Ecma(AmfObject v){ AmfValue x; x.type=T_ECMA_ARRAY; x.obj=std::move(v); return x; }
};

// AMF0 encode helpers (append bytes to out)
void AmfEncodeNumber(std::vector<uint8_t>& out, double v);
void AmfEncodeBool(std::vector<uint8_t>& out, bool v);
void AmfEncodeString(std::vector<uint8_t>& out, const std::string& s);
void AmfEncodeNull(std::vector<uint8_t>& out);
void AmfEncodeObject(std::vector<uint8_t>& out, const AmfObject& o);
void AmfEncodeEcmaArray(std::vector<uint8_t>& out, const AmfObject& o);
void AmfEncodeValue(std::vector<uint8_t>& out, const AmfValue& v);

// AMF0 decode (returns # bytes consumed; on error returns 0)
size_t AmfDecodeValue(const uint8_t* data, size_t size, AmfValue& out);
// Decode a sequence of top-level values until size exhausted
std::vector<AmfValue> AmfDecodeAll(const uint8_t* data, size_t size);

// Chunk-stream encoder: serialize a Message into RTMP chunks.
// chunkSize is current peer chunk size for this direction.
// csid: chunk stream id (2..63 single-byte form). For protocol control use 2,
// for invokes use 3, for video use 6, for audio use 4 (typical FFmpeg picks).
void EncodeChunks(std::vector<uint8_t>& out,
                  const Message& msg,
                  uint32_t chunkSize,
                  uint8_t  csid);

// Chunk-stream decoder: incremental, feed bytes and pop completed messages.
class ChunkDecoder {
public:
    void SetPeerChunkSize(uint32_t cs) { m_peerChunkSize = cs; }
    uint32_t PeerChunkSize() const { return m_peerChunkSize; }

    // Append raw bytes from socket. Returns true unless protocol error.
    bool Feed(const uint8_t* data, size_t size);

    // Pop one fully-assembled message; returns false if none available.
    bool Pop(Message& out);

private:
    struct StreamState {
        uint8_t  fmt = 0;
        uint32_t timestamp = 0;
        uint32_t timestampDelta = 0;
        uint32_t msgLength = 0;
        uint8_t  msgTypeId = 0;
        uint32_t msgStreamId = 0;
        std::vector<uint8_t> partial; // accumulated payload for in-progress msg
        bool hasExtTs = false;
        uint32_t extTs = 0;
    };

    std::vector<uint8_t> m_buffer;
    std::map<uint32_t, StreamState> m_streams;
    std::vector<Message> m_ready;
    uint32_t m_peerChunkSize = 128; // RTMP default
};

// 1536-byte simple handshake. Caller drives socket I/O.
// Server-side: ServerHandshake reads C0+C1, writes S0+S1+S2, reads C2.
// Client-side: ClientHandshake writes C0+C1, reads S0+S1+S2, writes C2.
// Returns true on success. Read/Write are blocking callbacks.
struct IO {
    // Return true on success and exactly n bytes transferred.
    virtual bool Read(uint8_t* dst, size_t n) = 0;
    virtual bool Write(const uint8_t* src, size_t n) = 0;
    virtual ~IO() = default;
};

bool ServerHandshake(IO& io);
bool ClientHandshake(IO& io);

} // namespace rtmp
