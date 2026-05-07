#include "DebugLog.h"

#include <Windows.h>
#include <stdio.h>
#include <share.h>
#include <mutex>
#include <chrono>
#include <ctime>

static FILE* g_logFile = nullptr;
static std::mutex g_logMtx;

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

void DebugLogInit() {
    std::wstring dir = GetExeDir();
    std::wstring path = dir + L"\\rtmpserver.log";
    g_logFile = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (g_logFile) {
        DebugLogWrite("=== rtmpserver log start ===");
    } else {
        OutputDebugStringW((L"failed to open log " + path).c_str());
    }
}

void DebugLogShutdown() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void DebugLogWrite(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    char line[2200];
    int m = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (m < 0) return;

    if (g_logFile) {
        fwrite(line, 1, (size_t)m, g_logFile);
        fflush(g_logFile);
    }
    OutputDebugStringA(line);
}
