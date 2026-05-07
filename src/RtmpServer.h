#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>

#include <Windows.h>
#include <winsock2.h>

namespace rtmp {

struct StreamFrame {
    bool isVideo = true;
    bool isKey = false;
    bool isSequenceHeader = false;
    std::vector<uint8_t> tag;
    uint32_t timestampMs = 0;
};

class ClientSession; // defined in RtmpServer.cpp

class RtmpServer {
    friend class ClientSession;

public:
    using ClientCountChanged = std::function<void(int)>;
    using LogFn = std::function<void(const std::string&)>;

    RtmpServer();
    ~RtmpServer();

    bool Start(uint16_t port = 1935);
    void Stop();

    void SetVideoSequenceHeader(std::vector<uint8_t> tag);
    void SetAudioSequenceHeader(std::vector<uint8_t> tag);
    void SetMetaData(std::vector<uint8_t> onMetaDataPayload);

    void PushVideoFrame(std::vector<uint8_t> tag, bool keyframe, uint32_t timestampMs);
    void PushAudioFrame(std::vector<uint8_t> tag, uint32_t timestampMs);

    void ResetStream();

    int  ClientCount();

    void SetClientCountChanged(ClientCountChanged cb) { m_onClientCountChanged = std::move(cb); }
    void SetLogFn(LogFn cb) { m_log = std::move(cb); }

    static std::string DiscoverLocalIPv4();

    // Returns true and clears the flag if a keyframe was requested by a new
    // client. The encoder thread should poll this each iteration.
    bool ConsumeKeyframeRequest() {
        bool exp = true;
        return m_keyReq.compare_exchange_strong(exp, false);
    }

private:
    void AcceptLoop();
    void NotifyClientCount();
    void Log(const std::string& s);

    // Called by ClientSession when "play" is received: ship cached headers + GOP.
    void PushCachedToClient(ClientSession* c);

    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_acceptThread;

    std::mutex m_clientsMtx;
    std::vector<std::shared_ptr<ClientSession>> m_clients;

    std::mutex m_streamMtx;
    std::vector<uint8_t> m_videoSeqHdr;
    std::vector<uint8_t> m_audioSeqHdr;
    std::vector<uint8_t> m_metaData;
    std::deque<StreamFrame> m_gopCache;

    ClientCountChanged m_onClientCountChanged;
    LogFn m_log;

    uint16_t m_port = 1935;
    std::atomic<bool> m_keyReq{false};

public:
    void RequestKeyframe() { m_keyReq = true; }
};

} // namespace rtmp
