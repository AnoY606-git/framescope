// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "framescope/hud_core.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

struct ImGuiContext;

namespace framescope::dxgi {

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

void Log(const wchar_t* message, HRESULT result = S_OK) noexcept;

// Restores an application's current context as well as keeping the HUD's context private.
class ContextScope {
public:
    explicit ContextScope(ImGuiContext* context);
    ~ContextScope();
private:
    ImGuiContext* previous_;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual bool Draw(IDXGISwapChain* swapchain, const ControlData& control,
                      const FrameData& frame, std::uint64_t timestamp) = 0;
    // A fence wait is permitted during ResizeBuffers only, never in the frame path.
    virtual bool BeforeResize() = 0;
    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
};

std::unique_ptr<Renderer> MakeDx11Renderer(IDXGISwapChain* swapchain);
std::unique_ptr<Renderer> MakeDx12Renderer(IDXGISwapChain* swapchain,
                                         ID3D12CommandQueue* real_queue);

struct PresentTicket {
    bool primary = false;
    Api api = Api::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestamp = 0;
    double cpu_us = 0;
};

bool InitializeOverlay();
void CaptureSwapchain(IDXGISwapChain* swapchain, IUnknown* creation_device, HWND window);
PresentTicket BeforePresent(IDXGISwapChain* swapchain) noexcept;
void AfterPresent(const PresentTicket& ticket, HRESULT result) noexcept;
void BeforeResize(IDXGISwapChain* swapchain) noexcept;
void CaptureResizeQueues(IDXGISwapChain* swapchain, UINT count,
                         IUnknown* const* queues) noexcept;

bool InstallHooks();
DWORD WINAPI InitializeWorker(void* module);

} // namespace framescope::dxgi
