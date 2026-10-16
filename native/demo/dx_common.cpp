// SPDX-License-Identifier: GPL-3.0-only
#include "dx_common.hpp"

#include <cstring>
#include <iostream>

namespace framescope::demo {
namespace {
// Source is also checked in as shaders/scene.hlsl for inspection/regeneration.
constexpr char kSceneHlsl[] = R"hlsl(
struct Input { float3 position : POSITION; float3 color : COLOR; };
struct Output { float4 position : SV_Position; float3 color : COLOR; };
Output VSMain(Input v) {
    Output o; o.position = float4(v.position, 1.0); o.color = v.color; return o;
}
float4 PSMain(Output v) : SV_Target { return float4(v.color, 1.0); }
)hlsl";
} // namespace

ShaderCompiler::ShaderCompiler() : library_(L"d3dcompiler_47.dll") {
    compile_ = reinterpret_cast<pD3DCompile>(library_.Find("D3DCompile"));
    if (!compile_) throw std::runtime_error("System d3dcompiler_47.dll has no D3DCompile");
}

ComPtr<ID3DBlob> ShaderCompiler::Compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = compile_(kSceneHlsl, std::strlen(kSceneHlsl),
        "FrameScope original scene", nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result) && errors) {
        throw std::runtime_error(std::string("Scene shader compilation: ") +
            std::string(static_cast<const char*>(errors->GetBufferPointer()),
                        errors->GetBufferSize()));
    }
    CheckHr(result, "D3DCompile");
    return code;
}

bool SupportsTearing(IDXGIFactory2* factory) noexcept {
    ComPtr<IDXGIFactory5> newer;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(newer.GetAddressOf())))) return false;
    BOOL supported = FALSE;
    return SUCCEEDED(newer->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
        &supported, sizeof(supported))) && supported;
}

void RequestDxgiFullscreen(IDXGISwapChain* swapchain, WindowMode mode) {
    if (mode != WindowMode::Exclusive) return;
    const HRESULT result = swapchain->SetFullscreenState(TRUE, nullptr);
    if (FAILED(result)) {
        std::cerr << "Exclusive fullscreen not acquired (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(result) << std::dec
                  << "); continuing without exclusive access.\n";
    }
}

bool DxgiFullscreen(IDXGISwapChain* swapchain) noexcept {
    if (!swapchain) return false;
    BOOL fullscreen = FALSE;
    return SUCCEEDED(swapchain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen;
}
} // namespace framescope::demo
