// SPDX-License-Identifier: GPL-3.0-only
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "framescope/protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void Reset() {
        if (*this) CloseHandle(value_);
        value_ = nullptr;
    }
private:
    HANDLE value_ = nullptr;
};

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(length), '\0');
    if (length)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            output.data(), length, nullptr, nullptr);
    return output;
}

std::runtime_error WindowsError(const char* operation, DWORD error = GetLastError()) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::string text = std::string(operation) + " (Win32 " + std::to_string(error) + ")";
    if (message) {
        text += ": " + Utf8(message);
        LocalFree(message);
    }
    return std::runtime_error(text);
}

void Log(const std::string& text) {
    std::cerr << "[FrameScope.Launcher] " << text << '\n';
}

struct Options {
    fs::path demo;
    std::wstring api;
    std::wstring session;
    std::wstring mode = L"windowed";
    std::vector<std::pair<std::wstring, std::wstring>> forwarded;
};

void Help() {
    std::cout << "FrameScope private preview: launches ONLY FrameScope.Demo.exe.\n"
        "No arbitrary game/PID injection, administrator rights or anti-cheat bypass.\n"
        "  --demo PATH --api dx11|dx12|vulkan --session 32hex\n"
        "  [--mode windowed|borderless|exclusive] [--duration 0..86400]\n"
        "  [--cpu-load 0..100] [--gpu-load 1..100] [--stutter-ms 0..5000]\n"
        "  [--vsync on|off]\n"
        "The controller must create the session mapping first. Vulkan needs a\n"
        "non-elevated process and an installed Vulkan runtime/driver.\n";
}

void ValidateNumber(const std::wstring& value, const std::wstring& key,
                    double low, double high, bool integral) {
    wchar_t* end = nullptr;
    errno = 0;
    const double number = std::wcstod(value.c_str(), &end);
    if (value.empty() || end != value.c_str() + value.size() || errno == ERANGE ||
        !std::isfinite(number) || number < low || number > high ||
        (integral && std::floor(number) != number))
        throw std::runtime_error("Invalid numeric value for " + Utf8(key));
}

bool Parse(int argc, wchar_t** argv, Options& options) {
    std::set<std::wstring> seen;
    for (int index = 1; index < argc; ++index) {
        const std::wstring key = argv[index];
        if (key == L"--help" || key == L"-h") { Help(); return false; }
        if (!seen.insert(key).second)
            throw std::runtime_error("Duplicate option: " + Utf8(key));
        if (index + 1 >= argc)
            throw std::runtime_error("Missing value for " + Utf8(key));
        const std::wstring value = argv[++index];
        if (key == L"--demo") options.demo = value;
        else if (key == L"--api") options.api = value;
        else if (key == L"--session") options.session = value;
        else if (key == L"--mode") options.mode = value;
        else if (key == L"--duration") {
            ValidateNumber(value, key, 0, 86400, false);
            options.forwarded.emplace_back(key, value);
        } else if (key == L"--cpu-load") {
            ValidateNumber(value, key, 0, 100, true);
            options.forwarded.emplace_back(key, value);
        } else if (key == L"--gpu-load") {
            ValidateNumber(value, key, 1, 100, true);
            options.forwarded.emplace_back(key, value);
        } else if (key == L"--stutter-ms") {
            ValidateNumber(value, key, 0, 5000, true);
            options.forwarded.emplace_back(key, value);
        } else if (key == L"--vsync") {
            if (value != L"on" && value != L"off")
                throw std::runtime_error("--vsync must be on or off");
            options.forwarded.emplace_back(key, value);
        } else {
            throw std::runtime_error("Unknown option: " + Utf8(key));
        }
    }
    if (options.demo.empty() || !framescope::ValidSession(options.session))
        throw std::runtime_error("--demo and a 32-hex-character --session are required");
    if (options.api != L"dx11" && options.api != L"dx12" && options.api != L"vulkan")
        throw std::runtime_error("--api must be dx11, dx12 or vulkan");
    if (options.mode != L"windowed" && options.mode != L"borderless" && options.mode != L"exclusive")
        throw std::runtime_error("--mode must be windowed, borderless or exclusive");
    if (_wcsicmp(options.demo.filename().c_str(), L"FrameScope.Demo.exe") != 0)
        throw std::runtime_error("Preview policy: target basename must be FrameScope.Demo.exe");
    return true;
}

fs::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw WindowsError("GetModuleFileNameW");
    path.resize(length);
    return fs::path(path);
}

fs::path ExistingFile(const fs::path& path) {
    std::error_code error;
    const fs::path canonical = fs::canonical(path, error);
    if (error || !fs::is_regular_file(canonical, error))
        throw std::runtime_error("Required file is missing: " + Utf8(path.native()));
    return canonical;
}

void RequireX64Pe(const fs::path& path, bool dll) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) throw WindowsError("Open native image");
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    DWORD read = 0;
    if (!ReadFile(file.Get(), &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        dos.e_lfanew > 16 * 1024 * 1024)
        throw std::runtime_error("Invalid native PE image: " + Utf8(path.native()));
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file.Get(), offset, nullptr, FILE_BEGIN) ||
        !ReadFile(file.Get(), &nt, sizeof(nt), &read, nullptr) || read != sizeof(nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        bool(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) != dll)
        throw std::runtime_error("Expected a Windows x64 " + std::string(dll ? "DLL: " : "EXE: ") +
                                 Utf8(path.native()));
}

void ValidateMapping(const std::wstring& session) {
    Handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, framescope::MappingName(session).c_str()));
    if (!mapping)
        throw WindowsError("Open UI session mapping (start the controller first)");
    const void* view = MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, framescope::kMappingSize);
    if (!view) throw WindowsError("Map UI session header");
    const auto* header = static_cast<const std::uint32_t*>(view);
    const bool valid = header[0] == framescope::kMagic &&
        header[1] == framescope::kProtocolVersion && header[2] == framescope::kMappingSize;
    UnmapViewOfFile(view);
    if (!valid)
        throw std::runtime_error("UI mapping has an incompatible FrameScope protocol header");
}

struct CaseInsensitive {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};
using Environment = std::map<std::wstring, std::wstring, CaseInsensitive>;

Environment CloneEnvironment() {
    LPWCH block = GetEnvironmentStringsW();
    if (!block) throw WindowsError("GetEnvironmentStringsW");
    Environment result;
    try {
        for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
            const wchar_t* separator = wcschr(entry + (*entry == L'=' ? 1 : 0), L'=');
            if (separator)
                result[std::wstring(entry, separator)] = separator + 1;
        }
    } catch (...) {
        FreeEnvironmentStringsW(block);
        throw;
    }
    FreeEnvironmentStringsW(block);
    return result;
}

void AppendList(Environment& environment, const wchar_t* key, const std::wstring& value) {
    std::wstring& list = environment[key];
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t end = list.find(L';', start);
        const std::wstring item = list.substr(start, end == std::wstring::npos ? end : end - start);
        if (_wcsicmp(item.c_str(), value.c_str()) == 0)
            return;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (!list.empty() && list.back() != L';') list += L';';
    list += value;
}

std::vector<wchar_t> EnvironmentBlock(const Environment& environment) {
    std::vector<wchar_t> block;
    for (const auto& entry : environment) {
        block.insert(block.end(), entry.first.begin(), entry.first.end());
        block.push_back(L'=');
        block.insert(block.end(), entry.second.begin(), entry.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1) block.push_back(L'\0');
    return block;
}

bool Elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        throw WindowsError("Open process token");
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size))
        throw WindowsError("Read process elevation");
    return elevation.TokenIsElevated != 0;
}

// Implements the Windows CommandLineToArgvW/MS C-runtime quoting rules, including
// trailing backslashes before a closing quote. No shell is involved.
std::wstring Quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result += ch;
        } else {
            result.append(slashes, L'\\');
            result += ch;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

Handle NewEvent(const std::wstring& session, const wchar_t* suffix) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, framescope::EventName(session, suffix).c_str()));
    const DWORD error = GetLastError();
    if (!event) throw WindowsError("Create local startup event", error);
    if (error == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Session startup event already exists; choose a fresh random session ID");
    return event;
}

class InheritedStreams {
public:
    InheritedStreams() {
        handles_[0] = DuplicateOrNull(STD_INPUT_HANDLE, GENERIC_READ);
        handles_[1] = DuplicateOrNull(STD_OUTPUT_HANDLE, GENERIC_WRITE);
        handles_[2] = DuplicateOrNull(STD_ERROR_HANDLE, GENERIC_WRITE);
        for (std::size_t index = 0; index < handles_.size(); ++index)
            list_[index] = handles_[index].Get();
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage_.resize(bytes);
        attributes_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(attributes_, 1, 0, &bytes)) {
            attributes_ = nullptr;
            throw WindowsError("Initialize child handle list");
        }
        if (!UpdateProcThreadAttribute(attributes_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       list_.data(), sizeof(list_), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes_);
            attributes_ = nullptr;
            throw WindowsError("Set child standard-handle inheritance");
        }
    }
    ~InheritedStreams() {
        if (attributes_) DeleteProcThreadAttributeList(attributes_);
    }
    void Apply(STARTUPINFOEXW& info) {
        info.StartupInfo.cb = sizeof(info);
        info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        info.StartupInfo.hStdInput = list_[0];
        info.StartupInfo.hStdOutput = list_[1];
        info.StartupInfo.hStdError = list_[2];
        info.lpAttributeList = attributes_;
    }
private:
    static Handle DuplicateOrNull(DWORD kind, DWORD access) {
        HANDLE duplicate = nullptr;
        HANDLE original = GetStdHandle(kind);
        if (original && original != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate,
                             0, TRUE, DUPLICATE_SAME_ACCESS))
            return Handle(duplicate);
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle null_stream(CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!null_stream) throw WindowsError("Open fallback standard stream");
        return null_stream;
    }
    std::array<Handle, 3> handles_;
    std::array<HANDLE, 3> list_{};
    std::vector<unsigned char> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_ = nullptr;
};

std::atomic<HANDLE> cancel_event{nullptr};
BOOL WINAPI ConsoleControl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        HANDLE event = cancel_event.load(std::memory_order_relaxed);
        if (event) SetEvent(event);
        return TRUE;
    }
    return FALSE;
}

BOOL CALLBACK CloseOwnWindow(HWND window, LPARAM process) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(process)) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}

struct Child {
    Handle process;
    Handle main_thread;
    DWORD id = 0;
    DWORD thread_id = 0;
    HANDLE start = nullptr;
    bool completed = false;

    ~Child() {
        if (!process || completed || WaitForSingleObject(process.Get(), 0) != WAIT_TIMEOUT)
            return;
        // Only the child just created by this launcher is ever closed/terminated.
        // Release the gate even on failure so no child is left waiting 30 seconds.
        if (start) SetEvent(start);
        EnumWindows(CloseOwnWindow, static_cast<LPARAM>(id));
        PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
        if (WaitForSingleObject(process.Get(), 750) == WAIT_TIMEOUT) {
            EnumWindows(CloseOwnWindow, static_cast<LPARAM>(id));
            PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
            if (WaitForSingleObject(process.Get(), 750) == WAIT_TIMEOUT) {
                Log("Own demo did not close gracefully; terminating that child only.");
                TerminateProcess(process.Get(), 1);
                WaitForSingleObject(process.Get(), 2000);
            }
        }
    }
};

struct ChildExited { DWORD code; };

DWORD ExitCode(Child& child) {
    DWORD code = 1;
    if (!GetExitCodeProcess(child.process.Get(), &code))
        throw WindowsError("Read demo exit status");
    child.completed = true;
    return code;
}

void Await(Child& child, HANDLE event, HANDLE failed, HANDLE cancel, DWORD timeout,
           const char* stage) {
    const HANDLE handles[] = {event, child.process.Get(), failed, cancel};
    const DWORD result = WaitForMultipleObjects(_countof(handles), handles, FALSE, timeout);
    if (result == WAIT_OBJECT_0) return;
    if (result == WAIT_OBJECT_0 + 1) {
        Log(std::string("Demo exited while waiting for ") + stage + '.');
        throw ChildExited{ExitCode(child)};
    }
    if (result == WAIT_OBJECT_0 + 2)
        throw std::runtime_error(std::string("Native HUD initialization failed during ") + stage +
                                 "; see the inherited demo stderr/debug output");
    if (result == WAIT_OBJECT_0 + 3)
        throw std::runtime_error("Launch cancelled");
    if (result == WAIT_TIMEOUT)
        throw std::runtime_error(std::string("Timed out waiting for ") + stage);
    throw WindowsError("Wait for demo startup");
}

DWORD Remaining(ULONGLONG deadline, DWORD maximum) {
    const ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, maximum));
}

std::uintptr_t RemoteModule(DWORD process, const std::wstring& basename,
                             const fs::path* expected_path = nullptr) {
    Handle snapshot;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        snapshot = Handle(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process));
        if (snapshot) break;
        const DWORD error = GetLastError();
        if (error != ERROR_BAD_LENGTH || attempt == 7)
            throw WindowsError("Snapshot own child modules", error);
        // Microsoft documents retrying ERROR_BAD_LENGTH when the loader's
        // module list changes while a Toolhelp snapshot is being captured.
        Sleep(1);
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot.Get(), &entry); more;
         more = Module32NextW(snapshot.Get(), &entry)) {
        if (_wcsicmp(entry.szModule, basename.c_str()) != 0)
            continue;
        if (expected_path) {
            std::error_code error;
            const bool same_file = fs::equivalent(fs::path(entry.szExePath), *expected_path, error);
            if (error || !same_file)
                continue;
        }
        return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    }
    return 0;
}

void InjectOwnHud(Child& child, const fs::path& dll, HANDLE cancel, DWORD timeout) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC load_library = kernel ? GetProcAddress(kernel, "LoadLibraryW") : nullptr;
    if (!load_library) throw WindowsError("Resolve LoadLibraryW");
    // On current Windows this export can be forwarded to KernelBase. Compute
    // the offset in the module actually containing the function, rather than
    // assuming identical module bases in the launcher and child.
    HMODULE local_owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(load_library), &local_owner))
        throw WindowsError("Resolve LoadLibraryW owner");
    wchar_t owner_path[32768]{};
    const DWORD owner_length = GetModuleFileNameW(local_owner, owner_path, _countof(owner_path));
    if (!owner_length || owner_length >= _countof(owner_path))
        throw WindowsError("Read LoadLibraryW owner path");
    const std::uintptr_t remote_owner = RemoteModule(child.id, fs::path(owner_path).filename().native());
    if (!remote_owner)
        throw std::runtime_error("LoadLibraryW system module is absent from the own demo child");
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(load_library) -
                                  reinterpret_cast<std::uintptr_t>(local_owner);
    const auto remote_load = reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_owner + offset);
    const std::size_t bytes = (dll.native().size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(child.process.Get(), nullptr, bytes,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) throw WindowsError("Allocate own DLL path in demo");
    SIZE_T written = 0;
    if (!WriteProcessMemory(child.process.Get(), remote_path, dll.c_str(), bytes, &written) ||
        written != bytes) {
        const DWORD error = GetLastError();
        VirtualFreeEx(child.process.Get(), remote_path, 0, MEM_RELEASE);
        throw WindowsError("Copy own DLL path into demo", error);
    }
    Handle thread(CreateRemoteThread(child.process.Get(), nullptr, 0, remote_load, remote_path,
                                     0, nullptr));
    if (!thread) {
        const DWORD error = GetLastError();
        VirtualFreeEx(child.process.Get(), remote_path, 0, MEM_RELEASE);
        throw WindowsError("Load own HUD DLL in own demo", error);
    }
    const HANDLE handles[] = {thread.Get(), child.process.Get(), cancel};
    const DWORD result = WaitForMultipleObjects(_countof(handles), handles, FALSE, timeout);
    if (result == WAIT_OBJECT_0) {
        VirtualFreeEx(child.process.Get(), remote_path, 0, MEM_RELEASE);
        // A 64-bit HMODULE cannot be validated through GetExitCodeThread's DWORD.
        bool loaded = false;
        for (unsigned attempt = 0; attempt < 10 && !loaded; ++attempt) {
            loaded = RemoteModule(child.id, L"FrameScope.Hud.dll", &dll) != 0;
            if (!loaded) Sleep(10); // Read-only verification, not another injection.
        }
        if (!loaded) {
            DWORD returned = 0;
            GetExitCodeThread(thread.Get(), &returned);
            throw std::runtime_error("LoadLibraryW did not load the packaged FrameScope.Hud.dll; "
                "check its x64 dependencies and demo stderr (load return low 32 bits: " +
                std::to_string(returned) + ")");
        }
        return;
    }
    // A running LoadLibrary thread may still read the path. Keep this tiny
    // allocation until the failed child is closed by Child's cleanup guard.
    if (result == WAIT_OBJECT_0 + 1) throw ChildExited{ExitCode(child)};
    if (result == WAIT_OBJECT_0 + 2) throw std::runtime_error("Launch cancelled during DLL load");
    if (result == WAIT_TIMEOUT) throw std::runtime_error("Timed out loading own HUD DLL");
    throw WindowsError("Wait for own HUD DLL load");
}

DWORD Run(const Options& options, HANDLE cancel) {
    const fs::path directory = ExecutablePath().parent_path();
    const fs::path demo = ExistingFile(options.demo);
    if (_wcsicmp(demo.filename().c_str(), L"FrameScope.Demo.exe") != 0)
        throw std::runtime_error("Resolved preview target must be named FrameScope.Demo.exe");
    // Also require the packaged demo, not an arbitrary renamed executable from
    // elsewhere. --demo remains explicit so a UI cannot silently select a game.
    const fs::path packaged_demo = ExistingFile(directory / L"FrameScope.Demo.exe");
    if (_wcsicmp(demo.c_str(), packaged_demo.c_str()) != 0)
        throw std::runtime_error("Preview policy: --demo must select FrameScope.Demo.exe beside the launcher");
    RequireX64Pe(demo, false);
    ValidateMapping(options.session);
    const bool vulkan = options.api == L"vulkan";
    fs::path dll;
    if (vulkan) {
        if (Elevated())
            throw std::runtime_error("Vulkan preview must run non-elevated: the Vulkan loader ignores "
                                     "scoped layer environment variables in elevated processes. "
                                     "Close administrator terminals/UI and launch normally.");
        ExistingFile(directory / L"FrameScope.Vulkan.json");
        const fs::path layer = ExistingFile(directory / L"FrameScope.Vulkan.dll");
        RequireX64Pe(layer, true);
    } else {
        dll = ExistingFile(directory / L"FrameScope.Hud.dll");
        RequireX64Pe(dll, true);
    }

    Environment environment = CloneEnvironment();
    environment[L"FRAMESCOPE_SESSION"] = options.session;
    environment[L"FRAMESCOPE_DEMO_GATE"] = L"1";
    if (vulkan) {
        AppendList(environment, L"VK_ADD_LAYER_PATH", directory.native());
        // VK_LAYER_PATH overrides VK_ADD_LAYER_PATH. Preserve its existing
        // contents, but also append our manifest directory when it is set.
        if (environment.find(L"VK_LAYER_PATH") != environment.end())
            AppendList(environment, L"VK_LAYER_PATH", directory.native());
        AppendList(environment, L"VK_INSTANCE_LAYERS", L"VK_LAYER_FRAMESCOPE_hud");
        Log("Vulkan uses a scoped explicit layer; no DLL injection or machine/registry changes.");
    }
    std::vector<wchar_t> child_environment = EnvironmentBlock(environment);
    std::wstring command = Quote(demo.native()) + L" --api " + Quote(options.api) +
                           L" --mode " + Quote(options.mode);
    for (const auto& argument : options.forwarded)
        command += L" " + argument.first + L" " + Quote(argument.second);
    Handle ready = NewEvent(options.session, L"ready");
    Handle start = NewEvent(options.session, L"start");
    Handle loaded = NewEvent(options.session, L"loaded");
    Handle hook = NewEvent(options.session, L"hook");
    Handle failed = NewEvent(options.session, L"failed");
    InheritedStreams streams;
    STARTUPINFOEXW startup{};
    streams.Apply(startup);
    PROCESS_INFORMATION process{};
    const ULONGLONG gate_deadline = GetTickCount64() + 25000;
    if (!CreateProcessW(demo.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        child_environment.data(), directory.c_str(), &startup.StartupInfo, &process))
        throw WindowsError("Create own demo process");
    Child child;
    child.process = Handle(process.hProcess);
    child.main_thread = Handle(process.hThread);
    child.id = process.dwProcessId;
    child.thread_id = process.dwThreadId;
    child.start = start.Get();
    try {
        Await(child, ready.Get(), failed.Get(), cancel, Remaining(gate_deadline, 10000), "demo ready gate");
        if (!vulkan) {
            InjectOwnHud(child, dll, cancel, Remaining(gate_deadline, 10000));
            if (!SetEvent(loaded.Get())) throw WindowsError("Confirm completed own HUD DLL load");
            Await(child, hook.Get(), failed.Get(), cancel, Remaining(gate_deadline, 10000), "DXGI hook-ready event");
        }
        if (!SetEvent(start.Get())) throw WindowsError("Release demo graphics-start gate");
        // The Vulkan loader negotiates the explicit layer after start. Unlike
        // injected DXGI there is no pre-graphics hook-ready handshake to await;
        // the UI/smoke broker validates actual layer-produced frame telemetry.
        const HANDLE handles[] = {child.process.Get(), cancel};
        const DWORD wait = WaitForMultipleObjects(_countof(handles), handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            const DWORD code = ExitCode(child);
            if (vulkan && code != 0)
                Log("Vulkan preview failed. Use a non-elevated process and an installed Vulkan "
                    "runtime/driver; check demo diagnostics and preserved VK_LAYER_PATH/"
                    "VK_LOADER_LAYERS_* filters.");
            return code;
        }
        if (wait == WAIT_OBJECT_0 + 1) throw std::runtime_error("Launch cancelled");
        throw WindowsError("Wait for own demo exit");
    } catch (const ChildExited& exited) {
        return exited.code;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    static_assert(sizeof(void*) == 8, "FrameScope.Launcher requires Windows x64");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Handle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancel) { Log("Cannot create launch cancellation event."); return 1; }
    cancel_event.store(cancel.Get(), std::memory_order_relaxed);
    SetConsoleCtrlHandler(ConsoleControl, TRUE);
    int result = 1;
    try {
        Options options;
        result = Parse(argc, argv, options) ? static_cast<int>(Run(options, cancel.Get())) : 0;
    } catch (const std::exception& error) {
        Log(error.what());
    } catch (...) {
        Log("Unexpected launcher failure.");
    }
    SetConsoleCtrlHandler(ConsoleControl, FALSE);
    cancel_event.store(nullptr, std::memory_order_relaxed);
    return result;
}
