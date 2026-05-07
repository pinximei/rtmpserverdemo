#include "UI.h"

#include <commctrl.h>
#include <CommCtrl.h>
#include <strsafe.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
constexpr UINT WM_APP_SET_URL        = WM_APP + 1;
constexpr UINT WM_APP_SET_STATUS     = WM_APP + 2;
constexpr UINT WM_APP_SET_ENC        = WM_APP + 3;
constexpr UINT WM_APP_SET_CAP_FPS    = WM_APP + 4;
constexpr UINT WM_APP_SET_ENC_FPS    = WM_APP + 5;
constexpr UINT WM_APP_SET_CLIENTS    = WM_APP + 6;
constexpr UINT WM_APP_SET_ACTIVE     = WM_APP + 7;

constexpr UINT ID_BTN_START = 1001;
constexpr UINT ID_BTN_STOP  = 1002;
constexpr UINT ID_BTN_COPY  = 1003;

const wchar_t kClassName[] = L"RtmpDesktopStreamerWnd";
}

static void SetText(HWND h, const std::wstring& s) {
    SetWindowTextW(h, s.c_str());
}

UIWindow::UIWindow() {}
UIWindow::~UIWindow() {}

bool UIWindow::Create(HINSTANCE hi, const Callbacks& cb) {
    m_cb = cb;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    m_hwnd = CreateWindowExW(0, kClassName, L"RTMP 桌面推流",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 320,
        nullptr, nullptr, hi, this);
    if (!m_hwnd) return false;

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    auto mklbl = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND lbl = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, w, h, m_hwnd, nullptr, hi, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
        return lbl;
    };
    auto mkbtn = [&](const wchar_t* text, UINT id, int x, int y, int w, int h) {
        HWND b = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, y, w, h, m_hwnd, (HMENU)(UINT_PTR)id, hi, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
        return b;
    };

    int y = 16;
    mklbl(L"推流地址：", 16, y, 80, 20);
    m_lblUrl = mklbl(L"(等待启动)", 96, y, 380, 20);
    m_btnCopy = mkbtn(L"复制", ID_BTN_COPY, 480, y - 2, 56, 24);
    y += 32;

    mklbl(L"状态：", 16, y, 80, 20);
    m_lblStatus = mklbl(L"未启动", 96, y, 440, 20);
    y += 28;

    mklbl(L"编码器：", 16, y, 80, 20);
    m_lblEncoder = mklbl(L"-", 96, y, 440, 20);
    y += 28;

    mklbl(L"采集帧率：", 16, y, 80, 20);
    m_lblCaptureFps = mklbl(L"0.0 fps", 96, y, 200, 20);
    mklbl(L"编码帧率：", 296, y, 80, 20);
    m_lblEncodeFps = mklbl(L"0.0 fps", 376, y, 160, 20);
    y += 28;

    mklbl(L"播放器连接数：", 16, y, 110, 20);
    m_lblClients = mklbl(L"0", 126, y, 80, 20);
    y += 36;

    m_btnStart = mkbtn(L"开始推流", ID_BTN_START, 16,  y, 120, 32);
    m_btnStop  = mkbtn(L"停止",     ID_BTN_STOP,  150, y, 120, 32);
    EnableWindow(m_btnStop, FALSE);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

LRESULT CALLBACK UIWindow::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    UIWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        auto cs = (CREATESTRUCT*)l;
        self = (UIWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->m_hwnd = h;
    } else {
        self = (UIWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    if (self) return self->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT UIWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        UINT id = LOWORD(w);
        if (id == ID_BTN_START && m_cb.onStart) m_cb.onStart();
        else if (id == ID_BTN_STOP && m_cb.onStop) m_cb.onStop();
        else if (id == ID_BTN_COPY) {
            if (OpenClipboard(h)) {
                EmptyClipboard();
                size_t bytes = (m_url.size() + 1) * sizeof(wchar_t);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (g) {
                    void* p = GlobalLock(g);
                    if (p) {
                        memcpy(p, m_url.c_str(), bytes);
                        GlobalUnlock(g);
                        SetClipboardData(CF_UNICODETEXT, g);
                    }
                }
                CloseClipboard();
            }
        }
        return 0;
    }
    case WM_APP_SET_URL: {
        m_url = *((std::wstring*)l);
        delete (std::wstring*)l;
        SetText(m_lblUrl, m_url);
        return 0;
    }
    case WM_APP_SET_STATUS: {
        auto* s = (std::wstring*)l;
        SetText(m_lblStatus, *s);
        delete s;
        return 0;
    }
    case WM_APP_SET_ENC: {
        auto* s = (std::wstring*)l;
        SetText(m_lblEncoder, *s);
        delete s;
        return 0;
    }
    case WM_APP_SET_CAP_FPS: {
        double v;
        memcpy(&v, &l, sizeof(double) <= sizeof(LPARAM) ? sizeof(double) : sizeof(LPARAM));
        wchar_t b[64];
        StringCchPrintfW(b, 64, L"%.1f fps", v);
        SetText(m_lblCaptureFps, b);
        return 0;
    }
    case WM_APP_SET_ENC_FPS: {
        double v;
        memcpy(&v, &l, sizeof(double) <= sizeof(LPARAM) ? sizeof(double) : sizeof(LPARAM));
        wchar_t b[64];
        StringCchPrintfW(b, 64, L"%.1f fps", v);
        SetText(m_lblEncodeFps, b);
        return 0;
    }
    case WM_APP_SET_CLIENTS: {
        wchar_t b[32];
        StringCchPrintfW(b, 32, L"%d", (int)l);
        SetText(m_lblClients, b);
        return 0;
    }
    case WM_APP_SET_ACTIVE: {
        BOOL on = (BOOL)l;
        EnableWindow(m_btnStart, !on);
        EnableWindow(m_btnStop,  on);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void UIWindow::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
void UIWindow::Quit() { if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0); }

void UIWindow::SetUrl(const std::wstring& url) {
    if (!m_hwnd) return;
    PostMessageW(m_hwnd, WM_APP_SET_URL, 0, (LPARAM)new std::wstring(url));
}
void UIWindow::SetStatus(const std::wstring& s) {
    if (!m_hwnd) return;
    PostMessageW(m_hwnd, WM_APP_SET_STATUS, 0, (LPARAM)new std::wstring(s));
}
void UIWindow::SetEncoderInfo(const std::wstring& s) {
    if (!m_hwnd) return;
    PostMessageW(m_hwnd, WM_APP_SET_ENC, 0, (LPARAM)new std::wstring(s));
}
void UIWindow::SetCaptureFps(double fps) {
    if (!m_hwnd) return;
    LPARAM l = 0;
    memcpy(&l, &fps, sizeof(double) <= sizeof(LPARAM) ? sizeof(double) : sizeof(LPARAM));
    PostMessageW(m_hwnd, WM_APP_SET_CAP_FPS, 0, l);
}
void UIWindow::SetEncodeFps(double fps) {
    if (!m_hwnd) return;
    LPARAM l = 0;
    memcpy(&l, &fps, sizeof(double) <= sizeof(LPARAM) ? sizeof(double) : sizeof(LPARAM));
    PostMessageW(m_hwnd, WM_APP_SET_ENC_FPS, 0, l);
}
void UIWindow::SetClientCount(int n) {
    if (!m_hwnd) return;
    PostMessageW(m_hwnd, WM_APP_SET_CLIENTS, 0, (LPARAM)n);
}
void UIWindow::SetStreamingActive(bool on) {
    if (!m_hwnd) return;
    PostMessageW(m_hwnd, WM_APP_SET_ACTIVE, 0, (LPARAM)(on ? 1 : 0));
}
