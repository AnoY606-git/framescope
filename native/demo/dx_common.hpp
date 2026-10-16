// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "win_util.hpp"
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace framescope::demo {
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

class ShaderCompiler {
public:
    ShaderCompiler();
    ComPtr<ID3DBlob> Compile(const char* entry, const char* profile);
private:
    SystemLibrary library_;
    pD3DCompile compile_ = nullptr;
};

bool SupportsTearing(IDXGIFactory2* factory) noexcept;
void RequestDxgiFullscreen(IDXGISwapChain* swapchain, WindowMode mode);
bool DxgiFullscreen(IDXGISwapChain* swapchain) noexcept;

} // namespace framescope::demo
