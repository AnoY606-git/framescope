// SPDX-License-Identifier: GPL-3.0-only
#include "framescope/hud_core.hpp"

#include <imgui.h>

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void Check(bool value, const char* description) {
    if (!value) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void RenderFixture(framescope::ControlData control, const framescope::FrameData& frame,
                   float width, float height, bool expected_visible) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    framescope::DrawHud(control, frame, width, height);
    ImGui::Render();
    auto* data = ImGui::GetDrawData();
    Check(data != nullptr && data->Valid, "layout produces a valid CPU draw list");
    Check(expected_visible ? data->TotalVtxCount > 0 : data->TotalVtxCount == 0,
          expected_visible ? "enabled HUD has visible geometry" : "disabled HUD has no geometry");
    for (int list = 0; list < data->CmdListsCount; ++list) {
        const auto* commands = data->CmdLists[list];
        for (const auto& vertex : commands->VtxBuffer) {
            Check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y),
                  "layout never generates nonfinite coordinates");
        }
    }
}
} // namespace

int main() {
    using namespace framescope;
    auto* context = ImGui::CreateContext();
    ConfigureHudFonts();
    auto& io = ImGui::GetIO();
    const auto font_count = io.Fonts->Fonts.Size;
    Check(font_count >= 4, "all four font roles have a system-font or built-in fallback");
    ConfigureHudFonts();
    Check(io.Fonts->Fonts.Size == font_count, "font configuration is idempotent");
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Check(pixels != nullptr && width > 0 && height > 0, "fixed font atlas builds without a GPU");
    io.Fonts->SetTexID(1);

    // Только явные fixtures для layout. Рабочий монитор никогда не получает эти данные.
    ControlData control{};
    control.flags = Enabled | ShowFps | ShowFrametime | ShowCpu | ShowGpu | ShowVram |
                    ShowRam | ShowLows | ShowTemperatures | Graph;
    control.scale = 1.0f;
    control.opacity = 0.88f;
    control.margin_x = control.margin_y = 16;
    control.text_color = 0xffeaf1ff;
    control.accent_color = 0xff5eead4;
    control.background_color = 0xff111827;
    control.available_mask = Cpu | CpuMax | Gpu | Ram | Vram | GpuTemperature;
    control.cpu_usage = 0; // Настоящий ноль не должен превращаться в N/A.
    control.cpu_max = 40;
    control.gpu_usage = 73;
    control.ram_used_gib = 16;
    control.ram_total_gib = 32;
    control.vram_used_gib = 4;
    control.vram_total_gib = 12;
    control.gpu_temperature = 57;
    FrameData frame{};
    frame.present_count = 2000;
    frame.fps = 144;
    frame.frametime_ms = 6.94;
    frame.low_1 = 110;
    frame.low_01 = 90;
    frame.flags = HasFps | HasLow1 | HasLow01;
    frame.api = static_cast<unsigned>(Api::Vulkan);
    frame.graph_count = 120;
    for (auto& value : frame.graph) { value = 6.94f; }
    frame.graph[50] = 80;

    RenderFixture(control, frame, 1280, 720, true);
    control.flags |= Horizontal;
    RenderFixture(control, frame, 1280, 720, true);
    control.scale = 2.5f;
    control.corner = 3;
    RenderFixture(control, frame, 640, 480, true);
    control.flags &= ~Enabled;
    RenderFixture(control, frame, 640, 480, false);
    ImGui::DestroyContext(context);
    if (failures == 0) { std::cout << "HUD layout/font checks passed (CPU geometry only)\n"; }
    return failures == 0 ? 0 : 1;
}
