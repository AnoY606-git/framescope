// SPDX-License-Identifier: GPL-3.0-only
#include "overlay.hpp"

#include <MinHook.h>

#include <array>
#include <cwchar>
#include <mutex>
#include <utility>

namespace framescope::dxgi {
namespace {

constexpr std::size_t kHookVariants = 8;
std::mutex hook_mutex;
thread_local unsigned suppress_callbacks = 0;
thread_local unsigned present_depth = 0;
thread_local unsigned resize_depth = 0;

struct DepthScope {
    explicit DepthScope(unsigned& value) : value_(value) { ++value_; }
    ~DepthScope() { --value_; }
    unsigned& value_;
};

template <class Fn> struct MethodSlot {
    void* target = nullptr;
    Fn original = nullptr;
};
template <class Fn> using MethodSlots = std::array<MethodSlot<Fn>, kHookVariants>;

void HookError(const wchar_t* operation, MH_STATUS status) {
    wchar_t text[256]{};
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"%ls (MinHook status %d).",
                 operation, static_cast<int>(status));
    Log(text);
}

template <class Fn>
bool InstallMethod(MethodSlots<Fn>& slots, void* target,
                   const std::array<Fn, kHookVariants>& detours) {
    std::lock_guard<std::mutex> lock(hook_mutex);
    for (const auto& slot : slots)
        if (slot.target == target)
            return true;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        auto& slot = slots[index];
        if (slot.target)
            continue;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detours[index]),
                                         reinterpret_cast<void**>(&slot.original));
        if (status != MH_OK) {
            HookError(L"Cannot create DXGI detour", status);
            return false;
        }
        slot.target = target;
        status = MH_EnableHook(target);
        if (status == MH_OK || status == MH_ERROR_ENABLED)
            return true;
        HookError(L"Cannot enable DXGI detour", status);
        MH_RemoveHook(target); // Only this newly created, never-enabled hook.
        slot = {};
        return false;
    }
    Log(L"DXGI method implementation limit reached.");
    return false;
}

template <class Fn>
bool InstallExport(HMODULE module, const char* name, Fn detour, Fn& original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target)
        return false;
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour),
                                     reinterpret_cast<void**>(&original));
    if (status != MH_OK) {
        HookError(L"Cannot create DXGI export detour", status);
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        HookError(L"Cannot enable DXGI export detour", status);
        return false;
    }
    return true;
}

void** Vtable(IUnknown* object) { return *reinterpret_cast<void***>(object); }

using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT,
                                              UINT, const UINT*, IUnknown* const*);
using CreateFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                             IDXGISwapChain**);
using CreateHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
    IDXGISwapChain1**);
using CreateCoreFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCompositionFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

MethodSlots<PresentFn> present_slots;
MethodSlots<Present1Fn> present1_slots;
MethodSlots<ResizeFn> resize_slots;
MethodSlots<Resize1Fn> resize1_slots;
MethodSlots<CreateFn> create_slots;
MethodSlots<CreateHwndFn> hwnd_slots;
MethodSlots<CreateCoreFn> core_slots;
MethodSlots<CreateCompositionFn> composition_slots;

bool InstallSwapchainHooks(IDXGISwapChain* swapchain);
bool InstallFactoryHooks(IUnknown* factory);

void Created(IUnknown* device, IDXGISwapChain* swapchain, HWND window = nullptr) noexcept {
    if (suppress_callbacks || !swapchain)
        return;
    try {
        if (!InstallSwapchainHooks(swapchain))
            Log(L"Could not install every present/resize hook on the new swapchain.");
        CaptureSwapchain(swapchain, device, window);
    } catch (...) {
        Log(L"Could not register the new swapchain; original creation result unchanged.");
    }
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* chain, UINT interval, UINT flags) {
    const auto original = present_slots[Index].original;
    if (suppress_callbacks || present_depth)
        return original(chain, interval, flags);
    DepthScope depth(present_depth);
    if (flags & DXGI_PRESENT_TEST)
        return original(chain, interval, flags);
    const PresentTicket ticket = BeforePresent(chain);
    const HRESULT result = original(chain, interval, flags);
    AfterPresent(ticket, result);
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* chain, UINT interval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* parameters) {
    const auto original = present1_slots[Index].original;
    if (suppress_callbacks || present_depth)
        return original(chain, interval, flags, parameters);
    DepthScope depth(present_depth);
    if (flags & DXGI_PRESENT_TEST)
        return original(chain, interval, flags, parameters);
    const PresentTicket ticket = BeforePresent(chain);
    const HRESULT result = original(chain, interval, flags, parameters);
    AfterPresent(ticket, result);
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourResize(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
                                       DXGI_FORMAT format, UINT flags) {
    const auto original = resize_slots[Index].original;
    if (suppress_callbacks || resize_depth)
        return original(chain, count, width, height, format, flags);
    DepthScope depth(resize_depth);
    BeforeResize(chain);
    return original(chain, count, width, height, format, flags);
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourResize1(IDXGISwapChain3* chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* node_masks, IUnknown* const* queues) {
    const auto original = resize1_slots[Index].original;
    if (suppress_callbacks || resize_depth)
        return original(chain, count, width, height, format, flags, node_masks, queues);
    DepthScope depth(resize_depth);
    BeforeResize(chain);
    const HRESULT result = original(chain, count, width, height, format, flags, node_masks, queues);
    if (SUCCEEDED(result) && queues) {
        if (!count) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(chain->GetDesc(&desc)))
                count = desc.BufferCount;
        }
        CaptureResizeQueues(chain, count, queues);
    }
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourCreate(IDXGIFactory* factory, IUnknown* device,
                                       DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** output) {
    const HRESULT result = create_slots[Index].original(factory, device, desc, output);
    if (SUCCEEDED(result) && output)
        Created(device, *output, desc ? desc->OutputWindow : nullptr);
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourCreateHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrict_output, IDXGISwapChain1** output) {
    const HRESULT result = hwnd_slots[Index].original(factory, device, window, desc, fullscreen,
                                                      restrict_output, output);
    if (SUCCEEDED(result) && output)
        Created(device, *output, window);
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourCreateCore(IDXGIFactory2* factory, IUnknown* device,
    IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_output,
    IDXGISwapChain1** output) {
    const HRESULT result = core_slots[Index].original(factory, device, window, desc,
                                                      restrict_output, output);
    if (SUCCEEDED(result) && output)
        Created(device, *output);
    return result;
}

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE DetourCreateComposition(IDXGIFactory2* factory, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_output, IDXGISwapChain1** output) {
    const HRESULT result = composition_slots[Index].original(factory, device, desc,
                                                             restrict_output, output);
    if (SUCCEEDED(result) && output)
        Created(device, *output);
    return result;
}

template <std::size_t... Indices>
bool InstallSwapchainImpl(IDXGISwapChain* swapchain, std::index_sequence<Indices...>) {
    bool okay = InstallMethod(present_slots, Vtable(swapchain)[8],
        std::array<PresentFn, kHookVariants>{&DetourPresent<Indices>...});
    okay = InstallMethod(resize_slots, Vtable(swapchain)[13],
        std::array<ResizeFn, kHookVariants>{&DetourResize<Indices>...}) && okay;
    ComPtr<IDXGISwapChain1> chain1;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(chain1.GetAddressOf()))))
        okay = InstallMethod(present1_slots, Vtable(chain1.Get())[22],
            std::array<Present1Fn, kHookVariants>{&DetourPresent1<Indices>...}) && okay;
    ComPtr<IDXGISwapChain3> chain3;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(chain3.GetAddressOf()))))
        okay = InstallMethod(resize1_slots, Vtable(chain3.Get())[39],
            std::array<Resize1Fn, kHookVariants>{&DetourResize1<Indices>...}) && okay;
    return okay;
}

bool InstallSwapchainHooks(IDXGISwapChain* swapchain) {
    return InstallSwapchainImpl(swapchain, std::make_index_sequence<kHookVariants>{});
}

template <std::size_t... Indices>
bool InstallFactoryImpl(IUnknown* object, std::index_sequence<Indices...>) {
    ComPtr<IDXGIFactory> factory;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(factory.GetAddressOf()))))
        return false;
    bool okay = InstallMethod(create_slots, Vtable(factory.Get())[10],
        std::array<CreateFn, kHookVariants>{&DetourCreate<Indices>...});
    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(factory2.GetAddressOf())))) {
        okay = InstallMethod(hwnd_slots, Vtable(factory2.Get())[15],
            std::array<CreateHwndFn, kHookVariants>{&DetourCreateHwnd<Indices>...}) && okay;
        // These paths are hooked for queue capture, but not selected as the
        // preview's primary swapchain unless they have an eligible main HWND.
        InstallMethod(core_slots, Vtable(factory2.Get())[16],
            std::array<CreateCoreFn, kHookVariants>{&DetourCreateCore<Indices>...});
        InstallMethod(composition_slots, Vtable(factory2.Get())[24],
            std::array<CreateCompositionFn, kHookVariants>{&DetourCreateComposition<Indices>...});
    } else {
        okay = false;
    }
    return okay;
}

bool InstallFactoryHooks(IUnknown* factory) {
    return InstallFactoryImpl(factory, std::make_index_sequence<kHookVariants>{});
}

using FactoryFn = HRESULT (WINAPI*)(REFIID, void**);
using Factory2Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
FactoryFn original_factory = nullptr;
FactoryFn original_factory1 = nullptr;
Factory2Fn original_factory2 = nullptr;

void FactoryCreated(HRESULT result, void** factory) noexcept {
    if (SUCCEEDED(result) && factory && *factory) {
        try {
            if (!InstallFactoryHooks(static_cast<IUnknown*>(*factory)))
                Log(L"Factory creation detours are incomplete.");
        } catch (...) {
            Log(L"Could not hook a new factory; original HRESULT unchanged.");
        }
    }
}

HRESULT WINAPI DetourFactory(REFIID iid, void** factory) {
    const HRESULT result = original_factory(iid, factory);
    FactoryCreated(result, factory);
    return result;
}
HRESULT WINAPI DetourFactory1(REFIID iid, void** factory) {
    const HRESULT result = original_factory1(iid, factory);
    FactoryCreated(result, factory);
    return result;
}
HRESULT WINAPI DetourFactory2(UINT flags, REFIID iid, void** factory) {
    const HRESULT result = original_factory2(flags, iid, factory);
    FactoryCreated(result, factory);
    return result;
}

bool BootstrapSwapchainHooks(HINSTANCE module) {
    const wchar_t* class_name = L"FrameScope.Hud.Bootstrap";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = module;
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class))
        return false;
    HWND window = CreateWindowExW(0, class_name, L"", WS_OVERLAPPED, 0, 0, 16, 16,
                                   nullptr, nullptr, module, nullptr);
    bool okay = false;
    if (window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 16;
        desc.BufferDesc.Height = 16;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<IDXGISwapChain> chain;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, chain.GetAddressOf(),
            device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (SUCCEEDED(result))
            okay = InstallSwapchainHooks(chain.Get());
        else
            Log(L"Cannot create the hidden hook-bootstrap D3D11 swapchain.", result);
        chain.Reset();
        context.Reset();
        device.Reset();
        DestroyWindow(window);
    }
    UnregisterClassW(class_name, module);
    return okay;
}

bool OptedInOwnDemo() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path))
        return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    wchar_t gate[8]{};
    return _wcsicmp(name, L"FrameScope.Demo.exe") == 0 &&
        GetEnvironmentVariableW(L"FRAMESCOPE_DEMO_GATE", gate, _countof(gate)) == 1 &&
        gate[0] == L'1' && ValidSession(SessionFromEnvironment());
}

void SignalSessionEvent(const wchar_t* suffix) {
    const std::wstring session = SessionFromEnvironment();
    if (!ValidSession(session))
        return;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(session, suffix).c_str());
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
}

bool AwaitCompletedLoad() {
    const std::wstring session = SessionFromEnvironment();
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, EventName(session, L"loaded").c_str());
    if (!event) {
        Log(L"Missing launcher load-complete gate; refusing unsynchronized hook installation.");
        return false;
    }
    const DWORD result = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    if (result != WAIT_OBJECT_0)
        Log(L"Launcher did not confirm completed LoadLibrary before hook initialization.");
    return result == WAIT_OBJECT_0;
}

} // namespace

bool InstallHooks() {
    DepthScope suppressed(suppress_callbacks);
    Log(L"Installing DXGI interception outside loader lock.");
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        HookError(L"Cannot initialize MinHook", status);
        return false;
    }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi)
        return false;
    bool okay = InstallExport(dxgi, "CreateDXGIFactory", &DetourFactory, original_factory);
    okay = InstallExport(dxgi, "CreateDXGIFactory1", &DetourFactory1, original_factory1) && okay;
    okay = InstallExport(dxgi, "CreateDXGIFactory2", &DetourFactory2, original_factory2) && okay;
    ComPtr<IDXGIFactory1> factory;
    const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        Log(L"Cannot bootstrap DXGI factory.", result);
        return false;
    }
    okay = InstallFactoryHooks(factory.Get()) && okay;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&InstallHooks), &module))
        return false;
    Log(L"Preparing hidden D3D11 swapchain for hook discovery.");
    okay = BootstrapSwapchainHooks(module) && okay;
    return okay;
}

DWORD WINAPI InitializeWorker(void* module) {
    Log(L"Initialization worker entered.");
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&InitializeWorker), &pinned))
        return 1;
    (void)module;
    try {
        if (!OptedInOwnDemo()) {
            Log(L"Refusing hooks: this DLL is restricted to the opted-in FrameScope.Demo.exe.");
            SignalSessionEvent(L"failed");
            return 1;
        }
        // MinHook briefly suspends peer threads to patch code. Do not race the
        // injection thread's loader/TLS detach work: the launcher confirms that
        // LoadLibrary's thread has COMPLETELY exited before this worker patches.
        if (!AwaitCompletedLoad() || !InitializeOverlay() || !InstallHooks()) {
            Log(L"Hook initialization failed; the hook-ready event was not signaled.");
            SignalSessionEvent(L"failed");
            return 1;
        }
        Log(L"DXGI factory, Present/Present1 and resize hooks installed.");
        SignalSessionEvent(L"hook");
        return 0;
    } catch (...) {
        Log(L"Unexpected hook initialization failure.");
        SignalSessionEvent(L"failed");
        return 1;
    }
}

} // namespace framescope::dxgi
