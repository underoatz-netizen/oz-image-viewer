// oz/runner/utf.hpp - UTF-8 <-> UTF-16 conversion helpers (Windows only).
#pragma once

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace oz::runner {

inline std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace oz::runner