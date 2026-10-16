// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "demo.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace framescope::demo {

inline void CheckHr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << static_cast<unsigned long>(result) << ')';
        throw std::runtime_error(message.str());
    }
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    void Reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

// An absolute System32 path prevents a local Vulkan runtime/compiler DLL from
// being loaded from the executable directory or current working directory.
class SystemLibrary {
public:
    SystemLibrary() = default;
    explicit SystemLibrary(const wchar_t* name) { Load(name); }
    ~SystemLibrary() { if (module_) FreeLibrary(module_); }
    SystemLibrary(const SystemLibrary&) = delete;
    SystemLibrary& operator=(const SystemLibrary&) = delete;
    void Load(const wchar_t* name) {
        if (module_) throw std::logic_error("System DLL already loaded");
        wchar_t directory[MAX_PATH + 1]{};
        const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (!length || length >= MAX_PATH) {
            throw std::runtime_error("GetSystemDirectoryW failed");
        }
        const std::wstring path = std::wstring(directory, length) + L"\\" + name;
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) {
            throw std::runtime_error("Required system DLL unavailable (Win32 " +
                                     std::to_string(GetLastError()) + ")");
        }
    }
    FARPROC Find(const char* name) const noexcept { return GetProcAddress(module_, name); }
private:
    HMODULE module_ = nullptr;
};

} // namespace framescope::demo
