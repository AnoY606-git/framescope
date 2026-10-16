// SPDX-License-Identifier: GPL-3.0-only
#include "overlay.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace framescope::dxgi {
namespace {

struct Candidate {
    IUnknown* identity = nullptr; // Non-owning: never keep the swapchain alive.
    HWND window = nullptr;
    ComPtr<ID3D12CommandQueue> queue;
};

struct Overlay {
    Session session;
    std::mutex mutex;
    std::vector<Candidate> candidates;
    IUnknown* primary = nullptr;
    Api api = Api::Unknown;
    std::unique_ptr<Renderer> renderer;
    bool renderer_attempted = false;
    bool renderer_skip_reported = false;
    std::atomic<bool> drawing_disabled{false};
};

// Deliberately process-lifetime. There is no unload/shutdown while detours can run.
Overlay* overlay = nullptr;

IUnknown* Identity(IUnknown* object) {
    ComPtr<IUnknown> identity;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()))))
        return nullptr;
    return identity.Get();
}

bool IsMainWindow(HWND window) {
    DWORD process = 0;
    if (!window || !IsWindow(window) || !IsWindowVisible(window) ||
        GetAncestor(window, GA_ROOT) != window || GetWindow(window, GW_OWNER) != nullptr)
        return false;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() &&
           !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
}

ComPtr<ID3D12CommandQueue> QueueFrom(IUnknown* object) {
    ComPtr<ID3D12CommandQueue> queue;
    if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))) &&
        queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        return queue;
    return {};
}

} // namespace

void Log(const wchar_t* message, HRESULT result) noexcept {
    wchar_t text[1024]{};
    if (FAILED(result))
        _snwprintf_s(text, _countof(text), _TRUNCATE,
                     L"[FrameScope.Hud] %ls (HRESULT 0x%08lX)\n", message,
                     static_cast<unsigned long>(result));
    else
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"[FrameScope.Hud] %ls\n", message);
    OutputDebugStringW(text);
    // The launcher inherits the demo's standard handles, including for smoke tests.
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error && error != INVALID_HANDLE_VALUE) {
        char utf8[3072]{};
        const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8,
                                              static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        DWORD written = 0;
        if (length > 1)
            WriteFile(error, utf8, static_cast<DWORD>(length - 1), &written, nullptr);
    }
}

ContextScope::ContextScope(ImGuiContext* context) : previous_(ImGui::GetCurrentContext()) {
    ImGui::SetCurrentContext(context);
}

ContextScope::~ContextScope() {
    ImGui::SetCurrentContext(previous_);
}

bool InitializeOverlay() {
    auto state = std::make_unique<Overlay>();
    if (!state->session.ConnectFromEnvironment()) {
        Log(L"Cannot connect to the UI session mapping; hooks not enabled.");
        return false;
    }
    overlay = state.release();
    return true;
}

void CaptureSwapchain(IDXGISwapChain* swapchain, IUnknown* creation_device, HWND window) {
    if (!overlay || !swapchain)
        return;
    Candidate candidate;
    candidate.identity = Identity(swapchain);
    candidate.queue = QueueFrom(creation_device);
    candidate.window = window;
    if (!candidate.window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swapchain->GetDesc(&desc)))
            candidate.window = desc.OutputWindow;
    }
    if (!candidate.identity)
        return;
    std::lock_guard<std::mutex> lock(overlay->mutex);
    auto& entries = overlay->candidates;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Candidate& item) {
        return item.identity == candidate.identity;
    });
    if (found != entries.end())
        *found = std::move(candidate);
    else if (entries.size() < 32)
        entries.push_back(std::move(candidate));
    else
        Log(L"Swapchain candidate limit reached; preview supports one main swapchain.");
}

PresentTicket BeforePresent(IDXGISwapChain* swapchain) noexcept {
    PresentTicket ticket;
    ticket.timestamp = QpcNow();
    try {
        if (!overlay)
            return ticket;
        std::unique_lock<std::mutex> lock(overlay->mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return ticket; // Never wait behind another present/resize to draw a HUD.

        IUnknown* identity = Identity(swapchain);
        if (!identity || (overlay->primary && overlay->primary != identity))
            return ticket;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapchain->GetDesc(&desc)))
            return ticket;

        if (!overlay->primary) {
            if (!IsMainWindow(desc.OutputWindow))
                return ticket;
            ComPtr<ID3D11Device> dx11;
            ComPtr<ID3D12Device> dx12;
            if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(dx11.GetAddressOf()))))
                overlay->api = Api::Dx11;
            else if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(dx12.GetAddressOf()))))
                overlay->api = Api::Dx12;
            else
                return ticket;
            overlay->primary = identity;
            Log(overlay->api == Api::Dx11 ? L"Selected main D3D11 swapchain."
                                         : L"Selected main D3D12 swapchain.");
        }

        ticket.primary = true;
        ticket.api = overlay->api;
        ticket.width = desc.BufferDesc.Width;
        ticket.height = desc.BufferDesc.Height;
        if (!ticket.width || !ticket.height) {
            RECT client{};
            if (GetClientRect(desc.OutputWindow, &client)) {
                ticket.width = static_cast<std::uint32_t>(std::max<LONG>(0, client.right));
                ticket.height = static_cast<std::uint32_t>(std::max<LONG>(0, client.bottom));
            }
        }

        ControlData control{};
        if (!overlay->session.ReadControl(control) || !(control.flags & HudFlags::Enabled) ||
            overlay->drawing_disabled.load(std::memory_order_relaxed) ||
            IsIconic(desc.OutputWindow) || !ticket.width || !ticket.height)
            return ticket;

        if (!overlay->renderer_attempted) {
            overlay->renderer_attempted = true;
            if (overlay->api == Api::Dx11) {
                overlay->renderer = MakeDx11Renderer(swapchain);
            } else {
                const auto candidate = std::find_if(overlay->candidates.begin(),
                    overlay->candidates.end(), [&](const Candidate& item) {
                        return item.identity == identity;
                    });
                if (candidate != overlay->candidates.end() && candidate->queue)
                    overlay->renderer = MakeDx12Renderer(swapchain, candidate->queue.Get());
                else
                    Log(L"No captured real D3D12 present queue; drawing disabled (no fake queue).");
            }
            if (!overlay->renderer)
                Log(L"Renderer initialization failed; present telemetry remains available.");
        }

        if (overlay->renderer) {
            const bool drawn = overlay->renderer->Draw(swapchain, control,
                overlay->session.Snapshot(), ticket.timestamp);
            if (!drawn && !overlay->renderer_skip_reported) {
                Log(L"Primary renderer skipped a HUD draw; present telemetry is not proof of visible HUD pixels.");
                overlay->renderer_skip_reported = true;
            }
            if (overlay->renderer->Width() && overlay->renderer->Height()) {
                ticket.width = overlay->renderer->Width();
                ticket.height = overlay->renderer->Height();
            }
        }
        ticket.cpu_us = static_cast<double>(QpcNow() - ticket.timestamp) * 1000000.0 /
                        static_cast<double>(QpcFrequency());
    } catch (...) {
        // An exception between NewFrame and Render may leave an open ImGui
        // frame. Do not enter that context again and risk an assertion in game.
        if (overlay) overlay->drawing_disabled.store(true, std::memory_order_relaxed);
        Log(L"HUD frame failed; forwarding the original Present unchanged.");
    }
    return ticket;
}

void AfterPresent(const PresentTicket& ticket, HRESULT result) noexcept {
    if (!ticket.primary || !overlay || FAILED(result) || result == DXGI_STATUS_OCCLUDED)
        return;
    try {
        overlay->session.ObservePresent(ticket.timestamp, ticket.api, ticket.width,
                                        ticket.height, ticket.cpu_us);
    } catch (...) {
        Log(L"Could not publish present telemetry.");
    }
}

void BeforeResize(IDXGISwapChain* swapchain) noexcept {
    try {
        if (!overlay)
            return;
        std::lock_guard<std::mutex> lock(overlay->mutex);
        if (Identity(swapchain) != overlay->primary || !overlay->renderer)
            return;
        if (!overlay->renderer->BeforeResize())
            Log(L"HUD fence did not retire during resize; resources retained for GPU safety.");
    } catch (...) {
        Log(L"HUD resize preparation failed; forwarding ResizeBuffers unchanged.");
    }
}

void CaptureResizeQueues(IDXGISwapChain* swapchain, UINT count,
                         IUnknown* const* queues) noexcept {
    try {
        if (!overlay || !queues || count == 0)
            return;
        ComPtr<ID3D12CommandQueue> queue = QueueFrom(queues[0]);
        bool supported = !!queue;
        for (UINT index = 1; index < count; ++index) {
            ComPtr<ID3D12CommandQueue> other = QueueFrom(queues[index]);
            if (!other || Identity(other.Get()) != Identity(queue.Get())) {
                supported = false;
                break;
            }
        }
        if (!supported) {
            queue.Reset();
            Log(L"Unsupported ResizeBuffers1 queues; D3D12 drawing disabled, not guessed.");
        }
        std::lock_guard<std::mutex> lock(overlay->mutex);
        const IUnknown* identity = Identity(swapchain);
        for (auto& candidate : overlay->candidates) {
            if (candidate.identity != identity)
                continue;
            if (overlay->primary == identity &&
                Identity(candidate.queue.Get()) != Identity(queue.Get())) {
                // BeforeResize has already retired the old queue's HUD commands.
                if (overlay->renderer && !overlay->renderer->BeforeResize())
                    return;
                overlay->renderer.reset();
                overlay->renderer_attempted = false;
            }
            candidate.queue = std::move(queue);
            return;
        }
    } catch (...) {
        Log(L"Could not capture ResizeBuffers1 present queues.");
    }
}

} // namespace framescope::dxgi
