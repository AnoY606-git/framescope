// SPDX-License-Identifier: GPL-3.0-only
#include "overlay.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace framescope::dxgi {
namespace {

bool SameObject(IUnknown* left, IUnknown* right) {
    ComPtr<IUnknown> a, b;
    return left && right && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(a.GetAddressOf()))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(b.GetAddressOf()))) && a.Get() == b.Get();
}

bool SupportedFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

struct BufferFrame {
    ComPtr<ID3D12Resource> backbuffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    std::uint64_t fence_value = 0;
};

class Dx12Renderer final : public Renderer {
public:
    bool Initialize(IDXGISwapChain* swapchain, ID3D12CommandQueue* real_queue) {
        if (!real_queue || real_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return false;
        queue_ = real_queue; // The exact queue passed to DXGI creation, never a dummy queue.
        HRESULT result = swapchain->GetDevice(IID_PPV_ARGS(device_.GetAddressOf()));
        ComPtr<ID3D12Device> queue_device;
        if (FAILED(result) || FAILED(queue_->GetDevice(IID_PPV_ARGS(queue_device.GetAddressOf()))) ||
            !SameObject(device_.Get(), queue_device.Get())) {
            Log(L"D3D12 swapchain and captured queue devices do not match.");
            return false;
        }
        if (device_->GetNodeCount() != 1) {
            Log(L"Linked-node D3D12 devices are outside the single-node preview.");
            return false;
        }
        ComPtr<IDXGISwapChain3> modern;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(modern.GetAddressOf())))) {
            Log(L"D3D12 HUD requires IDXGISwapChain3.");
            return false;
        }
        result = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
        if (FAILED(result)) {
            Log(L"Cannot create HUD fence.", result);
            return false;
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_)
            return false;
        ImGuiContext* previous = ImGui::GetCurrentContext();
        imgui_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(previous);
        if (!imgui_)
            return false;
        ContextScope scope(imgui_);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
        io.BackendPlatformName = "framescope_no_input";
        ConfigureHudFonts();
        return true;
    }

    ~Dx12Renderer() override {
        if (!WaitForOwnWork()) {
            // Do not release objects still referenced by GPU commands. This path
            // is an error-only process-lifetime quarantine, not an unsafe unload.
            for (auto& frame : frames_) {
                frame.backbuffer.Detach();
                frame.allocator.Detach();
                frame.list.Detach();
            }
            rtv_heap_.Detach();
            srv_heap_.Detach();
            fence_.Detach();
            queue_.Detach();
            device_.Detach();
            Log(L"Quarantined unfinished D3D12 HUD resources until process exit.");
            return;
        }
        ShutdownBackend();
        if (imgui_)
            ImGui::DestroyContext(imgui_);
        if (fence_event_)
            CloseHandle(fence_event_);
    }

    bool Draw(IDXGISwapChain* swapchain, const ControlData& control, const FrameData& frame,
              std::uint64_t timestamp) override {
        if (disabled_ || FAILED(device_->GetDeviceRemovedReason()))
            return false;
        if (!initialized_ && !CreateResources(swapchain)) {
            disabled_ = true;
            Log(L"D3D12 HUD resource initialization failed; drawing skipped until resize.");
            return false;
        }
        ComPtr<IDXGISwapChain3> modern;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(modern.GetAddressOf()))))
            return false;
        const UINT index = modern->GetCurrentBackBufferIndex();
        if (index >= frames_.size())
            return false;
        BufferFrame& current = frames_[index];
        const std::size_t backend_slot = render_calls_ % frames_.size();
        const std::uint64_t completed = fence_->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            disabled_ = true;
            return false;
        }
        // ImGui rotates upload buffers by render-call count, not swapchain index.
        // Fence BOTH rings, including when the HUD was skipped on previous frames.
        if (completed < current.fence_value || completed < backend_fences_[backend_slot])
            return false;

        HRESULT result = current.allocator->Reset();
        if (FAILED(result))
            return Fault(L"Cannot reset HUD command allocator.", result);
        result = current.list->Reset(current.allocator.Get(), nullptr);
        if (FAILED(result))
            return Fault(L"Cannot reset HUD command list.", result);

        ContextScope scope(imgui_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width_), static_cast<float>(height_));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = last_timestamp_ && timestamp > last_timestamp_
            ? std::clamp(static_cast<float>(static_cast<double>(timestamp - last_timestamp_) /
                         static_cast<double>(QpcFrequency())), 0.000001f, 1.0f)
            : 1.0f / 60.0f;
        last_timestamp_ = timestamp;
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        DrawHud(control, frame, static_cast<float>(width_), static_cast<float>(height_));
        ImGui::Render();
        if (ImGui::GetDrawData()->TotalVtxCount == 0) {
            current.list->Close();
            return true;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = current.backbuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        current.list->ResourceBarrier(1, &barrier);
        current.list->OMSetRenderTargets(1, &current.target, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        current.list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), current.list.Get());
        ++render_calls_; // Official backend advances even if its internal allocation failed.
        backend_fences_[backend_slot] = 0;
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        current.list->ResourceBarrier(1, &barrier);
        result = current.list->Close();
        if (FAILED(result))
            return Fault(L"Cannot close HUD command list.", result);
        ID3D12CommandList* lists[] = {current.list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const std::uint64_t value = ++next_fence_;
        result = queue_->Signal(fence_.Get(), value);
        if (FAILED(result)) {
            untracked_work_ = true;
            return Fault(L"Cannot signal HUD submission fence.", result);
        }
        submitted_fence_ = value;
        current.fence_value = value;
        backend_fences_[backend_slot] = value;
        return true;
    }

    bool BeforeResize() override {
        if (!WaitForOwnWork())
            return false;
        ShutdownBackend();
        frames_.clear(); // Release EVERY backbuffer reference before original ResizeBuffers.
        backend_fences_.clear();
        rtv_heap_.Reset();
        srv_heap_.Reset();
        descriptors_.fill(false);
        render_calls_ = 0;
        width_ = height_ = 0;
        disabled_ = false;
        return true;
    }

    std::uint32_t Width() const override { return width_; }
    std::uint32_t Height() const override { return height_; }

private:
    static constexpr UINT kSrvCount = 16;

    bool Fault(const wchar_t* message, HRESULT result) {
        disabled_ = true;
        Log(message, result);
        return false;
    }

    static void AllocateSrv(ImGui_ImplDX12_InitInfo* info,
                            D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                            D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        auto& self = *static_cast<Dx12Renderer*>(info->UserData);
        *cpu = {};
        *gpu = {};
        for (UINT index = 0; index < kSrvCount; ++index) {
            if (self.descriptors_[index])
                continue;
            self.descriptors_[index] = true;
            *cpu = self.srv_heap_->GetCPUDescriptorHandleForHeapStart();
            *gpu = self.srv_heap_->GetGPUDescriptorHandleForHeapStart();
            cpu->ptr += static_cast<SIZE_T>(index) * self.srv_stride_;
            gpu->ptr += static_cast<UINT64>(index) * self.srv_stride_;
            return;
        }
        Log(L"HUD SRV descriptor allocator exhausted.");
    }

    static void FreeSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                        D3D12_GPU_DESCRIPTOR_HANDLE) {
        auto& self = *static_cast<Dx12Renderer*>(info->UserData);
        if (!self.srv_heap_ || !cpu.ptr || !self.srv_stride_)
            return;
        const SIZE_T start = self.srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr;
        if (cpu.ptr < start)
            return;
        const SIZE_T index = (cpu.ptr - start) / self.srv_stride_;
        if (index < kSrvCount)
            self.descriptors_[index] = false;
    }

    bool CreateResources(IDXGISwapChain* swapchain) {
        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT result = swapchain->GetDesc(&desc);
        if (FAILED(result) || desc.BufferCount < 2 || desc.BufferCount > 16 ||
            desc.SampleDesc.Count != 1 || !SupportedFormat(desc.BufferDesc.Format)) {
            Log(L"D3D12 preview needs a 2..16 buffer, non-MSAA SDR swapchain.");
            return false;
        }
        frames_.resize(desc.BufferCount);
        backend_fences_.assign(desc.BufferCount, 0);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = desc.BufferCount;
        result = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv_heap_.GetAddressOf()));
        if (FAILED(result))
            return Fault(L"Cannot create HUD RTV heap.", result);
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kSrvCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(srv_heap_.GetAddressOf()));
        if (FAILED(result))
            return Fault(L"Cannot create HUD SRV heap.", result);
        srv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT rtv_stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE target = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < desc.BufferCount; ++index) {
            BufferFrame& frame = frames_[index];
            result = swapchain->GetBuffer(index, IID_PPV_ARGS(frame.backbuffer.GetAddressOf()));
            if (FAILED(result))
                return Fault(L"Cannot get D3D12 backbuffer.", result);
            const D3D12_RESOURCE_DESC resource = frame.backbuffer->GetDesc();
            width_ = static_cast<std::uint32_t>(resource.Width);
            height_ = resource.Height;
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = desc.BufferDesc.Format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(frame.backbuffer.Get(), &view, target);
            frame.target = target;
            target.ptr += rtv_stride;
            result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(frame.allocator.GetAddressOf()));
            if (FAILED(result))
                return Fault(L"Cannot create per-buffer HUD allocator.", result);
            result = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                frame.allocator.Get(), nullptr, IID_PPV_ARGS(frame.list.GetAddressOf()));
            if (FAILED(result))
                return Fault(L"Cannot create per-buffer HUD command list.", result);
            result = frame.list->Close();
            if (FAILED(result))
                return Fault(L"Cannot initialize HUD command list.", result);
        }

        ContextScope scope(imgui_);
        ImGui_ImplDX12_InitInfo info;
        info.Device = device_.Get();
        info.CommandQueue = queue_.Get();
        info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
        info.RTVFormat = desc.BufferDesc.Format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.UserData = this;
        info.SrvDescriptorHeap = srv_heap_.Get();
        info.SrvDescriptorAllocFn = AllocateSrv;
        info.SrvDescriptorFreeFn = FreeSrv;
        initialized_ = ImGui_ImplDX12_Init(&info);
        if (!initialized_)
            return false;
        // Pinned 1.91.9b performs one synchronous font upload here on the REAL
        // queue. There is no per-frame device/fence wait after this setup step.
        return ImGui_ImplDX12_CreateDeviceObjects();
    }

    bool WaitForOwnWork() {
        if (!device_ || FAILED(device_->GetDeviceRemovedReason()))
            return true; // A removed device cannot still access these objects.
        if (untracked_work_)
            return false;
        if (!fence_ || !submitted_fence_ || fence_->GetCompletedValue() >= submitted_fence_)
            return true;
        ResetEvent(fence_event_);
        HRESULT result = fence_->SetEventOnCompletion(submitted_fence_, fence_event_);
        if (FAILED(result)) {
            Log(L"Cannot arm HUD resize fence event.", result);
            return false;
        }
        const DWORD wait = WaitForSingleObject(fence_event_, 2000);
        return wait == WAIT_OBJECT_0 && fence_->GetCompletedValue() >= submitted_fence_;
    }

    void ShutdownBackend() {
        if (imgui_ && initialized_) {
            ContextScope scope(imgui_);
            ImGui_ImplDX12_Shutdown();
            initialized_ = false;
        }
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    std::vector<BufferFrame> frames_;
    std::vector<std::uint64_t> backend_fences_;
    std::array<bool, kSrvCount> descriptors_{};
    UINT srv_stride_ = 0;
    ImGuiContext* imgui_ = nullptr;
    bool initialized_ = false;
    bool disabled_ = false;
    bool untracked_work_ = false;
    std::uint32_t width_ = 0, height_ = 0;
    std::uint64_t last_timestamp_ = 0;
    std::uint64_t render_calls_ = 0;
    std::uint64_t next_fence_ = 0;
    std::uint64_t submitted_fence_ = 0;
};

} // namespace

std::unique_ptr<Renderer> MakeDx12Renderer(IDXGISwapChain* swapchain,
                                         ID3D12CommandQueue* real_queue) {
    auto renderer = std::make_unique<Dx12Renderer>();
    if (!renderer->Initialize(swapchain, real_queue))
        return {};
    return renderer;
}

} // namespace framescope::dxgi
