// SPDX-License-Identifier: GPL-3.0-only
#include "demo.hpp"
#include "dx_common.hpp"
#include <d3d11.h>

#include <cstring>
#include <iostream>

namespace framescope::demo {
namespace {
class Dx11Renderer final : public Renderer {
public:
    ~Dx11Renderer() override {
        if (context_) { context_->ClearState(); context_->Flush(); }
        if (swapchain_ && DxgiFullscreen(swapchain_.Get()))
            swapchain_->SetFullscreenState(FALSE, nullptr);
    }

    void Initialize(HWND window, unsigned width, unsigned height,
                    const Options& options) override {
        vsync_ = options.vsync;
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const auto create = [&](UINT requestedFlags) {
            device_.Reset(); context_.Reset();
            HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                nullptr, requestedFlags, levels, 2, D3D11_SDK_VERSION,
                device_.GetAddressOf(), nullptr, context_.GetAddressOf());
            if (result == E_INVALIDARG) {
                device_.Reset(); context_.Reset();
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                    nullptr, requestedFlags, levels + 1, 1, D3D11_SDK_VERSION,
                    device_.GetAddressOf(), nullptr, context_.GetAddressOf());
            }
            return result;
        };
        HRESULT result = create(flags);
        if (FAILED(result) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            std::cerr << "DX11 debug device unavailable; retrying the real hardware device without the optional debug layer.\n";
            result = create(flags & ~D3D11_CREATE_DEVICE_DEBUG);
        }
        CheckHr(result, "D3D11CreateDevice (hardware)");

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        CheckHr(device_.As(&dxgiDevice), "Query DXGI device");
        CheckHr(dxgiDevice->GetAdapter(adapter.GetAddressOf()), "GetAdapter");
        CheckHr(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())), "Get DXGI factory");
        allowTearing_ = SupportsTearing(factory.Get());
        swapchainFlags_ = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (options.mode == WindowMode::Exclusive) swapchainFlags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = swapchainFlags_;
        CheckHr(factory->CreateSwapChainForHwnd(device_.Get(), window, &description,
            nullptr, nullptr, swapchain_.GetAddressOf()), "DX11 CreateSwapChainForHwnd");
        CheckHr(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        RequestDxgiFullscreen(swapchain_.Get(), options.mode);
        if (DxgiFullscreen(swapchain_.Get())) {
            RECT client{};
            GetClientRect(window, &client);
            CheckHr(swapchain_->ResizeBuffers(0, static_cast<UINT>(client.right),
                static_cast<UINT>(client.bottom), DXGI_FORMAT_UNKNOWN, swapchainFlags_),
                "DX11 ResizeBuffers after fullscreen transition");
        }
        CreateTarget();

        ShaderCompiler compiler;
        const auto vertexCode = compiler.Compile("VSMain", "vs_5_0");
        const auto pixelCode = compiler.Compile("PSMain", "ps_5_0");
        CheckHr(device_->CreateVertexShader(vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), nullptr, vertexShader_.GetAddressOf()), "CreateVertexShader");
        CheckHr(device_->CreatePixelShader(pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(), nullptr, pixelShader_.GetAddressOf()), "CreatePixelShader");
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        CheckHr(device_->CreateInputLayout(layout, 2, vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), inputLayout_.GetAddressOf()), "CreateInputLayout");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(kMaxVertices * sizeof(Vertex));
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CheckHr(device_->CreateBuffer(&buffer, nullptr, vertexBuffer_.GetAddressOf()), "Create vertex buffer");
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        CheckHr(device_->CreateRasterizerState(&rasterizer, rasterizer_.GetAddressOf()), "CreateRasterizerState");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        CheckHr(device_->CreateDepthStencilState(&depth, depth_.GetAddressOf()), "CreateDepthStencilState");
    }

    void Resize(unsigned width, unsigned height) override {
        if (!width || !height) return;
        context_->ClearState();
        target_.Reset();
        context_->Flush();
        CheckHr(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
            swapchainFlags_), "DX11 ResizeBuffers");
        CreateTarget();
    }

    PresentResult Render(const std::vector<Vertex>& vertices, unsigned drawCount) override {
        if (vertices.size() > kMaxVertices) throw std::runtime_error("Scene exceeds vertex buffer capacity");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHr(context_->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "DX11 Map vertices");
        std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
        context_->Unmap(vertexBuffer_.Get(), 0);
        auto* target = target_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context_->OMSetDepthStencilState(depth_.Get(), 0);
        context_->ClearRenderTargetView(target, kClearColor);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->RSSetState(rasterizer_.Get());
        const UINT stride = sizeof(Vertex), offset = 0;
        auto* buffer = vertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        for (unsigned draw = 0; draw < drawCount; ++draw)
            context_->Draw(static_cast<UINT>(vertices.size()), 0);
        const UINT presentFlags = !vsync_ && allowTearing_ && !FullscreenAcquired()
            ? DXGI_PRESENT_ALLOW_TEARING : 0;
        const HRESULT result = swapchain_->Present(vsync_ ? 1 : 0, presentFlags);
        if (result == DXGI_STATUS_OCCLUDED) { Sleep(16); return PresentResult::Skipped; }
        CheckHr(result, "DX11 Present");
        return result == S_OK ? PresentResult::Presented : PresentResult::Skipped;
    }

    unsigned Width() const noexcept override { return width_; }
    unsigned Height() const noexcept override { return height_; }
    bool FullscreenAcquired() const noexcept override { return DxgiFullscreen(swapchain_.Get()); }

private:
    void CreateTarget() {
        ComPtr<ID3D11Texture2D> image;
        CheckHr(swapchain_->GetBuffer(0, IID_PPV_ARGS(image.GetAddressOf())), "DX11 GetBuffer");
        D3D11_TEXTURE2D_DESC description{};
        image->GetDesc(&description);
        width_ = description.Width; height_ = description.Height;
        CheckHr(device_->CreateRenderTargetView(image.Get(), nullptr, target_.GetAddressOf()), "CreateRenderTargetView");
    }
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11DepthStencilState> depth_;
    unsigned width_ = 0, height_ = 0;
    UINT swapchainFlags_ = 0;
    bool vsync_ = true, allowTearing_ = false;
};
} // namespace
std::unique_ptr<Renderer> MakeDx11Renderer() { return std::make_unique<Dx11Renderer>(); }
} // namespace framescope::demo
