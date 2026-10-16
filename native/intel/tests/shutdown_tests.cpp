// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
HANDLE reportHandle = nullptr;

void report(const std::string& text) {
    DWORD written = 0;
    const std::string line = text + "\n";
    WriteFile(reportHandle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

BOOL WINAPI ignoreOwnSignal(DWORD) { return TRUE; }

struct Child {
    PROCESS_INFORMATION process{};
    HANDLE readPipe = nullptr;

    Child(const std::wstring& executable, bool legacy) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE writePipe = nullptr;
        check(CreatePipe(&readPipe, &writePipe, &security, 0), "CreatePipe");
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        HANDLE nullDevice = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = nullDevice;
        startup.hStdInput = nullDevice;
        std::wstring command = L"\"" + executable + L"\" --watch --interval-ms 500";
        if (legacy) command += L" --legacy";
        const BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (nullDevice != INVALID_HANDLE_VALUE) CloseHandle(nullDevice);
        if (!started) { CloseHandle(readPipe); readPipe = nullptr; }
        check(started, "CreateProcess");
        CloseHandle(process.hThread);
        process.hThread = nullptr;
    }

    ~Child() {
        if (readPipe) CloseHandle(readPipe);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 1); // Только собственный зависший test child.
            CloseHandle(process.hProcess);
        }
    }

    void firstLine() {
        const auto deadline = GetTickCount64() + 30000;
        std::string line;
        while (GetTickCount64() < deadline) {
            DWORD available = 0;
            check(PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr), "Child closed before first JSON");
            if (available) {
                char value = 0;
                DWORD count = 0;
                check(ReadFile(readPipe, &value, 1, &count, nullptr) && count == 1, "ReadFile");
                if (value == '\n') {
                    check(line.find("\"schemaVersion\":1") != std::string::npos, "Invalid first JSON header");
                    return;
                }
                line.push_back(value);
                check(line.size() <= 1024 * 1024, "Unbounded JSON line");
            } else Sleep(10);
        }
        throw std::runtime_error("First JSON timeout");
    }

    void expectExit() {
        check(WaitForSingleObject(process.hProcess, 7000) == WAIT_OBJECT_0, "Shutdown timeout");
        DWORD code = 0;
        check(GetExitCodeProcess(process.hProcess, &code) && code == 0, "Unexpected exit code");
    }
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) return 2;
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(),
                    &reportHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    bool privateConsole = false;
    int result = 0;
    try {
        const bool legacy = argc == 3 && std::wstring(argv[2]) == L"--legacy";
        {
            Child child(argv[1], legacy);
            child.firstLine();
            CloseHandle(child.readPipe);
            child.readPipe = nullptr;
            child.expectExit();
        }
        report("PASS: parent closed stdout pipe");

        // Сигналы посылаются только в новую test-only console, не в console harness/user.
        FreeConsole();
        check(AllocConsole(), "Cannot allocate isolated console for signal tests");
        privateConsole = true;
        if (const auto window = GetConsoleWindow()) ShowWindow(window, SW_HIDE);
        SetConsoleCtrlHandler(ignoreOwnSignal, TRUE);
        {
            Child child(argv[1], legacy);
            child.firstLine();
            check(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child.process.dwProcessId), "Generate CTRL_BREAK");
            child.expectExit();
        }
        report("PASS: isolated Ctrl+Break");
        {
            Child child(argv[1], legacy);
            Sleep(2000); // stdout не читается: тестируем cancellation заполненного pipe.
            check(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child.process.dwProcessId), "Generate blocked-write CTRL_BREAK");
            child.expectExit();
        }
        report("PASS: Ctrl+Break with undrained stdout pipe");
        {
            Child child(argv[1], legacy);
            child.firstLine();
            // CTRL_C нельзя адресовать одной process group: private console содержит только тесты.
            check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "Generate CTRL_C");
            child.expectExit();
        }
        report("PASS: isolated Ctrl+C");
    } catch (const std::exception& exception) {
        report(std::string("FAIL: ") + exception.what());
        result = 1;
    }
    if (privateConsole) FreeConsole();
    if (reportHandle) CloseHandle(reportHandle);
    return result;
}
