// SPDX-License-Identifier: GPL-3.0-only
#include "demo.hpp"
#include "win_util.hpp"
#include "../include/framescope/protocol.hpp"
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace framescope::demo {
namespace {
using Clock = std::chrono::steady_clock;

void Help() {
    std::cout << "FrameScope.Demo (standalone original scene; no HUD)\n"
                 "  --api dx11|dx12|vulkan\n"
                 "  --mode windowed|borderless|exclusive\n"
                 "  --duration seconds       0 = interactive (default)\n"
                 "  --cpu-load 0..100        busy work: N% of a 16.667 ms budget/frame\n"
                 "  --gpu-load 1..100        complete scene draw calls/frame\n"
                 "  --stutter-ms 0..5000     deliberate pause every 2 seconds\n"
                 "  --vsync on|off\n"
                 "Esc quits; Space toggles stutter (100 ms if initially zero);\n"
                 "+/- changes GPU draw-call count. Diagnostics go to stderr;\n"
                 "one JSON summary goes to stdout on exit.\n";
}

double Number(const std::string& text, const char* option, double low, double high,
              bool integral = false) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || errno == ERANGE ||
        !std::isfinite(value) || value < low || value > high ||
        (integral && std::floor(value) != value)) {
        throw std::runtime_error(std::string("Invalid value for ") + option);
    }
    return value;
}

bool Parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") { Help(); return false; }
        if (i + 1 >= argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--api") {
            if (value == "dx11") options.api = Api::Dx11;
            else if (value == "dx12") options.api = Api::Dx12;
            else if (value == "vulkan") options.api = Api::Vulkan;
            else throw std::runtime_error("Unknown graphics API: " + value);
        } else if (key == "--mode") {
            if (value == "windowed") options.mode = WindowMode::Windowed;
            else if (value == "borderless") options.mode = WindowMode::Borderless;
            else if (value == "exclusive") options.mode = WindowMode::Exclusive;
            else throw std::runtime_error("Unknown window mode: " + value);
        } else if (key == "--duration") {
            options.duration = Number(value, "--duration", 0, 86400);
        } else if (key == "--cpu-load") {
            options.cpuLoad = static_cast<unsigned>(Number(value, "--cpu-load", 0, 100, true));
        } else if (key == "--gpu-load") {
            options.gpuLoad = static_cast<unsigned>(Number(value, "--gpu-load", 1, 100, true));
        } else if (key == "--stutter-ms") {
            options.stutterMs = static_cast<unsigned>(Number(value, "--stutter-ms", 0, 5000, true));
        } else if (key == "--vsync") {
            if (value == "on") options.vsync = true;
            else if (value == "off") options.vsync = false;
            else throw std::runtime_error("--vsync must be on or off");
        } else {
            throw std::runtime_error("Unknown option: " + key);
        }
    }
    return true;
}

std::wstring Environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size) throw std::runtime_error("Environment changed during startup");
    value.resize(written);
    return value;
}

void StartupGate() {
    if (Environment(L"FRAMESCOPE_DEMO_GATE") != L"1") return;
    const auto session = Environment(L"FRAMESCOPE_SESSION");
    const bool valid = session.size() == 32 && std::all_of(session.begin(), session.end(),
        [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                             (c >= L'A' && c <= L'F'); });
    if (!valid) throw std::runtime_error("FRAMESCOPE_SESSION must be exactly 32 hexadecimal characters");
    UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE,
        framescope::EventName(session, L"ready").c_str()));
    UniqueHandle start(CreateEventW(nullptr, TRUE, FALSE,
        framescope::EventName(session, L"start").c_str()));
    if (!ready.Get() || !start.Get()) throw std::runtime_error("Could not open startup gate events");
    if (!SetEvent(ready.Get())) throw std::runtime_error("Could not signal startup ready event");
    std::cerr << "Startup ready; waiting for launcher before any graphics initialization.\n";
    const DWORD result = WaitForSingleObject(start.Get(), 30000);
    if (result == WAIT_TIMEOUT) throw std::runtime_error("Startup gate timed out after 30 seconds");
    if (result != WAIT_OBJECT_0) throw std::runtime_error("Startup gate wait failed");
}

struct InputState {
    bool quit = false;
    bool stutter = false;
    unsigned gpuLoad = 1;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* input = reinterpret_cast<InputState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        input = static_cast<InputState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(input));
    }
    switch (message) {
    case WM_CLOSE:
        if (input) input->quit = true;
        return 0; // Keep HWND alive until all graphics resources are released.
    case WM_DESTROY:
        if (input) input->quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (input) {
            if (wParam == VK_ESCAPE) input->quit = true;
            if (wParam == VK_SPACE && !(lParam & (1ll << 30))) input->stutter = !input->stutter;
            if (wParam == VK_ADD || wParam == VK_OEM_PLUS)
                input->gpuLoad = std::min(input->gpuLoad + 1, 100u);
            if (wParam == VK_SUBTRACT || wParam == VK_OEM_MINUS)
                input->gpuLoad = std::max(input->gpuLoad - 1, 1u);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN) return 0; // Alt+Enter must not silently change modes.
        break;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

class AppWindow {
public:
    ~AppWindow() {
        if (window_) DestroyWindow(window_);
        if (registered_) UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
    }
    void Create(const Options& options, InputState& input) {
        // Optional newer DPI API; retain a Windows-compatible fallback.
        using SetDpi = BOOL(WINAPI*)(HANDLE);
        const auto dpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                               "SetProcessDpiAwarenessContext"));
        if (dpi) dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        else SetProcessDPIAware();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProcedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassExW failed");
        registered_ = true;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
            throw std::runtime_error("GetMonitorInfoW failed");
        const bool popup = options.mode != WindowMode::Windowed;
        const DWORD style = popup ? WS_POPUP : WS_OVERLAPPEDWINDOW;
        RECT rectangle{};
        if (popup) {
            rectangle = monitor.rcMonitor;
        } else {
            rectangle = {0, 0, std::min<LONG>(1280, monitor.rcWork.right - monitor.rcWork.left - 64),
                               std::min<LONG>(720, monitor.rcWork.bottom - monitor.rcWork.top - 96)};
            AdjustWindowRectEx(&rectangle, style, FALSE, 0);
            OffsetRect(&rectangle, monitor.rcWork.left + 32 - rectangle.left,
                       monitor.rcWork.top + 32 - rectangle.top);
        }
        std::wstring title = L"FrameScope Demo - ";
        const std::string api = ApiName(options.api);
        title.append(api.begin(), api.end());
        title += popup ? (options.mode == WindowMode::Exclusive ? L" - exclusive requested" : L" - borderless")
                       : L" - windowed";
        window_ = CreateWindowExW(0, kClassName, title.c_str(), style,
            rectangle.left, rectangle.top, rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top, nullptr, nullptr, wc.hInstance, &input);
        if (!window_) throw std::runtime_error("CreateWindowExW failed");
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
    }
    HWND Get() const noexcept { return window_; }
private:
    static constexpr const wchar_t* kClassName = L"FrameScopeStandaloneDemo";
    HWND window_ = nullptr;
    bool registered_ = false;
};

void BusyWork(unsigned load) {
    if (!load) return;
    const auto end = Clock::now() + std::chrono::duration<double, std::milli>(load / 6.0);
    static std::atomic<std::uint32_t> sink{1};
    std::uint32_t value = sink.load(std::memory_order_relaxed);
    while (Clock::now() < end) {
        for (int i = 0; i < 1024; ++i) value = value * 1664525u + 1013904223u;
        sink.store(value, std::memory_order_relaxed);
    }
}

std::string JsonString(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                << static_cast<unsigned>(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}
} // namespace
} // namespace framescope::demo

static int RunDemo(int argc, char** argv) {
    using namespace framescope::demo;
    static_assert(sizeof(void*) == 8, "FrameScope.Demo requires Windows x64");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Options options;
    InputState input;
    AppWindow window;
    std::unique_ptr<Renderer> renderer;
    std::uint64_t frameCount = 0;
    unsigned width = 0, height = 0;
    bool fullscreen = false;
    int status = 0;
    std::string error;
    auto started = Clock::now();
    try {
        if (!Parse(argc, argv, options)) return 0;
        StartupGate(); // Absolutely no graphics initialization precedes this.
        input.gpuLoad = options.gpuLoad;
        input.stutter = options.stutterMs != 0;
        window.Create(options, input);
        RECT client{};
        if (!GetClientRect(window.Get(), &client)) throw std::runtime_error("GetClientRect failed");
        unsigned clientWidth = static_cast<unsigned>(client.right);
        unsigned clientHeight = static_cast<unsigned>(client.bottom);
        switch (options.api) {
        case Api::Dx11: renderer = MakeDx11Renderer(); break;
        case Api::Dx12: renderer = MakeDx12Renderer(); break;
        case Api::Vulkan: renderer = MakeVulkanRenderer(); break;
        }
        renderer->Initialize(window.Get(), clientWidth, clientHeight, options);
        width = renderer->Width(); height = renderer->Height();
        std::cerr << "Rendering " << ApiName(options.api) << ' ' << width << 'x' << height
                  << "; exclusive acquired=" << (renderer->FullscreenAcquired() ? "true" : "false") << '\n';
        started = Clock::now();
        double nextStutter = 2.0;
        while (!input.quit) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) input.quit = true;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (input.quit) break;
            double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
            if (options.duration > 0 && elapsed >= options.duration) break;
            if (IsIconic(window.Get())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!GetClientRect(window.Get(), &client)) throw std::runtime_error("GetClientRect failed");
            const unsigned newWidth = static_cast<unsigned>(client.right);
            const unsigned newHeight = static_cast<unsigned>(client.bottom);
            if (!newWidth || !newHeight) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (clientWidth != newWidth || clientHeight != newHeight) {
                renderer->Resize(newWidth, newHeight);
                clientWidth = newWidth; clientHeight = newHeight;
            }
            if (elapsed >= nextStutter) {
                nextStutter = elapsed + 2.0;
                if (input.stutter) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        options.stutterMs ? options.stutterMs : 100));
                    const double afterPause = std::chrono::duration<double>(Clock::now() - started).count();
                    if (afterPause >= nextStutter) nextStutter = afterPause + 2.0;
                }
            }
            BusyWork(options.cpuLoad);
            elapsed = std::chrono::duration<double>(Clock::now() - started).count();
            if (options.duration > 0 && elapsed >= options.duration) break;
            const auto vertices = BuildScene(elapsed, renderer->Width(), renderer->Height());
            if (renderer->Render(vertices, input.gpuLoad) == PresentResult::Presented) ++frameCount;
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            width = renderer->Width(); height = renderer->Height();
        }
    } catch (const std::exception& failure) {
        status = 1;
        error = failure.what();
        std::cerr << "FrameScope.Demo: " << error << '\n';
    } catch (...) {
        status = 1;
        error = "Unknown native exception";
        std::cerr << "FrameScope.Demo: " << error << '\n';
    }
    const double duration = std::chrono::duration<double>(Clock::now() - started).count();
    if (renderer) {
        width = renderer->Width(); height = renderer->Height();
        fullscreen = renderer->FullscreenAcquired();
    }
    renderer.reset(); // Release graphics and exclusive access before the HWND.
    std::cout.imbue(std::locale::classic());
    std::cout << "{\"api\":" << JsonString(ApiName(options.api))
              << ",\"frame_count\":" << frameCount
              << ",\"width\":" << width << ",\"height\":" << height
              << ",\"fullscreen_requested\":" << (options.mode == WindowMode::Exclusive ? "true" : "false")
              << ",\"fullscreen_acquired\":" << (fullscreen ? "true" : "false")
              << ",\"exit_status\":" << status
              << ",\"duration\":" << std::fixed << std::setprecision(6) << duration;
    if (!error.empty()) std::cout << ",\"error\":" << JsonString(error);
    std::cout << "}\n";
    return status;
}

// Support both a console build and the parent's WIN32/-municode executable.
// With redirected inherited handles the GUI entry point still writes the same
// stdout JSON; no console is allocated and no message box is ever displayed.
int main(int argc, char** argv) { return RunDemo(argc, argv); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) {
        std::cout << "{\"api\":\"unknown\",\"frame_count\":0,\"width\":0,\"height\":0,"
                     "\"fullscreen_requested\":false,\"fullscreen_acquired\":false,"
                     "\"exit_status\":1,\"duration\":0,\"error\":\"CommandLineToArgvW failed\"}\n";
        return 1;
    }
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(std::max(size, 1)), '\0');
        if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, text.data(), size, nullptr, nullptr);
        text.pop_back();
        arguments.push_back(std::move(text));
    }
    LocalFree(wide);
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) argv.push_back(argument.data());
    return RunDemo(argc, argv.data());
}
