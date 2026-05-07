#include "RtmpProtocol.h"
#include "DebugLog.h"

#include <Windows.h>
#include <sysinfoapi.h>
#include <cstring>
#include <random>
#include <algorithm>

namespace rtmp {

// --- AMF0 encode -----------------------------------------------------------

static void put_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
static void put_be16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)(v & 0xFF));
}
static void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)((v >> 24) & 0xFF));
    o.push_back((uint8_t)((v >> 16) & 0xFF));
    o.push_back((uint8_t)((v >> 8) & 0xFF));
    o.push_back((uint8_t)(v & 0xFF));
}
static void put_be_double(std::vector<uint8_t>& o, double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 7; i >= 0; --i) o.push_back((uint8_t)((u >> (i * 8)) & 0xFF));
}

void AmfEncodeNumber(std::vector<uint8_t>& out, double v) {
    out.push_back(0x00);
    put_be_double(out, v);
}
void AmfEncodeBool(std::vector<uint8_t>& out, bool v) {
    out.push_back(0x01);
    out.push_back(v ? 0x01 : 0x00);
}
void AmfEncodeString(std::vector<uint8_t>& out, const std::string& s) {
    if (s.size() <= 0xFFFF) {
        out.push_back(0x02);
        put_be16(out, (uint16_t)s.size());
        out.insert(out.end(), s.begin(), s.end());
    } else {
        out.push_back(0x0C); // long string
        put_be32(out, (uint32_t)s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
}
void AmfEncodeNull(std::vector<uint8_t>& out) {
    out.push_back(0x05);
}
static void AmfEncodeStringRaw(std::vector<uint8_t>& out, const std::string& s) {
    put_be16(out, (uint16_t)s.size());
    out.insert(out.end(), s.begin(), s.end());
}
void AmfEncodeObject(std::vector<uint8_t>& out, const AmfObject& o) {
    out.push_back(0x03); // object marker
    for (const auto& kv : o) {
        AmfEncodeStringRaw(out, kv.first);
        AmfEncodeValue(out, kv.second);
    }
    AmfEncodeStringRaw(out, ""); // empty key
    out.push_back(0x09);          // object end marker
}
void AmfEncodeEcmaArray(std::vector<uint8_t>& out, const AmfObject& o) {
    out.push_back(0x08); // ecma array marker
    put_be32(out, (uint32_t)o.size());
    for (const auto& kv : o) {
        AmfEncodeStringRaw(out, kv.first);
        AmfEncodeValue(out, kv.second);
    }
    AmfEncodeStringRaw(out, "");
    out.push_back(0x09);
}
void AmfEncodeValue(std::vector<uint8_t>& out, const AmfValue& v) {
    switch (v.type) {
        case AmfValue::T_NUMBER:     AmfEncodeNumber(out, v.num); break;
        case AmfValue::T_BOOL:       AmfEncodeBool(out, v.b); break;
        case AmfValue::T_STRING:     AmfEncodeString(out, v.str); break;
        case AmfValue::T_NULL:       AmfEncodeNull(out); break;
        case AmfValue::T_OBJECT:     AmfEncodeObject(out, v.obj); break;
        case AmfValue::T_ECMA_ARRAY: AmfEncodeEcmaArray(out, v.obj); break;
    }
}

// --- AMF0 decode -----------------------------------------------------------

static bool need(const uint8_t*, size_t size, size_t off, size_t n) {
    return off + n <= size;
}
static uint16_t get_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static double get_be_double(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | p[i];
    double v;
    std::memcpy(&v, &u, 8);
    return v;
}

// Returns bytes consumed, or 0 on error
size_t AmfDecodeValue(const uint8_t* data, size_t size, AmfValue& out) {
    if (size < 1) return 0;
    uint8_t marker = data[0];
    size_t off = 1;
    switch (marker) {
        case 0x00: { // number
            if (!need(data, size, off, 8)) return 0;
            out = AmfValue::Number(get_be_double(data + off));
            return off + 8;
        }
        case 0x01: { // boolean
            if (!need(data, size, off, 1)) return 0;
            out = AmfValue::Bool(data[off] != 0);
            return off + 1;
        }
        case 0x02: { // string
            if (!need(data, size, off, 2)) return 0;
            uint16_t n = get_be16(data + off);
            off += 2;
            if (!need(data, size, off, n)) return 0;
            out = AmfValue::Str(std::string((const char*)(data + off), n));
            return off + n;
        }
        case 0x03: { // object
            AmfObject obj;
            while (true) {
                if (!need(data, size, off, 2)) return 0;
                uint16_t klen = get_be16(data + off);
                off += 2;
                if (!need(data, size, off, klen)) return 0;
                std::string key((const char*)(data + off), klen);
                off += klen;
                if (klen == 0) {
                    // expect end marker 0x09
                    if (!need(data, size, off, 1)) return 0;
                    if (data[off] != 0x09) return 0;
                    off += 1;
                    out = AmfValue::Obj(std::move(obj));
                    return off;
                }
                AmfValue v;
                size_t c = AmfDecodeValue(data + off, size - off, v);
                if (c == 0) return 0;
                off += c;
                obj.emplace_back(std::move(key), std::move(v));
            }
        }
        case 0x05: { // null
            out = AmfValue::Null();
            return off;
        }
        case 0x06: { // undefined - treat as null
            out = AmfValue::Null();
            return off;
        }
        case 0x08: { // ecma array
            if (!need(data, size, off, 4)) return 0;
            off += 4; // skip count
            AmfObject obj;
            while (true) {
                if (!need(data, size, off, 2)) return 0;
                uint16_t klen = get_be16(data + off);
                off += 2;
                if (!need(data, size, off, klen)) return 0;
                std::string key((const char*)(data + off), klen);
                off += klen;
                if (klen == 0) {
                    if (!need(data, size, off, 1)) return 0;
                    if (data[off] != 0x09) return 0;
                    off += 1;
                    out = AmfValue::Ecma(std::move(obj));
                    return off;
                }
                AmfValue v;
                size_t c = AmfDecodeValue(data + off, size - off, v);
                if (c == 0) return 0;
                off += c;
                obj.emplace_back(std::move(key), std::move(v));
            }
        }
        case 0x0C: { // long string
            if (!need(data, size, off, 4)) return 0;
            uint32_t n = get_be32(data + off);
            off += 4;
            if (!need(data, size, off, n)) return 0;
            out = AmfValue::Str(std::string((const char*)(data + off), n));
            return off + n;
        }
        default:
            DLOG("AMF decode: unknown marker 0x%02X", marker);
            return 0;
    }
}

std::vector<AmfValue> AmfDecodeAll(const uint8_t* data, size_t size) {
    std::vector<AmfValue> result;
    size_t off = 0;
    while (off < size) {
        AmfValue v;
        size_t c = AmfDecodeValue(data + off, size - off, v);
        if (c == 0) break;
        off += c;
        result.push_back(std::move(v));
    }
    return result;
}

// --- Chunk encode ----------------------------------------------------------

void EncodeChunks(std::vector<uint8_t>& out,
                  const Message& msg,
                  uint32_t chunkSize,
                  uint8_t  csid)
{
    // Always use Type 0 basic header (full message header). Simpler and
    // perfectly valid; bandwidth overhead vs Type 1/2 is negligible at our
    // chunk size of 4096.

    const uint32_t totalLen = (uint32_t)msg.payload.size();
    uint32_t sent = 0;
    bool first = true;

    while (sent < totalLen || (totalLen == 0 && first)) {
        if (first) {
            // fmt=0, csid in low 6 bits
            out.push_back((uint8_t)(0x00 | (csid & 0x3F)));

            uint32_t ts = msg.timestamp;
            bool ext = ts >= 0xFFFFFF;
            uint32_t tsField = ext ? 0xFFFFFF : ts;
            out.push_back((uint8_t)((tsField >> 16) & 0xFF));
            out.push_back((uint8_t)((tsField >> 8) & 0xFF));
            out.push_back((uint8_t)(tsField & 0xFF));

            // message length (3 bytes BE)
            out.push_back((uint8_t)((totalLen >> 16) & 0xFF));
            out.push_back((uint8_t)((totalLen >> 8) & 0xFF));
            out.push_back((uint8_t)(totalLen & 0xFF));

            // type id
            out.push_back(msg.typeId);

            // message stream id (4 bytes LITTLE-endian by RTMP spec)
            uint32_t sid = msg.streamId;
            out.push_back((uint8_t)(sid & 0xFF));
            out.push_back((uint8_t)((sid >> 8) & 0xFF));
            out.push_back((uint8_t)((sid >> 16) & 0xFF));
            out.push_back((uint8_t)((sid >> 24) & 0xFF));

            if (ext) {
                out.push_back((uint8_t)((ts >> 24) & 0xFF));
                out.push_back((uint8_t)((ts >> 16) & 0xFF));
                out.push_back((uint8_t)((ts >> 8) & 0xFF));
                out.push_back((uint8_t)(ts & 0xFF));
            }

            first = false;
        } else {
            // fmt=3 continuation
            out.push_back((uint8_t)(0xC0 | (csid & 0x3F)));
            // extended timestamp continuation: per spec, if first chunk had
            // extended ts, repeat it on every continuation.
            if (msg.timestamp >= 0xFFFFFF) {
                uint32_t ts = msg.timestamp;
                out.push_back((uint8_t)((ts >> 24) & 0xFF));
                out.push_back((uint8_t)((ts >> 16) & 0xFF));
                out.push_back((uint8_t)((ts >> 8) & 0xFF));
                out.push_back((uint8_t)(ts & 0xFF));
            }
        }

        if (totalLen == 0) break;

        uint32_t remain = totalLen - sent;
        uint32_t take = remain < chunkSize ? remain : chunkSize;
        out.insert(out.end(), msg.payload.begin() + sent,
                              msg.payload.begin() + sent + take);
        sent += take;
    }
}

// --- Chunk decode ----------------------------------------------------------

bool ChunkDecoder::Feed(const uint8_t* data, size_t size) {
    m_buffer.insert(m_buffer.end(), data, data + size);

    while (true) {
        if (m_buffer.empty()) return true;
        size_t off = 0;
        size_t avail = m_buffer.size();

        // Basic header
        uint8_t b0 = m_buffer[off];
        uint8_t fmt = (b0 >> 6) & 0x03;
        uint32_t csid = b0 & 0x3F;
        size_t bhSize = 1;
        if (csid == 0) {
            if (avail < 2) return true;
            csid = 64 + m_buffer[off + 1];
            bhSize = 2;
        } else if (csid == 1) {
            if (avail < 3) return true;
            csid = 64 + m_buffer[off + 1] + ((uint32_t)m_buffer[off + 2] << 8);
            bhSize = 3;
        }

        if (avail < bhSize) return true;
        off += bhSize;

        // Message header sizes: fmt0=11, fmt1=7, fmt2=3, fmt3=0
        size_t mhSize = 0;
        switch (fmt) {
            case 0: mhSize = 11; break;
            case 1: mhSize = 7; break;
            case 2: mhSize = 3; break;
            case 3: mhSize = 0; break;
        }
        if (avail < off + mhSize) return true;

        StreamState& st = m_streams[csid];

        uint32_t ts = 0;
        bool extTsPresent = false;
        size_t mhAfter = off;

        if (fmt <= 2) {
            uint32_t f = ((uint32_t)m_buffer[mhAfter] << 16) |
                         ((uint32_t)m_buffer[mhAfter + 1] << 8) |
                          (uint32_t)m_buffer[mhAfter + 2];
            mhAfter += 3;
            ts = f;
            if (ts == 0xFFFFFF) extTsPresent = true;

            if (fmt <= 1) {
                uint32_t mlen = ((uint32_t)m_buffer[mhAfter] << 16) |
                                ((uint32_t)m_buffer[mhAfter + 1] << 8) |
                                 (uint32_t)m_buffer[mhAfter + 2];
                mhAfter += 3;
                uint8_t mtype = m_buffer[mhAfter];
                mhAfter += 1;
                st.msgLength = mlen;
                st.msgTypeId = mtype;

                if (fmt == 0) {
                    // 4-byte message stream id, LITTLE-endian
                    uint32_t sid =  (uint32_t)m_buffer[mhAfter] |
                                   ((uint32_t)m_buffer[mhAfter + 1] << 8) |
                                   ((uint32_t)m_buffer[mhAfter + 2] << 16) |
                                   ((uint32_t)m_buffer[mhAfter + 3] << 24);
                    mhAfter += 4;
                    st.msgStreamId = sid;
                }
            }
        } else {
            // fmt 3 may carry extended ts if first frame had one
            extTsPresent = st.hasExtTs;
        }

        size_t headerSize = mhAfter - 0;
        // optional extended timestamp 4 bytes
        if (extTsPresent) {
            if (avail < headerSize + 4) return true;
            uint32_t ext = ((uint32_t)m_buffer[headerSize] << 24) |
                           ((uint32_t)m_buffer[headerSize + 1] << 16) |
                           ((uint32_t)m_buffer[headerSize + 2] << 8) |
                            (uint32_t)m_buffer[headerSize + 3];
            if (fmt == 3) {
                // continuation: just consume; do not change accumulated ts
                (void)ext;
            } else {
                ts = ext;
            }
            headerSize += 4;
        }

        if (fmt == 0) {
            st.timestamp = ts;
            st.timestampDelta = 0;
            st.hasExtTs = (ts >= 0xFFFFFF);
        } else if (fmt == 1 || fmt == 2) {
            st.timestampDelta = ts;
            // only apply delta on first chunk of message
            if (st.partial.empty()) {
                st.timestamp += ts;
                st.hasExtTs = (ts >= 0xFFFFFF);
            }
        }
        // fmt == 3: nothing to update from header

        // Compute payload length for this chunk
        if (st.msgLength == 0) {
            // unknown / no-payload control or empty
            // consume header and continue
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + headerSize);
            continue;
        }
        uint32_t still = st.msgLength - (uint32_t)st.partial.size();
        uint32_t take = still < m_peerChunkSize ? still : m_peerChunkSize;

        if (avail < headerSize + take) return true;

        st.partial.insert(st.partial.end(),
                          m_buffer.begin() + headerSize,
                          m_buffer.begin() + headerSize + take);

        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + headerSize + take);

        if (st.partial.size() == st.msgLength) {
            Message msg;
            msg.typeId    = st.msgTypeId;
            msg.streamId  = st.msgStreamId;
            msg.timestamp = st.timestamp;
            msg.payload   = std::move(st.partial);
            st.partial.clear();
            m_ready.push_back(std::move(msg));

            // Handle protocol control inline: SET_CHUNK_SIZE
            const Message& m = m_ready.back();
            if (m.typeId == MSG_SET_CHUNK_SIZE && m.payload.size() >= 4) {
                uint32_t ncs = ((uint32_t)m.payload[0] << 24) |
                               ((uint32_t)m.payload[1] << 16) |
                               ((uint32_t)m.payload[2] << 8)  |
                                (uint32_t)m.payload[3];
                if (ncs > 0 && ncs <= 0xFFFFFF) {
                    m_peerChunkSize = ncs;
                    DLOG("peer chunk size -> %u", ncs);
                }
            }
        }
    }
}

bool ChunkDecoder::Pop(Message& out) {
    if (m_ready.empty()) return false;
    out = std::move(m_ready.front());
    m_ready.erase(m_ready.begin());
    return true;
}

// --- Handshake -------------------------------------------------------------

bool ServerHandshake(IO& io) {
    uint8_t c0 = 0;
    if (!io.Read(&c0, 1)) { DLOG("hs: read C0 failed"); return false; }
    if (c0 != 0x03) { DLOG("hs: bad C0 0x%02X", c0); return false; }

    std::vector<uint8_t> c1(1536);
    if (!io.Read(c1.data(), 1536)) { DLOG("hs: read C1 failed"); return false; }

    // S0
    uint8_t s0 = 0x03;
    if (!io.Write(&s0, 1)) { DLOG("hs: write S0 failed"); return false; }

    // S1: 4 bytes time + 4 bytes zero + 1528 random
    std::vector<uint8_t> s1(1536);
    uint32_t now = GetTickCount();
    s1[0] = (uint8_t)(now >> 24);
    s1[1] = (uint8_t)(now >> 16);
    s1[2] = (uint8_t)(now >> 8);
    s1[3] = (uint8_t)now;
    s1[4] = s1[5] = s1[6] = s1[7] = 0;
    std::mt19937 rng((uint32_t)GetTickCount());
    for (size_t i = 8; i < 1536; ++i) s1[i] = (uint8_t)rng();
    if (!io.Write(s1.data(), 1536)) { DLOG("hs: write S1 failed"); return false; }

    // S2 = echo of C1 (per simple handshake)
    if (!io.Write(c1.data(), 1536)) { DLOG("hs: write S2 failed"); return false; }

    // C2
    std::vector<uint8_t> c2(1536);
    if (!io.Read(c2.data(), 1536)) { DLOG("hs: read C2 failed"); return false; }

    return true;
}

bool ClientHandshake(IO& io) {
    uint8_t c0 = 0x03;
    if (!io.Write(&c0, 1)) return false;

    std::vector<uint8_t> c1(1536);
    uint32_t now = GetTickCount();
    c1[0] = (uint8_t)(now >> 24);
    c1[1] = (uint8_t)(now >> 16);
    c1[2] = (uint8_t)(now >> 8);
    c1[3] = (uint8_t)now;
    c1[4] = c1[5] = c1[6] = c1[7] = 0;
    std::mt19937 rng((uint32_t)GetTickCount());
    for (size_t i = 8; i < 1536; ++i) c1[i] = (uint8_t)rng();
    if (!io.Write(c1.data(), 1536)) return false;

    uint8_t s0 = 0;
    if (!io.Read(&s0, 1)) return false;
    if (s0 != 0x03) { DLOG("hs(client): bad S0 0x%02X", s0); return false; }
    std::vector<uint8_t> s1(1536), s2(1536);
    if (!io.Read(s1.data(), 1536)) return false;
    if (!io.Read(s2.data(), 1536)) return false;

    // C2 = echo of S1
    if (!io.Write(s1.data(), 1536)) return false;

    return true;
}

} // namespace rtmp
