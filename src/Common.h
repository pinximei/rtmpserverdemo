#pragma once

#include <Windows.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

#include "DebugLog.h"

#define HR_RETURN(expr) do { HRESULT _hr_ret = (expr); if (FAILED(_hr_ret)) { DLOG("HR FAIL %s -> 0x%08X (%s:%d)", #expr, (unsigned)_hr_ret, __FILE__, __LINE__); return _hr_ret; } } while(0)
#define HR_LOG(expr)    do { HRESULT _hr_log = (expr); if (FAILED(_hr_log)) { DLOG("HR FAIL %s -> 0x%08X (%s:%d)", #expr, (unsigned)_hr_log, __FILE__, __LINE__); } } while(0)

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
