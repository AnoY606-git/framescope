// SPDX-License-Identifier: GPL-3.0-only
#include "overlay.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include <algorithm>
#include <array>

namespace framescope::dxgi {
namespace {

// The official backend restores the graphics state it changes, except output
// targets and the infrequently used HS/DS/CS shaders. Restore those here too.
class SupplementalState {
public:
    explicit SupplementalState(ID3D11DeviceContext* context) : context_(context) {
        context_->OMGetRenderTargets(static_cast<UINT>(targets_.size()), targets_.data(), &depth_);
        context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
            static_cast<UINT>(uavs_.size()), uavs_.data());
        hs_count_ = ds_count_ = cs_count_ = 256;
        context_->HSGetShader(&hs_, hs_instances_.data(), &hs_count_);
        context_->DSGetShader(&ds_, ds_instances_.data(), &ds_count_);
        context_->CSGetShader(&cs_, cs_instances_.data(), &cs_count_);
    }

    ~SupplementalState() {
        UINT target_count = 0;
        for (UINT index = 0; index < targets_.size(); ++index)
            if (targets_[index])
                target_count = index + 1;
        std::array<UINT, D3D11_PS_CS_UAV_REGISTER_COUNT> counts;
        counts.fill(UINT(-1)); // Preserve append/consume counters.
        context_->OMSetRenderTargetsAndUnorderedAccessViews(target_count, targets_.data(), depth_,
            target_count, static_cast<UINT>(uavs_.size()) - target_count,
            uavs_.data() + target_count, counts.data() + target_count);
        context_->HSSetShader(hs_, hs_instances_.data(), hs_count_);
        context_->DSSetShader(ds_, ds_instances_.data(), ds_count_);
        context_->CSSetShader(cs_, cs_instances_.data(), cs_count_);
        ReleaseAll(targets_);
        ReleaseAll(uavs_);
        ReleaseAll(hs_instances_);
        ReleaseAll(ds_instances_);
        ReleaseAll(cs_instances_);
        if (depth_) depth_->Release();
        if (hs_) hs_->Release();
        if (ds_) ds_->Release();
        if (cs_) cs_->Release();
    }

private:
    template <class T, std::size_t Count>
    static void ReleaseAll(std::array<T*, Count>& entries) {
        for (T* entry : entries)
            if (entry) entry->Release();
    }

    ID3D11DeviceContext* context_;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets_{};
    std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs_{};
    ID3D11DepthStencilView* depth_ = nullptr;
    ID3D11HullShader* hs_ = nullptr;
    ID3D11DomainShader* ds_ = nullptr;
    ID3D11ComputeShader* cs_ = nullptr;
    std::array<ID3D11ClassInstance*, 256> hs_instances_{};
    std::array<ID3D11ClassInstance*, 256> ds_instances_{};
    std::array<ID3D11ClassInstance*, 256> cs_instances_{};
    UINT hs_count_ = 0, ds_count_ = 0, cs_count_ = 0;
};

class Dx11Renderer final : public Renderer {
public:
    bool Initialize(IDXGISwapChain* swapchain) {
        HRESULT result = swapchain->GetDevice(IID_PPV_ARGS(device_.GetAddressOf()));
        if (FAILED(result)) {
            Log(L"Cannot get D3D11 device.", result);
            return false;
        }
        device_->GetImmediateContext(context_.GetAddressOf());
        if (!context_)
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
        initialized_ = ImGui_ImplDX11_Init(device_.Get(), context_.Get());
        return initialized_;
    }

    ~Dx11Renderer() override {
        if (!imgui_)
            return;
        {
            ContextScope scope(imgui_);
            if (initialized_)
                ImGui_ImplDX11_Shutdown();
        }
        ImGui::DestroyContext(imgui_);
    }

    bool Draw(IDXGISwapChain* swapchain, const ControlData& control, const FrameData& frame,
              std::uint64_t timestamp) override {
        if (!initialized_ || failed_ || FAILED(device_->GetDeviceRemovedReason())) {
            LogSkipOnce(1u << 0, L"DX11 HUD draw skipped: renderer/device unavailable.");
            return false;
        }
        // The official DX11 backend maps its dynamic vertex/index buffers with
        // WRITE_DISCARD. The D3D11 runtime renames busy storage; CPU reuse does
        // NOT require the previous HUD's GPU commands to have completed. A
        // global EVENT/DONOTFLUSH gate here skips normally pipelined frames and
        // produces a flickering/absent HUD on flip-discard swapchains. Keep the
        // official asynchronous upload path: no fence wait and no forced Flush.
        if (!target_) {
            ComPtr<ID3D11Texture2D> backbuffer;
            HRESULT result = swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
            if (FAILED(result)) {
                LogSkipOnce(1u << 1, L"DX11 HUD draw skipped: cannot get the swapchain backbuffer.");
                return false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            backbuffer->GetDesc(&desc);
            width_ = desc.Width;
            height_ = desc.Height;
            result = device_->CreateRenderTargetView(backbuffer.Get(), nullptr, target_.GetAddressOf());
            if (FAILED(result)) {
                Log(L"Cannot create D3D11 HUD render target.", result);
                failed_ = true;
                return false;
            }
        }

        ContextScope scope(imgui_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width_), static_cast<float>(height_));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = last_timestamp_ && timestamp > last_timestamp_
            ? std::clamp(static_cast<float>(static_cast<double>(timestamp - last_timestamp_) /
                         static_cast<double>(QpcFrequency())), 0.000001f, 1.0f)
            : 1.0f / 60.0f;
        last_timestamp_ = timestamp;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        DrawHud(control, frame, static_cast<float>(width_), static_cast<float>(height_));
        ImGui::Render();
        if (ImGui::GetDrawData()->TotalVtxCount == 0 || ImGui::GetDrawData()->TotalIdxCount == 0) {
            LogSkipOnce(1u << 2, L"DX11 HUD draw skipped: ImGui produced no indexed vertices.");
            return false;
        }
        SupplementalState backup(context_.Get());
        ID3D11RenderTargetView* target = target_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!logged_submission_) {
            wchar_t message[256]{};
            _snwprintf_s(message, _countof(message), _TRUNCATE,
                L"DX11 positive-vertex HUD draw submitted: vertices=%d, indices=%d, target=%ux%u.",
                ImGui::GetDrawData()->TotalVtxCount, ImGui::GetDrawData()->TotalIdxCount,
                width_, height_);
            Log(message);
            logged_submission_ = true;
        }
        return true;
    }

    bool BeforeResize() override {
        target_.Reset();
        width_ = height_ = 0;
        failed_ = false;
        return true;
    }

    std::uint32_t Width() const override { return width_; }
    std::uint32_t Height() const override { return height_; }

private:
    void LogSkipOnce(std::uint32_t reason, const wchar_t* message) {
        if (!(logged_skips_ & reason)) {
            logged_skips_ |= reason;
            Log(message);
        }
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11RenderTargetView> target_;
    ImGuiContext* imgui_ = nullptr;
    bool initialized_ = false;
    bool failed_ = false;
    bool logged_submission_ = false;
    std::uint32_t logged_skips_ = 0;
    std::uint32_t width_ = 0, height_ = 0;
    std::uint64_t last_timestamp_ = 0;
};

} // namespace

std::unique_ptr<Renderer> MakeDx11Renderer(IDXGISwapChain* swapchain) {
    auto renderer = std::make_unique<Dx11Renderer>();
    if (!renderer->Initialize(swapchain))
        return {};
    return renderer;
}

} // namespace framescope::dxgi
