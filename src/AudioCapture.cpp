#include "AudioCapture.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <Avrt.h>
#include <chrono>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

static const CLSID CLSID_MMDeviceEnumerator_ = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator_    = __uuidof(IMMDeviceEnumerator);
static const IID IID_IAudioClient_           = __uuidof(IAudioClient);
static const IID IID_IAudioCaptureClient_    = __uuidof(IAudioCaptureClient);

AudioCapture::AudioCapture() {}
AudioCapture::~AudioCapture() { Stop(); }

HRESULT AudioCapture::Init() {
    ComPtr<IMMDeviceEnumerator> enumr;
    HR_RETURN(CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                               IID_IMMDeviceEnumerator_, (void**)&enumr));
    HR_RETURN(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &m_device));
    HR_RETURN(m_device->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)&m_audioClient));

    WAVEFORMATEX* mix = nullptr;
    HR_RETURN(m_audioClient->GetMixFormat(&mix));
    if (!mix) return E_FAIL;

    m_sampleRate = (int)mix->nSamplesPerSec;
    m_channels   = (int)mix->nChannels;
    m_bitsPerSample = (int)mix->wBitsPerSample;
    m_blockAlign = (int)mix->nBlockAlign;
    m_isFloat = false;
    m_validBits = m_bitsPerSample;

    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) m_isFloat = true;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mix;
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) m_isFloat = true;
        m_validBits = ext->Samples.wValidBitsPerSample;
    }
    DLOG("audio mix: %d Hz, %d ch, %d bits (%s)",
         m_sampleRate, m_channels, m_bitsPerSample, m_isFloat ? "float" : "int");

    REFERENCE_TIME bufDur = 100 * 10000; // 100 ms
    HRESULT hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufDur, 0, mix, nullptr);
    if (FAILED(hr)) {
        // Some drivers reject EVENTCALLBACK + loopback together; fall back to polling.
        DLOG("audio Initialize EVENTCALLBACK fail 0x%08X, retry polling", (unsigned)hr);
        hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufDur, 0, mix, nullptr);
    }
    CoTaskMemFree(mix);
    HR_RETURN(hr);

    // Try set event handle — if Init was called without EVENTCALLBACK we'll skip.
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(m_audioClient->SetEventHandle(m_event))) {
        // Polling mode: drop the event; CaptureLoop will use sleep.
        CloseHandle(m_event);
        m_event = nullptr;
    }

    HR_RETURN(m_audioClient->GetService(IID_IAudioCaptureClient_, (void**)&m_captureClient));
    return S_OK;
}

HRESULT AudioCapture::Start() {
    if (m_running) return S_OK;
    if (!m_audioClient || !m_captureClient) return E_FAIL;

    m_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HR_RETURN(m_audioClient->Start());
    m_startHns = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() * 10;
    m_running = true;
    m_thread = std::thread([this] { CaptureLoop(); });
    DLOG("audio capture started");
    return S_OK;
}

void AudioCapture::Stop() {
    if (m_quitEvent) SetEvent(m_quitEvent);
    m_running = false;
    if (m_audioClient) m_audioClient->Stop();
    if (m_thread.joinable()) m_thread.join();
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    if (m_quitEvent) { CloseHandle(m_quitEvent); m_quitEvent = nullptr; }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_queue.clear();
    }
}

void AudioCapture::CaptureLoop() {
    DWORD taskIndex = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // WASAPI loopback in silent periods doesn't emit any packets at all.
    // That stalls the encoder and creates a wall-clock gap between video PTS
    // (which keeps incrementing) and audio PTS (which is frozen at 0). The
    // result for ffplay is a steadily growing av-sync drift that looks like
    // ever-increasing latency. Fix: synthesize zero-PCM frames on a wall-clock
    // schedule so the audio timeline always advances with real time.
    const int kSilenceFramesPerPush = 1024;       // ~21 ms @ 48 kHz, matches AAC frame size
    const auto kSilencePeriod = std::chrono::milliseconds(20);
    auto lastPush = std::chrono::steady_clock::now();

    while (m_running) {
        if (m_event) {
            HANDLE waits[2] = { m_event, m_quitEvent };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, 20);
            if (w == WAIT_OBJECT_0 + 1) break;
            // WAIT_TIMEOUT falls through to silence-fill check below.
        } else {
            if (WaitForSingleObject(m_quitEvent, 5) == WAIT_OBJECT_0) break;
        }

        UINT32 packet = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packet))) packet = 0;
        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            HRESULT hr = m_captureClient->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
            if (FAILED(hr)) break;

            AudioBuffer buf;
            buf.frames = (int)frames;
            buf.samples.resize((size_t)frames * m_channels);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(buf.samples.begin(), buf.samples.end(), 0.0f);
            } else if (m_isFloat && m_bitsPerSample == 32) {
                memcpy(buf.samples.data(), data, frames * m_blockAlign);
            } else if (!m_isFloat && m_bitsPerSample == 16) {
                const int16_t* p = (const int16_t*)data;
                size_t total = (size_t)frames * m_channels;
                for (size_t i = 0; i < total; ++i) buf.samples[i] = (float)p[i] / 32768.0f;
            } else if (!m_isFloat && m_bitsPerSample == 32) {
                const int32_t* p = (const int32_t*)data;
                size_t total = (size_t)frames * m_channels;
                for (size_t i = 0; i < total; ++i) buf.samples[i] = (float)((double)p[i] / 2147483648.0);
            } else {
                std::fill(buf.samples.begin(), buf.samples.end(), 0.0f);
            }

            buf.timestampHns = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() * 10 - m_startHns;

            m_captureClient->ReleaseBuffer(frames);

            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_queue.push_back(std::move(buf));
                if (m_queue.size() > 256) m_queue.pop_front();  // safety
            }
            m_cv.notify_one();
            lastPush = std::chrono::steady_clock::now();

            if (FAILED(m_captureClient->GetNextPacketSize(&packet))) break;
        }

        // Silence fill: if WASAPI hasn't produced anything within one AAC
        // frame's worth of wall time, emit a zero-PCM buffer. This keeps the
        // audio timeline advancing with real time so it never falls behind
        // video PTS. We push at most one buffer per loop iteration to avoid
        // bursting after a long quiet stretch.
        auto now = std::chrono::steady_clock::now();
        if (now - lastPush >= kSilencePeriod) {
            AudioBuffer buf;
            buf.frames = kSilenceFramesPerPush;
            buf.samples.assign((size_t)kSilenceFramesPerPush * m_channels, 0.0f);
            buf.timestampHns = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count() * 10 - m_startHns;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_queue.push_back(std::move(buf));
                if (m_queue.size() > 256) m_queue.pop_front();
            }
            m_cv.notify_one();
            lastPush = now;
        }
    }

    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
}

bool AudioCapture::Read(AudioBuffer& out, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lk(m_mtx);
    if (!m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                       [this]{ return !m_queue.empty() || !m_running; })) {
        return false;
    }
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}
