#include "Common.h"
#include "DebugLog.h"
#include "UI.h"
#include "DesktopCapture.h"
#include "AudioCapture.h"
#include "VideoEncoder.h"
#include "AudioEncoder.h"
#include "FlvMuxer.h"
#include "RtmpServer.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <mfapi.h>
#include <strsafe.h>
#include <crtdbg.h>
#include <stdlib.h>
#include <signal.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace {
constexpr int kTargetFps = 60;
constexpr int kVideoBitrateBps = 6 * 1024 * 1024;   // 6 Mbps for 1080p60
constexpr int kAudioBitrateBps = 128 * 1024;        // 128 kbps AAC
constexpr int kAudioTargetSampleRate = 48000;       // AAC MFT supports 48k
constexpr int kAudioTargetChannels = 2;
}

class StreamingPipeline {
public:
    StreamingPipeline(rtmp::RtmpServer* srv, UIWindow* ui)
        : m_server(srv), m_ui(ui) {}

    bool Start() {
        DLOG("pipeline.Start enter");
        if (m_running) return true;
        m_running = true;
        m_quit = false;

        DLOG("pipeline: ResetStream");
        m_server->ResetStream();

        DLOG("pipeline: capture.Init");
        if (FAILED(m_capture.Init())) {
            DLOG("pipeline: capture.Init FAIL");
            m_ui->SetStatus(L"桌面采集初始化失败");
            m_running = false; return false;
        }
        m_width = m_capture.Width();
        m_height = m_capture.Height();
        DLOG("pipeline: capture %dx%d", m_width, m_height);

        DLOG("pipeline: audio.Init");
        if (FAILED(m_audio.Init())) {
            DLOG("pipeline: audio.Init FAIL");
            m_ui->SetStatus(L"音频采集初始化失败");
            m_running = false; return false;
        }
        DLOG("pipeline: audio.Init OK");

        // Use audio capture mix format if it matches AAC requirements; else
        // fall back to mono/silent path. Most Windows mixers run at 48 kHz
        // stereo, which is exactly what we need.
        int aRate = m_audio.SampleRate();
        int aCh   = m_audio.Channels();
        bool audioOK = (aRate == 44100 || aRate == 48000) && (aCh == 1 || aCh == 2);
        if (audioOK) {
            DLOG("pipeline: audioEnc.Init %dHz %dch", aRate, aCh);
            if (FAILED(m_audioEnc.Init(aRate, aCh, kAudioBitrateBps))) {
                DLOG("audio encoder init failed; disabling audio");
                audioOK = false;
            }
        } else {
            DLOG("audio mix unsupported (%d Hz, %d ch); disabling audio", aRate, aCh);
        }
        m_audioEnabled = audioOK;
        DLOG("pipeline: audioEnabled=%d", (int)m_audioEnabled);

        DLOG("pipeline: videoEnc.Init %dx%d @ %dfps", m_width, m_height, kTargetFps);
        if (FAILED(m_videoEnc.Init(m_width, m_height, kTargetFps,
                                   kVideoBitrateBps, m_capture.Device()))) {
            DLOG("pipeline: videoEnc.Init FAIL");
            m_ui->SetStatus(L"视频编码器初始化失败");
            m_running = false; return false;
        }
        DLOG("pipeline: videoEnc.Init OK hw=%d", (int)m_videoEnc.IsHardware());

        m_ui->SetEncoderInfo(m_videoEnc.IsHardware() ?
            L"H.264 硬编 (MF/HW MFT) + AAC LC" :
            L"H.264 软编 (MF/SW) + AAC LC");

        // Build initial onMetaData
        DLOG("pipeline: build metadata");
        std::vector<uint8_t> meta = flv::BuildOnMetaData(
            m_width, m_height, kTargetFps,
            kVideoBitrateBps / 1024,
            m_audioEnabled ? (kAudioBitrateBps / 1024) : 0,
            m_audioEnabled ? aRate : 0,
            m_audioEnabled ? aCh : 0);
        m_server->SetMetaData(std::move(meta));

        // Start audio capture
        if (m_audioEnabled) {
            DLOG("pipeline: audio.Start");
            if (FAILED(m_audio.Start())) {
                DLOG("audio start failed; disabling");
                m_audioEnabled = false;
            }
        }

        DLOG("pipeline: launching threads");
        m_t0 = std::chrono::steady_clock::now();
        m_videoThread = std::thread([this]{ VideoLoop(); });
        if (m_audioEnabled) m_audioThread = std::thread([this]{ AudioLoop(); });
        m_statsThread = std::thread([this]{ StatsLoop(); });
        DLOG("pipeline: Start done");

        m_ui->SetStatus(L"推流中");
        m_ui->SetStreamingActive(true);
        return true;
    }

    void Stop() {
        if (!m_running) return;
        m_quit = true;
        if (m_videoThread.joinable()) m_videoThread.join();
        if (m_audioThread.joinable()) m_audioThread.join();
        if (m_statsThread.joinable()) m_statsThread.join();

        m_audio.Stop();
        m_audioEnc.Shutdown();
        m_videoEnc.Shutdown();
        m_capture.Shutdown();
        m_server->ResetStream();

        m_running = false;
        m_ui->SetStatus(L"已停止");
        m_ui->SetStreamingActive(false);
        m_ui->SetCaptureFps(0);
        m_ui->SetEncodeFps(0);
    }

private:
    void VideoLoop() {
        DLOG("VideoLoop enter");
        // Pace at kTargetFps using high-resolution waitable timer.
        HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
            TIMER_ALL_ACCESS);
        if (!hTimer) hTimer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
        DLOG("VideoLoop hTimer=%p", hTimer);

        bool sentSeqHdr = false;
        bool didFirstForceKey = false;
        int captured = 0, encoded = 0;
        auto lastStats = std::chrono::steady_clock::now();

        const int64_t periodHns = 10000000 / kTargetFps;
        int64_t nextHns = 0;

        int acquireFailStreak = 0;
        while (!m_quit) {
            CapturedFrame f;
            HRESULT hr = m_capture.AcquireFrame(f, 5);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                acquireFailStreak = 0;
                continue;
            }
            if (FAILED(hr) && hr != S_FALSE) {
                if (acquireFailStreak++ < 3) {
                    DLOG("acquire fail 0x%08X (streak=%d)", (unsigned)hr, acquireFailStreak);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            acquireFailStreak = 0;

            ++captured;
            int64_t tsHns = f.timestampHns;

            // Force keyframe at very first frame, OR whenever the server
            // signals a new client just connected. New clients suppress all
            // video output until they see this IDR, so it MUST actually be
            // requested on the encoder (the previous code dropped the request).
            bool serverWantsKey = m_server->ConsumeKeyframeRequest();
            bool keyReq = !didFirstForceKey || serverWantsKey;
            didFirstForceKey = true;
            if (serverWantsKey) {
                DLOG("encoder: forcing IDR (new client joined)");
            }
            if (FAILED(m_videoEnc.EncodeFrame(f.nv12.Get(), tsHns, keyReq))) {
                DLOG("encode fail");
                continue;
            }

            EncodedVideoPacket pkt;
            while (m_videoEnc.PullPacket(pkt)) {
                ++encoded;
                if (!sentSeqHdr && pkt.keyframe &&
                    !m_videoEnc.SPS().empty() && !m_videoEnc.PPS().empty()) {
                    auto seq = flv::BuildVideoTagAvcSeqHeader(m_videoEnc.SPS(), m_videoEnc.PPS());
                    m_server->SetVideoSequenceHeader(seq);
                    sentSeqHdr = true;
                    DLOG("video sequence header sent");

                    // Audio sequence header
                    if (m_audioEnabled && !m_audioEnc.AudioSpecificConfig().empty()) {
                        auto aSeq = flv::BuildAudioTagAacSeqHeader(m_audioEnc.AudioSpecificConfig());
                        m_server->SetAudioSequenceHeader(aSeq);
                    }
                }

                auto nalus = flv::SplitAnnexB(pkt.annexB.data(), pkt.annexB.size());
                // Strip SPS/PPS from inline NAL list (they are in seq header)
                std::vector<std::vector<uint8_t>> body;
                body.reserve(nalus.size());
                for (auto& n : nalus) {
                    if (n.empty()) continue;
                    int t = n[0] & 0x1F;
                    if (t == 7 || t == 8) continue; // SPS/PPS
                    body.push_back(std::move(n));
                }
                if (body.empty()) continue;

                auto tag = flv::BuildVideoTagAvcNalu(body, pkt.keyframe);
                // Use a monotonic per-frame timestamp derived from the
                // configured frame rate, NOT the MFT's output sample time.
                // The SW MFT can reorder output timestamps (even with B=0)
                // which breaks player demuxers (DTS-out-of-order errors).
                uint32_t tsMs = (uint32_t)((m_videoOutCount * 1000LL) / kTargetFps);
                ++m_videoOutCount;
                m_server->PushVideoFrame(std::move(tag), pkt.keyframe, tsMs);
            }

            auto now = std::chrono::steady_clock::now();
            auto el = std::chrono::duration<double>(now - lastStats).count();
            if (el >= 1.0) {
                m_ui->SetCaptureFps(captured / el);
                m_ui->SetEncodeFps(encoded / el);
                captured = encoded = 0;
                lastStats = now;
            }

            // Pace next frame
            if (hTimer) {
                int64_t curHns = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - m_t0).count() * 10;
                if (nextHns == 0) nextHns = curHns + periodHns;
                int64_t wait = nextHns - curHns;
                if (wait > 0 && wait < 100 * 10000) {
                    LARGE_INTEGER due;
                    due.QuadPart = -wait;
                    SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(hTimer, 100);
                }
                nextHns += periodHns;
            }
        }

        m_videoEnc.Drain();
        EncodedVideoPacket pkt;
        while (m_videoEnc.PullPacket(pkt)) {
            // discard tail, stream is stopping
        }
        if (hTimer) CloseHandle(hTimer);
    }

    void AudioLoop() {
        DLOG("AudioLoop enter");
        AudioBuffer abuf;
        while (!m_quit) {
            if (!m_audio.Read(abuf, 50)) continue;
            HRESULT hr = m_audioEnc.EncodeFloat(abuf.samples.data(),
                                                abuf.frames, abuf.timestampHns);
            if (FAILED(hr)) {
                DLOG("audio encode fail 0x%08X", (unsigned)hr);
                continue;
            }
            EncodedAudioPacket apkt;
            while (m_audioEnc.PullPacket(apkt)) {
                auto tag = flv::BuildAudioTagAacRaw(apkt.aac.data(), apkt.aac.size());
                // Monotonic per-AAC-frame timestamp. Each AAC frame is 1024
                // samples; rate is the audio capture sample rate (matches
                // input/output: AAC encoder works at the native rate).
                int sr = m_audio.SampleRate();
                if (sr <= 0) sr = 48000;
                uint32_t tsMs = (uint32_t)((m_audioOutCount * 1024LL * 1000LL) / sr);
                ++m_audioOutCount;
                m_server->PushAudioFrame(std::move(tag), tsMs);
            }
        }
    }

    void StatsLoop() {
        while (!m_quit) {
            int n = m_server->ClientCount();
            m_ui->SetClientCount(n);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    rtmp::RtmpServer* m_server;
    UIWindow* m_ui;
    int64_t m_videoOutCount = 0;
    int64_t m_audioOutCount = 0;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_quit{false};

    DesktopCapture m_capture;
    AudioCapture   m_audio;
    VideoEncoder   m_videoEnc;
    AudioEncoder   m_audioEnc;

    std::thread m_videoThread, m_audioThread, m_statsThread;
    std::chrono::steady_clock::time_point m_t0;
    int m_width = 0, m_height = 0;
    bool m_audioEnabled = false;
};

static void OnInvalidParameter(const wchar_t* expr, const wchar_t* func,
                               const wchar_t* file, unsigned line, uintptr_t) {
    char ea[512]={0}, fa[256]={0}, fia[256]={0};
    if (expr) WideCharToMultiByte(CP_UTF8,0,expr,-1,ea,sizeof(ea)-1,nullptr,nullptr);
    if (func) WideCharToMultiByte(CP_UTF8,0,func,-1,fa,sizeof(fa)-1,nullptr,nullptr);
    if (file) WideCharToMultiByte(CP_UTF8,0,file,-1,fia,sizeof(fia)-1,nullptr,nullptr);
    DLOG("CRT INVALID PARAM: expr=%s func=%s file=%s line=%u",
         expr?ea:"(null)", func?fa:"(null)", file?fia:"(null)", line);
    // Don't terminate; allow caller to handle. (For diagnostics only.)
}
static void OnPureCall() { DLOG("CRT PURECALL"); }
static void OnTerminate() {
    DLOG("std::terminate called");
    abort();
}
static void OnAbortSig(int) {
    DLOG("SIGABRT signal");
}

int APIENTRY wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmdLine, int) {
    DebugLogInit();
    DLOG("rtmpserver starting");

    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    std::set_terminate(OnTerminate);
    signal(SIGABRT, OnAbortSig);
    _CrtSetReportMode(_CRT_ASSERT, 0);
    _CrtSetReportMode(_CRT_ERROR, 0);
    _CrtSetReportMode(_CRT_WARN, 0);

    bool autoStart = (cmdLine && wcsstr(cmdLine, L"/auto") != nullptr);

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HR_LOG(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    rtmp::RtmpServer server;
    UIWindow ui;

    StreamingPipeline pipeline(&server, &ui);

    UIWindow::Callbacks cb;
    cb.onStart = [&]{ pipeline.Start(); };
    cb.onStop  = [&]{ pipeline.Stop(); };

    if (!ui.Create(hi, cb)) {
        MessageBoxW(nullptr, L"创建窗口失败", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    server.SetClientCountChanged([&](int n){ ui.SetClientCount(n); });
    server.SetLogFn([](const std::string& s){ DLOG("[server] %s", s.c_str()); });

    if (!server.Start(1935)) {
        ui.SetStatus(L"端口 1935 监听失败 (是否被占用？请尝试以管理员运行)");
    } else {
        std::string ipv4 = rtmp::RtmpServer::DiscoverLocalIPv4();
        std::wstring url = L"rtmp://" + Utf8ToWide(ipv4) + L":1935/live/desktop";
        ui.SetUrl(url);
        ui.SetStatus(L"服务就绪，点击「开始推流」");
    }

    if (autoStart) {
        DLOG("auto-start enabled by /auto cli flag");
        std::thread([&pipeline]{
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            pipeline.Start();
        }).detach();
    }

    ui.Run();

    pipeline.Stop();
    server.Stop();

    MFShutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    DebugLogShutdown();
    return 0;
}
