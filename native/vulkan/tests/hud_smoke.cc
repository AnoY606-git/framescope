// SPDX-License-Identifier: GPL-3.0-only
// Real, separate demo-process integration test. Does not link/inject the renderer
// or modify the demo. Only a private mapping and child environment are supplied.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "framescope/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

bool SetChildEnvironment(std::vector<wchar_t>& block, const std::wstring& name, const std::wstring& value) {
    std::vector<std::wstring> entries;
    wchar_t* inherited = GetEnvironmentStringsW();
    if (!inherited) return false;
    const wchar_t* entry = inherited;
    while (*entry) {
        entries.emplace_back(entry);
        entry += std::wcslen(entry) + 1;
    }
    FreeEnvironmentStringsW(inherited);
    // Reuse the already constructed block on subsequent updates.
    if (!block.empty()) {
        entries.clear();
        entry = block.data();
        while (*entry) { entries.emplace_back(entry); entry += std::wcslen(entry) + 1; }
    }
    const std::wstring prefix = name + L"=";
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::wstring& item) {
        return item.size() >= prefix.size() && _wcsnicmp(item.c_str(), prefix.c_str(), prefix.size()) == 0;
    }), entries.end());
    entries.push_back(prefix + value);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    block.clear();
    for (const auto& item : entries) {
        block.insert(block.end(), item.begin(), item.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return true;
}

void Publish(void* view, framescope::ControlData& control) {
    auto* base = static_cast<unsigned char*>(view);
    auto* sequence = reinterpret_cast<volatile LONG64*>(base + framescope::kControlSequenceOffset);
    control.updated_qpc = framescope::QpcNow();
    InterlockedIncrement64(sequence);
    std::memcpy(base + framescope::kControlOffset, &control, sizeof(control));
    MemoryBarrier();
    InterlockedIncrement64(sequence);
}

bool ReadFrame(void* view, framescope::FrameData& frame) {
    auto* base = static_cast<unsigned char*>(view);
    auto* sequence = reinterpret_cast<volatile LONG64*>(base + framescope::kFrameSequenceOffset);
    const LONG64 before = InterlockedCompareExchange64(sequence, 0, 0);
    if (before & 1) return false;
    std::memcpy(&frame, base + framescope::kFrameOffset, sizeof(frame));
    MemoryBarrier();
    return before == InterlockedCompareExchange64(sequence, 0, 0);
}

struct WindowSearch { DWORD pid; HWND window; };

BOOL CALLBACK FindWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search.pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

std::string ReadPipe(HANDLE pipe) {
    std::string result;
    char buffer[1024];
    DWORD read = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read) result.append(buffer, read);
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool stale_mode = argc == 4 && std::wcscmp(argv[3], L"--stale") == 0;
    if (argc != 3 && !stale_mode) {
        std::fprintf(stderr, "Usage: hud_smoke.exe <absolute FrameScope.Demo.exe> <layer manifest directory> [--stale]\n");
        return 2;
    }
    HMODULE random_library = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Random = BOOLEAN (WINAPI*)(PVOID, ULONG);
    auto random = random_library ? reinterpret_cast<Random>(GetProcAddress(random_library, "SystemFunction036")) : nullptr;
    unsigned char bytes[16]{};
    if (!random || !random(bytes, sizeof(bytes))) return 3;
    FreeLibrary(random_library);
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring session;
    for (unsigned char value : bytes) { session.push_back(digits[value >> 4]); session.push_back(digits[value & 15]); }
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(framescope::kMappingSize), framescope::MappingName(session).c_str());
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) return 4;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, framescope::kMappingSize);
    if (!view) { CloseHandle(mapping); return 5; }
    std::memset(view, 0, framescope::kMappingSize);
    auto* header = static_cast<std::uint32_t*>(view);
    header[0] = framescope::kMagic;
    header[1] = framescope::kProtocolVersion;
    header[2] = framescope::kMappingSize;
    framescope::ControlData control{};
    control.flags = framescope::Enabled | framescope::ShowFps | framescope::ShowFrametime |
                    framescope::ShowCpu | framescope::ShowGpu | framescope::ShowRam |
                    framescope::ShowVram | framescope::ShowLows | framescope::Graph;
    control.scale = 1;
    control.opacity = 0.85f;
    control.margin_x = control.margin_y = 24;
    control.text_color = 0xffedf3f6;
    control.accent_color = 0xff4ce0c8;
    control.background_color = 0xff10151f;
    // available_mask remains zero: the test supplies NO invented sensor values.
    Publish(view, control);
    std::vector<wchar_t> environment;
    if (!SetChildEnvironment(environment, L"FRAMESCOPE_SESSION", session) ||
        !SetChildEnvironment(environment, L"FRAMESCOPE_DEMO_GATE", L"0") ||
        !SetChildEnvironment(environment, L"VK_LAYER_PATH", argv[2]) ||
        !SetChildEnvironment(environment, L"VK_INSTANCE_LAYERS", L"VK_LAYER_FRAMESCOPE_hud") ||
        !SetChildEnvironment(environment, L"VK_LOADER_DEBUG", L"error,warn")) return 6;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &attributes, 8192)) return 7;
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = pipe_write;
    startup.hStdError = pipe_write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" --api vulkan --mode windowed --duration 8 --vsync on";
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
        environment.data(), nullptr, &startup, &process)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 8;
    }
    CloseHandle(process.hThread);
    CloseHandle(pipe_write);
    const uint64_t begin = GetTickCount64();
    uint64_t last_publish = 0;
    bool positive = false, disabled = false, resumed = false, resized = false;
    bool resize_sent = false, minimize_sent = false, restore_sent = false;
    uint32_t initial_width = 0, initial_height = 0;
    uint64_t highest_count = 0;
    DWORD exit_code = STILL_ACTIVE;
    while (WaitForSingleObject(process.hProcess, 16) == WAIT_TIMEOUT) {
        const uint64_t elapsed = GetTickCount64() - begin;
        if (elapsed > 15000) {
            TerminateProcess(process.hProcess, 99); // Only this test's own child, never a game.
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
        const bool pause_heartbeat = stale_mode && elapsed >= 2000 && elapsed < 5500;
        if (!pause_heartbeat && (!last_publish || elapsed - last_publish >= 100)) {
            if (!stale_mode && elapsed >= 2000 && elapsed < 3500) control.flags &= ~framescope::Enabled;
            else control.flags |= framescope::Enabled;
            if (elapsed >= 3500) { control.flags |= framescope::Horizontal; control.scale = 1.25f; }
            Publish(view, control);
            last_publish = elapsed;
        }
        framescope::FrameData frame{};
        if (ReadFrame(view, frame) && frame.process_id == process.dwProcessId &&
            frame.api == static_cast<uint32_t>(framescope::Api::Vulkan)) {
            highest_count = std::max(highest_count, frame.present_count);
            if (frame.present_count > 5 && !initial_width) { initial_width = frame.width; initial_height = frame.height; }
            if (elapsed < 2000 && frame.hud_cpu_us > 0) positive = true;
            const bool disabled_window = stale_mode ? elapsed >= 5100 && elapsed < 5500 :
                                                     elapsed >= 2800 && elapsed < 3500;
            if (disabled_window && frame.hud_cpu_us == 0 && frame.present_count > 5) disabled = true;
            if (elapsed >= 6500 && frame.hud_cpu_us > 0) resumed = true;
            if (initial_width && (frame.width != initial_width || frame.height != initial_height)) resized = true;
        }
        WindowSearch search{process.dwProcessId, nullptr};
        if ((!resize_sent && elapsed >= 4800) || (!minimize_sent && elapsed >= 5700) ||
            (!restore_sent && elapsed >= 6300)) {
            EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&search));
            if (search.window) {
                if (!resize_sent && elapsed >= 4800) { MoveWindow(search.window, 70, 70, 760, 540, TRUE); resize_sent = true; }
                if (!minimize_sent && elapsed >= 5700) { ShowWindowAsync(search.window, SW_MINIMIZE); minimize_sent = true; }
                if (!restore_sent && elapsed >= 6300) { ShowWindowAsync(search.window, SW_RESTORE); restore_sent = true; }
            }
        }
    }
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    const std::string output = ReadPipe(pipe_read);
    CloseHandle(pipe_read);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    std::printf("%s", output.c_str());
    std::printf("Vulkan HUD integration (%s): exit=%lu presents=%llu submitted=%s disabled=%s resumed=%s resized=%s min/restore=%s\n",
        stale_mode ? "stale heartbeat" : "Enabled toggle", exit_code,
        static_cast<unsigned long long>(highest_count), positive ? "yes" : "NO",
        disabled ? "yes" : "NO", resumed ? "yes" : "NO", resized ? "yes" : "NO",
        minimize_sent && restore_sent ? "requested" : "NO");
    // A positive hud_cpu_us is published only after a real successful private
    // QueueSubmit. This is NOT a measurement of display scanout or image quality.
    return exit_code == 0 && highest_count > 30 && positive && disabled && resumed && resized ? 0 : 1;
}
