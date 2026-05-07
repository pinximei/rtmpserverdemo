#pragma once

#include <Windows.h>
#include <string>
#include <functional>

class UIWindow {
public:
    struct Callbacks {
        std::function<void()> onStart;   // user clicked "开始推流"
        std::function<void()> onStop;    // user clicked "停止"
    };

    UIWindow();
    ~UIWindow();

    bool Create(HINSTANCE hi, const Callbacks& cb);
    void Run();         // standard message pump until WM_QUIT
    void Quit();

    // Updates from background threads (post message internally; safe).
    void SetUrl(const std::wstring& url);
    void SetStatus(const std::wstring& s);
    void SetEncoderInfo(const std::wstring& s);
    void SetCaptureFps(double fps);
    void SetEncodeFps(double fps);
    void SetClientCount(int n);
    void SetStreamingActive(bool on);

    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void Layout();

    HWND m_hwnd = nullptr;
    HWND m_btnStart = nullptr;
    HWND m_btnStop = nullptr;
    HWND m_lblUrl = nullptr;
    HWND m_lblStatus = nullptr;
    HWND m_lblEncoder = nullptr;
    HWND m_lblCaptureFps = nullptr;
    HWND m_lblEncodeFps = nullptr;
    HWND m_lblClients = nullptr;
    HWND m_btnCopy = nullptr;
    Callbacks m_cb;
    std::wstring m_url;
};
