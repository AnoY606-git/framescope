// SPDX-License-Identifier: GPL-3.0-only
#include "demo.hpp"
#include "dx_common.hpp"
#include <d3d12.h>

#include <array>
#include <cstring>
#include <iostream>

namespace framescope::demo {
namespace {
constexpr unsigned kBufferCount = 3;

class Dx12Renderer final : public Renderer {
public:
    ~Dx12Renderer() override {
        // Destructors must not throw, including after partial initialization or
        // device removal. A bounded wait avoids an unattended CI hang.
        if (queue_ && fence_ && event_.Get()) {
            const auto value = nextFence_++;
            if (SUCCEEDED(queue_->Signal(fence_.Get(), value)) &&
                SUCCEEDED(fence_->SetEventOnCompletion(value, event_.Get())))
                WaitForSingleObject(event_.Get(), 5000);
        }
        if (swapchain_ && DxgiFullscreen(swapchain_.Get()))
            swapchain_->SetFullscreenState(FALSE, nullptr);
        for (auto& frame : frames_)
            if (frame.vertices && frame.mapped) frame.vertices->Unmap(0, nullptr);
    }

    void Initialize(HWND window, unsigned width, unsigned height,
                    const Options& options) override {
        vsync_ = options.vsync;
#ifdef _DEBUG
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf())))) debug->EnableDebugLayer();
        else std::cerr << "DX12 optional debug layer unavailable; using hardware without it.\n";
#endif
        ComPtr<IDXGIFactory4> factory;
        CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory1");
        for (UINT index = 0; ; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT enumerated = factory->EnumAdapters1(index, adapter.GetAddressOf());
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            CheckHr(enumerated, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 description{};
            CheckHr(adapter->GetDesc1(&description), "Get adapter description");
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device_.GetAddressOf())))) break;
        }
        if (!device_) throw std::runtime_error("No hardware Direct3D 12 device is available");
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CheckHr(device_->CreateCommandQueue(&queue, IID_PPV_ARGS(queue_.GetAddressOf())), "CreateCommandQueue");
        CheckHr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence_.GetAddressOf())), "CreateFence");
        event_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event_.Get()) throw std::runtime_error("Create DX12 fence event failed");

        allowTearing_ = SupportsTearing(factory.Get());
        swapchainFlags_ = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (options.mode == WindowMode::Exclusive) swapchainFlags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width; description.Height = height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = kBufferCount;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = swapchainFlags_;
        ComPtr<IDXGISwapChain1> basicSwapchain;
        // This must be the real DIRECT command queue, not the device: injected
        // DX12 renderers discover their submission queue through this call.
        CheckHr(factory->CreateSwapChainForHwnd(queue_.Get(), window, &description,
            nullptr, nullptr, basicSwapchain.GetAddressOf()), "DX12 CreateSwapChainForHwnd");
        CheckHr(basicSwapchain.As(&swapchain_), "Query IDXGISwapChain3");
        CheckHr(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        RequestDxgiFullscreen(swapchain_.Get(), options.mode);
        if (DxgiFullscreen(swapchain_.Get())) {
            RECT client{};
            GetClientRect(window, &client);
            CheckHr(swapchain_->ResizeBuffers(kBufferCount, static_cast<UINT>(client.right),
                static_cast<UINT>(client.bottom), DXGI_FORMAT_UNKNOWN, swapchainFlags_),
                "DX12 ResizeBuffers after fullscreen transition");
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = kBufferCount;
        CheckHr(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtvHeap_.GetAddressOf())), "Create RTV heap");
        rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CreateTargets();
        CreatePipeline();
        for (auto& frame : frames_) {
            CheckHr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(frame.allocator.GetAddressOf())), "CreateCommandAllocator");
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_UPLOAD;
            properties.CreationNodeMask = properties.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = kMaxVertices * sizeof(Vertex);
            buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CheckHr(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(frame.vertices.GetAddressOf())), "Create upload vertex buffer");
            const D3D12_RANGE noRead{0, 0};
            CheckHr(frame.vertices->Map(0, &noRead, &frame.mapped), "Map DX12 vertices");
        }
        CheckHr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            frames_[0].allocator.Get(), pipeline_.Get(), IID_PPV_ARGS(commands_.GetAddressOf())), "CreateCommandList");
        CheckHr(commands_->Close(), "Close initial command list");
    }

    void Resize(unsigned width, unsigned height) override {
        if (!width || !height) return;
        WaitIdle();
        for (auto& frame : frames_) frame.image.Reset();
        CheckHr(swapchain_->ResizeBuffers(kBufferCount, width, height,
            DXGI_FORMAT_UNKNOWN, swapchainFlags_), "DX12 ResizeBuffers");
        CreateTargets();
    }

    PresentResult Render(const std::vector<Vertex>& vertices, unsigned drawCount) override {
        if (vertices.size() > kMaxVertices) throw std::runtime_error("Scene exceeds vertex buffer capacity");
        const UINT index = swapchain_->GetCurrentBackBufferIndex();
        auto& frame = frames_[index];
        WaitFence(frame.fenceValue);
        std::memcpy(frame.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
        CheckHr(frame.allocator->Reset(), "Reset DX12 allocator");
        CheckHr(commands_->Reset(frame.allocator.Get(), pipeline_.Get()), "Reset DX12 command list");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.image.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands_->ResourceBarrier(1, &barrier);
        auto target = Rtv(index);
        commands_->OMSetRenderTargets(1, &target, FALSE, nullptr);
        commands_->ClearRenderTargetView(target, kClearColor, 0, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        commands_->RSSetViewports(1, &viewport);
        commands_->RSSetScissorRects(1, &scissor);
        commands_->SetGraphicsRootSignature(root_.Get());
        commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW view{frame.vertices->GetGPUVirtualAddress(),
            static_cast<UINT>(vertices.size() * sizeof(Vertex)), sizeof(Vertex)};
        commands_->IASetVertexBuffers(0, 1, &view);
        for (unsigned draw = 0; draw < drawCount; ++draw)
            commands_->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commands_->ResourceBarrier(1, &barrier);
        CheckHr(commands_->Close(), "Close DX12 command list");
        ID3D12CommandList* lists[] = {commands_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const UINT flags = !vsync_ && allowTearing_ && !FullscreenAcquired() ? DXGI_PRESENT_ALLOW_TEARING : 0;
        const HRESULT presented = swapchain_->Present(vsync_ ? 1 : 0, flags);
        // Fence after Present also covers commands submitted by an injected HUD
        // on this queue. Never reset an in-flight allocator or upload buffer.
        frame.fenceValue = nextFence_++;
        CheckHr(queue_->Signal(fence_.Get(), frame.fenceValue), "Signal DX12 frame fence");
        if (presented == DXGI_STATUS_OCCLUDED) { Sleep(16); return PresentResult::Skipped; }
        CheckHr(presented, "DX12 Present");
        return presented == S_OK ? PresentResult::Presented : PresentResult::Skipped;
    }

    unsigned Width() const noexcept override { return width_; }
    unsigned Height() const noexcept override { return height_; }
    bool FullscreenAcquired() const noexcept override { return DxgiFullscreen(swapchain_.Get()); }

private:
    struct Frame {
        ComPtr<ID3D12Resource> image;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> vertices;
        void* mapped = nullptr;
        std::uint64_t fenceValue = 0;
    };
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(unsigned index) const {
        auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtvStride_;
        return handle;
    }
    void CreateTargets() {
        for (unsigned i = 0; i < kBufferCount; ++i) {
            CheckHr(swapchain_->GetBuffer(i, IID_PPV_ARGS(frames_[i].image.GetAddressOf())), "DX12 GetBuffer");
            device_->CreateRenderTargetView(frames_[i].image.Get(), nullptr, Rtv(i));
            const auto description = frames_[i].image->GetDesc();
            width_ = static_cast<unsigned>(description.Width); height_ = description.Height;
        }
    }
    void WaitFence(std::uint64_t value) {
        if (!value) return;
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) {
            CheckHr(device_->GetDeviceRemovedReason(), "DX12 fence device removed");
            throw std::runtime_error("DX12 fence reported device removal");
        }
        if (completed >= value) return;
        CheckHr(fence_->SetEventOnCompletion(value, event_.Get()), "Set DX12 fence event");
        if (WaitForSingleObject(event_.Get(), 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("DX12 fence wait failed or timed out after 30 seconds");
    }
    void WaitIdle() {
        const auto value = nextFence_++;
        CheckHr(queue_->Signal(fence_.Get(), value), "Signal DX12 idle fence");
        WaitFence(value);
    }
    void CreatePipeline() {
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized, errors;
        CheckHr(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(), errors.GetAddressOf()), "Serialize root signature");
        CheckHr(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(root_.GetAddressOf())), "CreateRootSignature");
        ShaderCompiler compiler;
        const auto vertex = compiler.Compile("VSMain", "vs_5_0");
        const auto pixel = compiler.Compile("PSMain", "ps_5_0");
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        pipeline.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pipeline.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        pipeline.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pipeline.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pipeline.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pipeline.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pipeline.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.InputLayout = {layout, 2};
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        CheckHr(device_->CreateGraphicsPipelineState(&pipeline,
            IID_PPV_ARGS(pipeline_.GetAddressOf())), "Create DX12 scene pipeline");
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    std::array<Frame, kBufferCount> frames_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence_;
    UniqueHandle event_;
    std::uint64_t nextFence_ = 1;
    unsigned width_ = 0, height_ = 0;
    UINT rtvStride_ = 0, swapchainFlags_ = 0;
    bool vsync_ = true, allowTearing_ = false;
};
} // namespace
std::unique_ptr<Renderer> MakeDx12Renderer() { return std::make_unique<Dx12Renderer>(); }
} // namespace framescope::demo
