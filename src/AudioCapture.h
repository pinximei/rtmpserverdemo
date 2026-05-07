#pragma once

#include "Common.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>

#include <mmdeviceapi.h>
#include <audioclient.h>

// WASAPI loopback capture (system output mix). Output: float32 interleaved.
//
// Lifecycle:
//   Init() opens default render endpoint in loopback mode and queries its mix
//   format. Caller reads SampleRate()/Channels() before starting encoder.
//   Start() spawns worker thread which fills a thread-safe queue with PCM.
//   Read() pulls one buffer (blocking up to timeoutMs).

struct AudioBuffer {
    std::vector<float> samples;  // interleaved (channels-per-frame)
    int frames = 0;
    int64_t timestampHns = 0;    // 100-ns since Start()
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    HRESULT Init();
    HRESULT Start();
    void Stop();

    int SampleRate() const { return m_sampleRate; }
    int Channels()   const { return m_channels; }

    // Returns false on timeout (queue empty). Buffer ownership transfers.
    bool Read(AudioBuffer& out, uint32_t timeoutMs);

private:
    void CaptureLoop();

    ComPtr<IMMDevice>       m_device;
    ComPtr<IAudioClient>    m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;

    HANDLE m_event = nullptr;
    HANDLE m_quitEvent = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<AudioBuffer> m_queue;

    int m_sampleRate = 0;
    int m_channels = 0;
    int m_bitsPerSample = 0;
    int m_validBits = 0;
    int m_blockAlign = 0;
    bool m_isFloat = false;
    int64_t m_startHns = 0;
};
