#include "framescope/hud_core.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace framescope {

bool ValidSession(const std::wstring& session) {
    return session.size() == 32 && std::all_of(session.begin(), session.end(), [](wchar_t character) {
        return (character >= L'0' && character <= L'9') ||
               (character >= L'a' && character <= L'f') ||
               (character >= L'A' && character <= L'F');
    });
}

std::wstring SessionFromEnvironment() {
    wchar_t value[40]{};
    const DWORD length = GetEnvironmentVariableW(L"FRAMESCOPE_SESSION", value, 40);
    if (length == 0 || length >= 40) {
        return {};
    }
    std::wstring result(value, length);
    return ValidSession(result) ? result : std::wstring{};
}

std::uint64_t QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

Session::Session() : statistics_(QpcFrequency()) {}

Session::~Session() {
    if (view_) {
        UnmapViewOfFile(view_);
    }
    if (mapping_) {
        CloseHandle(static_cast<HANDLE>(mapping_));
    }
}

bool Session::ConnectFromEnvironment() {
    if (view_) {
        return true;
    }
    const auto session = SessionFromEnvironment();
    if (session.empty()) {
        return false;
    }
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, MappingName(session).c_str());
    if (!mapping) {
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingSize);
    if (!view) {
        CloseHandle(mapping);
        return false;
    }
    const auto* header = static_cast<const std::uint32_t*>(view);
    if (header[0] != kMagic || header[1] != kProtocolVersion || header[2] != kMappingSize) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }
    mapping_ = mapping;
    view_ = view;
    return true;
}

bool Session::ReadControl(ControlData& result) const {
    if (!view_) {
        return false;
    }
    auto* base = static_cast<std::uint8_t*>(view_);
    auto* sequence = reinterpret_cast<volatile LONG64*>(base + kControlSequenceOffset);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const LONG64 before = InterlockedCompareExchange64(sequence, 0, 0);
        if (before & 1) {
            continue;
        }
        std::memcpy(&result, base + kControlOffset, sizeof(result));
        MemoryBarrier();
        if (before == InterlockedCompareExchange64(sequence, 0, 0)) {
            const auto now = QpcNow();
            if (result.updated_qpc == 0 || result.updated_qpc > now ||
                now - result.updated_qpc > QpcFrequency() * 5) {
                // Закрытый/зависший контроллер не оставляет бесконечно живой HUD.
                result.flags &= ~Enabled;
            }
            return true;
        }
    }
    return false;
}

FrameData Session::Snapshot() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

bool Session::ReadIntel(IntelData& result) const {
    if (!view_) {
        return false;
    }
    auto* base = static_cast<std::uint8_t*>(view_);
    auto* sequence = reinterpret_cast<volatile LONG64*>(base + kIntelSequenceOffset);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const LONG64 before = InterlockedCompareExchange64(sequence, 0, 0);
        if (before & 1) {
            continue;
        }
        std::memcpy(&result, base + kIntelOffset, sizeof(result));
        MemoryBarrier();
        if (before == InterlockedCompareExchange64(sequence, 0, 0)) {
            const auto now = QpcNow();
            return result.magic == kIntelMagic && result.version == 1 &&
                   result.updated_qpc != 0 && result.updated_qpc <= now &&
                   now - result.updated_qpc <= QpcFrequency() * 5;
        }
    }
    return false;
}

void Session::ObservePresent(std::uint64_t timestamp, Api api, std::uint32_t width,
                             std::uint32_t height, double hud_cpu_us) {
    std::lock_guard lock(mutex_);
    statistics_.Observe(timestamp);
    latest_ = statistics_.Snapshot();
    latest_.api = static_cast<std::uint32_t>(api);
    latest_.width = width;
    latest_.height = height;
    latest_.process_id = GetCurrentProcessId();
    latest_.hud_cpu_us = hud_cpu_us;
    if (!view_) {
        return;
    }
    auto* base = static_cast<std::uint8_t*>(view_);
    auto* sequence = reinterpret_cast<volatile LONG64*>(base + kFrameSequenceOffset);
    InterlockedIncrement64(sequence);
    std::memcpy(base + kFrameOffset, &latest_, sizeof(latest_));
    MemoryBarrier();
    InterlockedIncrement64(sequence);
}

} // namespace framescope
