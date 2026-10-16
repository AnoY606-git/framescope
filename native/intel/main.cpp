// SPDX-License-Identifier: GPL-3.0-only
#include "sysman.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

namespace {
HANDLE stopEvent = nullptr;
HANDLE mainThread = nullptr;

BOOL WINAPI onConsoleControl(DWORD control) {
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT || control == CTRL_CLOSE_EVENT ||
        control == CTRL_SHUTDOWN_EVENT || control == CTRL_LOGOFF_EVENT) {
        if (stopEvent) SetEvent(stopEvent);
        if (mainThread) CancelSynchronousIo(mainThread);
        return TRUE;
    }
    return FALSE;
}

bool writeLine(std::string text) {
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return false;
    text.push_back('\n');
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return false;
    DWORD consoleMode = 0;
    if (GetConsoleMode(output, &consoleMode)) {
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0) return false;
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
        DWORD written = 0;
        return WriteConsoleW(output, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr) &&
               written == wide.size();
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return false;
        DWORD written = 0;
        const DWORD length = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 65536));
        if (!WriteFile(output, text.data() + offset, length, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

std::string timestamp() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char result[40]{};
    std::snprintf(result, sizeof(result), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear,
        utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    return result;
}

struct Options {
    bool watch = false;
    bool legacy = false;
    bool help = false;
    std::uint32_t intervalMs = 1000;
    std::string error;
};

Options parse(int argc, char** argv) {
    Options result;
    bool modeSeen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--once" || argument == "--watch") {
            if (modeSeen) { result.error = "choose_once_or_watch"; return result; }
            modeSeen = true;
            result.watch = argument == "--watch";
        } else if (argument == "--legacy") result.legacy = true;
        else if (argument == "--help" || argument == "-h") result.help = true;
        else if (argument == "--interval-ms") {
            if (++i == argc) { result.error = "interval_value_required"; return result; }
            const std::string_view value(argv[i]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.intervalMs);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                result.intervalMs < 500 || result.intervalMs > 5000) {
                result.error = "interval_must_be_integer_500_to_5000"; return result;
            }
        } else { result.error = "unknown_argument"; return result; }
    }
    return result;
}

int run(int argc, char** argv) {
    using namespace framescope::intel;
    const auto options = parse(argc, argv);
    if (options.help && options.error.empty()) {
        writeLine("FrameScope.Intel.exe [--once | --watch] [--interval-ms 500..5000] [--legacy]\n"
                  "Read-only Intel Level Zero Sysman. UTF-8 JSON Lines; no services/admin/drivers installed.\n"
                  "--once is the default; first output follows two samples. --legacy uses a fresh process\n"
                  "and process-local ZES_ENABLE_SYSMAN=1. Unavailable metrics have null values.");
        return 0;
    }
    if (!options.error.empty()) {
        Snapshot failure;
        failure.timestamp = timestamp();
        failure.backend = {"error", options.error, {}};
        writeLine(serialize(failure));
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) {
        Snapshot failure;
        failure.timestamp = timestamp();
        failure.backend = {"error", "create_stop_event_failed", {}};
        writeLine(serialize(failure));
        return 4;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainThread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    SetConsoleCtrlHandler(nullptr, FALSE);
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
    int exitCode = 0;
    try {
        SysmanBackend backend(options.legacy);
        auto nextSample = GetTickCount64() + options.intervalMs;
        backend.sample(); // baseline, без фиктивного нулевого power/utilization.
        std::uint64_t sequence = 0;
        do {
            const auto now = GetTickCount64();
            const DWORD delay = now < nextSample ? static_cast<DWORD>(nextSample - now) : 0;
            if (WaitForSingleObject(stopEvent, delay) != WAIT_TIMEOUT) break;
            auto snapshot = backend.sample();
            snapshot.timestamp = timestamp();
            snapshot.monotonicMs = GetTickCount64();
            snapshot.sequence = ++sequence;
            snapshot.intervalMs = options.intervalMs;
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
            if (!writeLine(serialize(snapshot))) break; // закрытый parent pipe — штатное завершение.
            if (!options.watch && snapshot.backend.status != "available") exitCode = 3;
            nextSample += options.intervalMs;
            const auto afterSample = GetTickCount64();
            // При медленном vendor call не устраиваем busy-loop «догоняющих» samples.
            if (nextSample <= afterSample) nextSample = afterSample + options.intervalMs;
        } while (options.watch);
    } catch (const std::exception&) {
        Snapshot failure;
        failure.timestamp = timestamp();
        failure.backend = {"error", "native_backend_exception", {}};
        writeLine(serialize(failure));
        exitCode = 4;
    }
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    if (mainThread) CloseHandle(mainThread);
    mainThread = nullptr;
    CloseHandle(stopEvent);
    stopEvent = nullptr;
    return exitCode;
}
} // namespace

int main(int argc, char** argv) { return run(argc, argv); }
