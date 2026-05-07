#include "RtmpServer.h"
#include "RtmpProtocol.h"
#include "DebugLog.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <deque>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace rtmp {

// Per-client send queue limit. The whole point of this queue is to absorb
// short bursts (one IDR ~200KB) without blocking the encoder thread, while
// preventing TCP/socket buffer from accumulating multi-second backlog when
// the client is too slow. 1 MB ≈ 1.3 seconds @ 6 Mbps; once we exceed it we
// start dropping non-critical packets.
constexpr size_t kClientSendQueueByteLimit = 1 * 1024 * 1024;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(SOCKET s, RtmpServer* owner) : m_sock(s), m_owner(owner) {}
    ~ClientSession() { Close(); }

    void Start() {
        m_thread = std::thread([self = shared_from_this()] { self->Run(); });
        m_senderThread = std::thread([self = shared_from_this()] { self->SenderLoop(); });
    }
    void Stop() {
        m_quit = true;
        // Wake the sender so it can exit promptly.
        {
            std::lock_guard<std::mutex> lk(m_qMtx);
            m_qCv.notify_all();
        }
        Close();
        if (m_thread.joinable() && std::this_thread::get_id() != m_thread.get_id()) {
            m_thread.join();
        } else if (m_thread.joinable()) {
            m_thread.detach();
        }
        if (m_senderThread.joinable() &&
            std::this_thread::get_id() != m_senderThread.get_id()) {
            m_senderThread.join();
        } else if (m_senderThread.joinable()) {
            m_senderThread.detach();
        }
    }

    bool Alive() const { return m_alive.load(); }
    bool IsPlaying() const { return m_playing.load(); }

    void SeedTsBase(uint32_t base) {
        m_tsBase = base;
        m_tsBaseSet = true;
    }
    uint32_t RebaseTs(uint32_t ts) {
        // Per-client timestamp rebase so the player sees a clock that starts
        // at zero when it joins. Without this, every late joiner sees
        // monotonically growing absolute timestamps which break player buffer.
        if (!m_tsBaseSet) {
            m_tsBase = ts;
            m_tsBaseSet = true;
            return 0;
        }
        return (ts >= m_tsBase) ? (ts - m_tsBase) : 0;
    }
    bool SendVideo(const std::vector<uint8_t>& tag, bool isKey, uint32_t ts) {
        if (!m_playing) return true;
        // New-client gate: drop everything until we see the first keyframe
        // produced for this client. This guarantees the player starts
        // decoding from a fresh IDR with zero historical lag.
        if (m_waitingForKey) {
            if (!isKey) return true;
            m_waitingForKey = false;
            // Anchor the per-client clock to this IDR.
            m_tsBase = ts;
            m_tsBaseSet = true;
        }
        Message m;
        m.typeId = MSG_VIDEO;
        m.streamId = m_streamId;
        m.timestamp = RebaseTs(ts);
        m.payload = tag;
        return EnqueueMedia(m, 6, /*isVideo*/true, /*isKey*/isKey);
    }
    bool SendAudio(const std::vector<uint8_t>& tag, uint32_t ts) {
        if (!m_playing) return true;
        // Hold audio until the gating keyframe has been delivered, otherwise
        // the player sees audio timestamps before the first video frame and
        // either stalls or skews A/V sync.
        if (m_waitingForKey) return true;
        Message m;
        m.typeId = MSG_AUDIO;
        m.streamId = m_streamId;
        m.timestamp = RebaseTs(ts);
        m.payload = tag;
        return EnqueueMedia(m, 4, /*isVideo*/false, /*isKey*/false);
    }
    void ArmWaitForKeyframe() { m_waitingForKey = true; }
    bool SendVideoUnconditional(const std::vector<uint8_t>& tag, uint32_t ts) {
        Message m;
        m.typeId = MSG_VIDEO;
        m.streamId = m_streamId;
        m.timestamp = ts;
        m.payload = tag;
        // Sequence headers must never be dropped — mark as critical.
        return EnqueueMedia(m, 6, /*isVideo*/true, /*isKey*/true);
    }
    bool SendAudioUnconditional(const std::vector<uint8_t>& tag, uint32_t ts) {
        Message m;
        m.typeId = MSG_AUDIO;
        m.streamId = m_streamId;
        m.timestamp = ts;
        m.payload = tag;
        // AAC sequence header — must not be dropped.
        return EnqueueMedia(m, 4, /*isVideo*/false, /*isKey*/true);
    }
    bool SendMetaData(const std::vector<uint8_t>& payload) {
        Message m;
        m.typeId = MSG_AMF0_DATA;
        m.streamId = m_streamId;
        m.timestamp = 0;
        m.payload = payload;
        return EnqueueMedia(m, 5, /*isVideo*/false, /*isKey*/true);
    }

private:
    void Close() {
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        m_alive = false;
    }

    bool ReadAll(uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = ::recv(m_sock, (char*)dst + got, (int)(n - got), 0);
            if (r <= 0) return false;
            got += (size_t)r;
        }
        return true;
    }
    bool WriteAll(const uint8_t* src, size_t n) {
        std::lock_guard<std::mutex> lk(m_writeMtx);
        if (m_sock == INVALID_SOCKET) return false;
        size_t sent = 0;
        while (sent < n) {
            int r = ::send(m_sock, (const char*)src + sent, (int)(n - sent), 0);
            if (r <= 0) return false;
            sent += (size_t)r;
        }
        return true;
    }

    struct SimpleIO : IO {
        ClientSession* s;
        SimpleIO(ClientSession* x) : s(x) {}
        bool Read(uint8_t* dst, size_t n) override { return s->ReadAll(dst, n); }
        bool Write(const uint8_t* src, size_t n) override { return s->WriteAll(src, n); }
    };

    bool SendOneMessage(const Message& m, uint8_t csid) {
        std::vector<uint8_t> buf;
        EncodeChunks(buf, m, m_outChunkSize, csid);
        return WriteAll(buf.data(), buf.size());
    }

    // Media packet routed through the per-client send queue. The encoder
    // thread enqueues; SenderLoop drains. If the player is too slow, the
    // queue grows. We never block the producer:
    //   - non-key video frames are dropped first (P-frames are disposable
    //     between IDRs)
    //   - if even after dropping P frames we still exceed the limit, we
    //     also drop audio packets (audio is best-effort under congestion)
    //   - keyframes / sequence headers / metadata are never dropped, but
    //     when we drop P-frames we ALSO ask the server to emit a fresh IDR
    //     so the client resyncs cleanly after the gap.
    bool EnqueueMedia(const Message& m, uint8_t csid, bool isVideo, bool isKey) {
        std::vector<uint8_t> buf;
        EncodeChunks(buf, m, m_outChunkSize, csid);
        size_t bsz = buf.size();

        std::lock_guard<std::mutex> lk(m_qMtx);
        if (m_quit) return false;

        // Drop strategy: if adding this packet would push the queue over
        // limit, first try to evict P-frames already queued.
        if (m_qBytes + bsz > kClientSendQueueByteLimit) {
            size_t freed = 0;
            int dropped = 0;
            for (auto it = m_q.begin(); it != m_q.end(); ) {
                if (m_qBytes + bsz - freed <= kClientSendQueueByteLimit) break;
                if (it->isVideo && !it->isKey) {
                    freed += it->buf.size();
                    ++dropped;
                    it = m_q.erase(it);
                } else {
                    ++it;
                }
            }
            m_qBytes -= freed;
            if (dropped > 0) {
                ++m_dropEvents;
                // Tell the server we need a fresh IDR ASAP — otherwise the
                // gap left by dropped P-frames will produce decode errors
                // until the next regular keyframe.
                m_owner->RequestKeyframe();
                if ((m_dropEvents & 0xFF) == 1) {
                    DLOG("client backlog: dropped %d P-frames (qBytes=%zu)",
                         dropped, m_qBytes);
                }
            }

            // Still too big? Also drop queued non-key audio.
            if (m_qBytes + bsz > kClientSendQueueByteLimit) {
                size_t freedA = 0;
                int droppedA = 0;
                for (auto it = m_q.begin(); it != m_q.end(); ) {
                    if (m_qBytes + bsz - freedA <= kClientSendQueueByteLimit) break;
                    if (!it->isVideo && !it->isKey) {
                        freedA += it->buf.size();
                        ++droppedA;
                        it = m_q.erase(it);
                    } else {
                        ++it;
                    }
                }
                m_qBytes -= freedA;
                if (droppedA > 0) {
                    DLOG("client backlog: dropped %d audio packets (qBytes=%zu)",
                         droppedA, m_qBytes);
                }
            }

            // Last resort: if THIS packet itself is non-critical and the
            // queue is still over budget, drop it.
            if (m_qBytes + bsz > kClientSendQueueByteLimit && !isKey) {
                return true; // pretend success — frame is intentionally dropped
            }
        }

        OutPacket p;
        p.buf = std::move(buf);
        p.isVideo = isVideo;
        p.isKey = isKey;
        m_qBytes += bsz;
        m_q.push_back(std::move(p));
        m_qCv.notify_one();
        return true;
    }

    void SenderLoop() {
        std::vector<uint8_t> tmp;
        while (!m_quit) {
            {
                std::unique_lock<std::mutex> lk(m_qMtx);
                m_qCv.wait(lk, [&]{ return m_quit || !m_q.empty(); });
                if (m_quit) break;
                tmp = std::move(m_q.front().buf);
                m_qBytes -= tmp.size();
                m_q.pop_front();
            }
            if (!WriteAll(tmp.data(), tmp.size())) {
                DLOG("session: send failed; closing");
                Close();
                break;
            }
        }
    }

    bool SendSetChunkSize(uint32_t cs) {
        Message m; m.typeId = MSG_SET_CHUNK_SIZE; m.streamId = 0; m.timestamp = 0;
        m.payload.resize(4);
        m.payload[0] = (uint8_t)((cs >> 24) & 0xFF);
        m.payload[1] = (uint8_t)((cs >> 16) & 0xFF);
        m.payload[2] = (uint8_t)((cs >> 8) & 0xFF);
        m.payload[3] = (uint8_t)(cs & 0xFF);
        if (!SendOneMessage(m, 2)) return false;
        m_outChunkSize = cs;
        return true;
    }
    bool SendWindowAck(uint32_t bytes) {
        Message m; m.typeId = MSG_WINDOW_ACK_SIZE; m.streamId = 0; m.timestamp = 0;
        m.payload.resize(4);
        m.payload[0] = (uint8_t)((bytes >> 24) & 0xFF);
        m.payload[1] = (uint8_t)((bytes >> 16) & 0xFF);
        m.payload[2] = (uint8_t)((bytes >> 8) & 0xFF);
        m.payload[3] = (uint8_t)(bytes & 0xFF);
        return SendOneMessage(m, 2);
    }
    bool SendSetPeerBw(uint32_t bytes) {
        Message m; m.typeId = MSG_SET_PEER_BW; m.streamId = 0; m.timestamp = 0;
        m.payload.resize(5);
        m.payload[0] = (uint8_t)((bytes >> 24) & 0xFF);
        m.payload[1] = (uint8_t)((bytes >> 16) & 0xFF);
        m.payload[2] = (uint8_t)((bytes >> 8) & 0xFF);
        m.payload[3] = (uint8_t)(bytes & 0xFF);
        m.payload[4] = 2;
        return SendOneMessage(m, 2);
    }
    bool SendUserCtrlStreamBegin(uint32_t streamId) {
        Message m; m.typeId = MSG_USER_CONTROL; m.streamId = 0; m.timestamp = 0;
        m.payload.resize(6);
        m.payload[0] = 0; m.payload[1] = 0;
        m.payload[2] = (uint8_t)((streamId >> 24) & 0xFF);
        m.payload[3] = (uint8_t)((streamId >> 16) & 0xFF);
        m.payload[4] = (uint8_t)((streamId >> 8) & 0xFF);
        m.payload[5] = (uint8_t)(streamId & 0xFF);
        return SendOneMessage(m, 2);
    }
    bool SendOnStatusPlay(double txid) {
        std::vector<uint8_t> p;
        AmfEncodeString(p, "onStatus");
        AmfEncodeNumber(p, txid);
        AmfEncodeNull(p);
        AmfObject o;
        o.emplace_back("level",       AmfValue::Str("status"));
        o.emplace_back("code",        AmfValue::Str("NetStream.Play.Start"));
        o.emplace_back("description", AmfValue::Str("Start playing"));
        AmfEncodeObject(p, o);
        Message m; m.typeId = MSG_AMF0_CMD; m.streamId = m_streamId; m.timestamp = 0;
        m.payload = std::move(p);
        return SendOneMessage(m, 5);
    }
    bool SendResultConnect(double txid) {
        std::vector<uint8_t> p;
        AmfEncodeString(p, "_result");
        AmfEncodeNumber(p, txid);
        AmfObject props;
        props.emplace_back("fmsVer",       AmfValue::Str("FMS/3,0,1,123"));
        props.emplace_back("capabilities", AmfValue::Number(31));
        AmfEncodeObject(p, props);
        AmfObject info;
        info.emplace_back("level",       AmfValue::Str("status"));
        info.emplace_back("code",        AmfValue::Str("NetConnection.Connect.Success"));
        info.emplace_back("description", AmfValue::Str("Connection succeeded."));
        info.emplace_back("objectEncoding", AmfValue::Number(0));
        AmfEncodeObject(p, info);
        Message m; m.typeId = MSG_AMF0_CMD; m.streamId = 0; m.timestamp = 0;
        m.payload = std::move(p);
        return SendOneMessage(m, 3);
    }
    bool SendResultCreateStream(double txid, uint32_t newStreamId) {
        std::vector<uint8_t> p;
        AmfEncodeString(p, "_result");
        AmfEncodeNumber(p, txid);
        AmfEncodeNull(p);
        AmfEncodeNumber(p, (double)newStreamId);
        Message m; m.typeId = MSG_AMF0_CMD; m.streamId = 0; m.timestamp = 0;
        m.payload = std::move(p);
        return SendOneMessage(m, 3);
    }

    void HandleAmfCmd(const Message& m) {
        std::vector<AmfValue> args = AmfDecodeAll(m.payload.data(), m.payload.size());
        if (args.size() < 2 || args[0].type != AmfValue::T_STRING) return;
        const std::string& cmd = args[0].str;
        double txid = (args[1].type == AmfValue::T_NUMBER) ? args[1].num : 0;

        if (cmd == "connect") {
            DLOG("client connect txid=%g", txid);
            SendWindowAck(2500000);
            SendSetPeerBw(2500000);
            SendSetChunkSize(4096);
            SendResultConnect(txid);
        } else if (cmd == "createStream") {
            m_streamId = 1;
            SendResultCreateStream(txid, m_streamId);
        } else if (cmd == "play") {
            DLOG("client play");
            SendUserCtrlStreamBegin(m_streamId);
            SendOnStatusPlay(txid);
            // Arm the keyframe gate BEFORE we go playing. Any live
            // PushVideoFrame that races us will be dropped until the
            // encoder delivers a fresh IDR.
            ArmWaitForKeyframe();
            m_owner->PushCachedToClient(this);
            m_playing = true;
            // Tell the encoder to emit a fresh IDR for this new client. The
            // gate above guarantees we only start streaming from THAT IDR
            // forward — no historical GOP, no stale frames.
            m_owner->RequestKeyframe();
        } else if (cmd == "publish") {
            DLOG("publish from network (ignored - server only handles play)");
        } else if (cmd == "deleteStream" || cmd == "closeStream" || cmd == "FCUnpublish") {
            DLOG("client cmd: %s", cmd.c_str());
        } else {
            DLOG("unhandled cmd: %s", cmd.c_str());
        }
    }

    void Run() {
        m_alive = true;
        SimpleIO io(this);
        if (!ServerHandshake(io)) {
            DLOG("session: handshake failed");
            Close();
            return;
        }

        ChunkDecoder dec;
        std::vector<uint8_t> tmp(4096);

        while (!m_quit) {
            int r = ::recv(m_sock, (char*)tmp.data(), (int)tmp.size(), 0);
            if (r <= 0) break;
            if (!dec.Feed(tmp.data(), (size_t)r)) {
                DLOG("session: decode error");
                break;
            }
            Message msg;
            while (dec.Pop(msg)) {
                if (msg.typeId == MSG_AMF0_CMD) {
                    HandleAmfCmd(msg);
                } else if (msg.typeId == MSG_USER_CONTROL ||
                           msg.typeId == MSG_WINDOW_ACK_SIZE ||
                           msg.typeId == MSG_ACK ||
                           msg.typeId == MSG_SET_PEER_BW ||
                           msg.typeId == MSG_SET_CHUNK_SIZE) {
                    // ignore (chunk size handled inline by decoder)
                }
            }
        }
        DLOG("session: exit");
        Close();
    }

    SOCKET m_sock;
    RtmpServer* m_owner;
    std::thread m_thread;
    std::thread m_senderThread;
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_alive{false};
    std::atomic<bool> m_playing{false};
    uint32_t m_streamId = 0;
    uint32_t m_outChunkSize = 128;
    std::mutex m_writeMtx;
    bool m_tsBaseSet = false;
    uint32_t m_tsBase = 0;
    // True from "play" until the next keyframe is delivered. While set,
    // SendVideo drops P-frames and SendAudio drops audio so the client
    // starts decoding from a fresh IDR with zero historical lag.
    std::atomic<bool> m_waitingForKey{false};

    // Asynchronous send queue: encoder thread enqueues packets, dedicated
    // SenderLoop thread drains them via WriteAll. Decouples encoder pacing
    // from socket throughput so a slow client cannot stall encoding.
    struct OutPacket {
        std::vector<uint8_t> buf;
        bool isVideo = false;
        bool isKey = false;
    };
    std::mutex m_qMtx;
    std::condition_variable m_qCv;
    std::deque<OutPacket> m_q;
    size_t m_qBytes = 0;
    uint64_t m_dropEvents = 0;
};

// ---------------------------------------------------------------------------

RtmpServer::RtmpServer() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
}
RtmpServer::~RtmpServer() {
    Stop();
    WSACleanup();
}

bool RtmpServer::Start(uint16_t port) {
    if (m_running) return true;
    m_port = port;

    m_listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) {
        Log("create socket failed");
        return false;
    }
    BOOL yes = TRUE;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        int e = WSAGetLastError();
        Log("bind " + std::to_string(port) + " failed: " + std::to_string(e));
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }
    if (::listen(m_listenSock, 8) != 0) {
        Log("listen failed");
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread([this] { AcceptLoop(); });
    Log("rtmp server listening on 0.0.0.0:" + std::to_string(port));
    return true;
}

void RtmpServer::Stop() {
    m_running = false;
    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();

    std::vector<std::shared_ptr<ClientSession>> tmp;
    {
        std::lock_guard<std::mutex> lk(m_clientsMtx);
        tmp.swap(m_clients);
    }
    for (auto& c : tmp) c->Stop();
    NotifyClientCount();
}

void RtmpServer::AcceptLoop() {
    while (m_running) {
        sockaddr_in caddr{}; int clen = sizeof(caddr);
        SOCKET cs = ::accept(m_listenSock, (sockaddr*)&caddr, &clen);
        if (cs == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }
        BOOL nodelay = TRUE;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        // Cap the OS send buffer so backlog cannot accumulate inside the
        // kernel. Without this, Windows TCP auto-tuning can grow SO_SNDBUF
        // to multiple MB, which at 6 Mbps maps to several seconds of
        // hidden delay before our queue-level drop policy ever sees the
        // congestion. 64 KB is small enough to push back fast, large
        // enough to absorb a single chunk-write burst.
        int sndBuf = 64 * 1024;
        setsockopt(cs, SOL_SOCKET, SO_SNDBUF, (const char*)&sndBuf, sizeof(sndBuf));
        // If a send blocks more than this, the client is hopelessly slow —
        // drop them so we don't stall the sender thread indefinitely.
        DWORD tmo = 1000;
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

        char ip[64] = {0};
        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        Log(std::string("accept ") + ip + ":" + std::to_string(ntohs(caddr.sin_port)));

        auto sess = std::make_shared<ClientSession>(cs, this);
        {
            std::lock_guard<std::mutex> lk(m_clientsMtx);
            m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
                [](const std::shared_ptr<ClientSession>& c){ return !c->Alive(); }),
                m_clients.end());
            m_clients.push_back(sess);
        }
        sess->Start();
        NotifyClientCount();
    }
}

void RtmpServer::SetVideoSequenceHeader(std::vector<uint8_t> tag) {
    std::lock_guard<std::mutex> lk(m_streamMtx);
    m_videoSeqHdr = std::move(tag);
}
void RtmpServer::SetAudioSequenceHeader(std::vector<uint8_t> tag) {
    std::lock_guard<std::mutex> lk(m_streamMtx);
    m_audioSeqHdr = std::move(tag);
}
void RtmpServer::SetMetaData(std::vector<uint8_t> p) {
    std::lock_guard<std::mutex> lk(m_streamMtx);
    m_metaData = std::move(p);
}

void RtmpServer::PushVideoFrame(std::vector<uint8_t> tag, bool keyframe, uint32_t ts) {
    {
        std::lock_guard<std::mutex> lk(m_streamMtx);
        if (keyframe) m_gopCache.clear();
        StreamFrame f;
        f.isVideo = true;
        f.isKey = keyframe;
        f.tag = tag;
        f.timestampMs = ts;
        m_gopCache.push_back(std::move(f));
        // Trim cache, but always keep at least one keyframe at the front. If
        // the encoder rarely emits keyframes (some SW MFTs only emit one IDR
        // at the very start), this keeps the IDR around for late joiners.
        while (m_gopCache.size() > 600) {
            // Don't pop a keyframe if there isn't another one later in cache.
            bool hasLaterKey = false;
            if (m_gopCache.front().isVideo && m_gopCache.front().isKey) {
                for (size_t i = 1; i < m_gopCache.size(); ++i) {
                    if (m_gopCache[i].isVideo && m_gopCache[i].isKey) { hasLaterKey = true; break; }
                }
                if (!hasLaterKey) break;  // keep this lone IDR forever
            }
            m_gopCache.pop_front();
        }
    }
    std::vector<std::shared_ptr<ClientSession>> snap;
    {
        std::lock_guard<std::mutex> lk(m_clientsMtx);
        snap = m_clients;
    }
    int sent = 0;
    for (auto& c : snap) {
        if (!c->Alive() || !c->IsPlaying()) continue;
        c->SendVideo(tag, keyframe, ts);
        ++sent;
    }
    static int total = 0;
    ++total;
    if (total <= 8 || total % 60 == 0 || keyframe) {
        DLOG("pushVideo #%d key=%d ts=%u size=%zu sent=%d/%zu",
             total, (int)keyframe, ts, tag.size(), sent, snap.size());
    }
}

void RtmpServer::PushAudioFrame(std::vector<uint8_t> tag, uint32_t ts) {
    std::vector<std::shared_ptr<ClientSession>> snap;
    {
        std::lock_guard<std::mutex> lk(m_clientsMtx);
        snap = m_clients;
    }
    int sent = 0;
    for (auto& c : snap) {
        if (!c->Alive() || !c->IsPlaying()) continue;
        c->SendAudio(tag, ts);
        ++sent;
    }
    static int total = 0;
    if (++total <= 8 || total % 60 == 0) {
        DLOG("pushAudio #%d ts=%u size=%zu sent=%d/%zu",
             total, ts, tag.size(), sent, snap.size());
    }
}

void RtmpServer::ResetStream() {
    std::vector<std::shared_ptr<ClientSession>> tmp;
    {
        std::lock_guard<std::mutex> lk(m_clientsMtx);
        tmp.swap(m_clients);
    }
    for (auto& c : tmp) c->Stop();

    std::lock_guard<std::mutex> lk(m_streamMtx);
    m_videoSeqHdr.clear();
    m_audioSeqHdr.clear();
    m_metaData.clear();
    m_gopCache.clear();
    NotifyClientCount();
}

int RtmpServer::ClientCount() {
    std::lock_guard<std::mutex> lk(m_clientsMtx);
    int n = 0;
    for (auto& c : m_clients) if (c->Alive() && c->IsPlaying()) ++n;
    return n;
}

void RtmpServer::NotifyClientCount() {
    if (m_onClientCountChanged) m_onClientCountChanged(ClientCount());
}

void RtmpServer::Log(const std::string& s) {
    DLOG("[srv] %s", s.c_str());
    if (m_log) m_log(s);
}

void RtmpServer::PushCachedToClient(ClientSession* c) {
    // Send only the bookkeeping the player needs to bootstrap a decoder:
    // onMetaData + AVC/AAC sequence headers. Do NOT ship the cached GOP —
    // those frames are seconds old and would force the player to start
    // decoding from a stale IDR, locking it into multi-second buffer lag.
    //
    // Instead, the caller (play handler) arms the per-client keyframe gate
    // and asks the encoder for a fresh IDR; SendVideo drops everything
    // until that IDR arrives, so the client locks onto live with sub-GOP
    // delay and zero historical backlog.
    std::vector<uint8_t> meta, vSeq, aSeq;
    {
        std::lock_guard<std::mutex> lk(m_streamMtx);
        meta = m_metaData;
        vSeq = m_videoSeqHdr;
        aSeq = m_audioSeqHdr;
    }
    if (!meta.empty()) c->SendMetaData(meta);
    if (!vSeq.empty()) c->SendVideoUnconditional(vSeq, 0);
    if (!aSeq.empty()) c->SendAudioUnconditional(aSeq, 0);
}

std::string RtmpServer::DiscoverLocalIPv4() {
    ULONG bufLen = 16 * 1024;
    std::vector<uint8_t> buf(bufLen);
    DWORD rc = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        rc = GetAdaptersAddresses(AF_INET, 0, nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &bufLen);
    }
    if (rc != NO_ERROR) return "127.0.0.1";

    auto* p = (PIP_ADAPTER_ADDRESSES)buf.data();
    std::string fallback = "127.0.0.1";
    for (; p; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp) continue;
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
            sockaddr_in* sa = (sockaddr_in*)ua->Address.lpSockaddr;
            if (!sa || sa->sin_family != AF_INET) continue;
            char ip[64] = {0};
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            std::string s(ip);
            if (s.rfind("169.254.", 0) == 0) continue;  // APIPA
            return s;
        }
    }
    return fallback;
}

} // namespace rtmp
