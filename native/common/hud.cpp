// SPDX-License-Identifier: GPL-3.0-only
#include "framescope/hud_core.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace framescope {
namespace {

ImVec4 Color(std::uint32_t argb, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((argb >> 16) & 255) / 255.0f,
                  static_cast<float>((argb >> 8) & 255) / 255.0f,
                  static_cast<float>(argb & 255) / 255.0f,
                  static_cast<float>((argb >> 24) & 255) / 255.0f * alpha);
}

ImVec4 Fade(ImVec4 color, float alpha) {
    color.w *= alpha;
    return color;
}

ImVec4 Mix(ImVec4 a, ImVec4 b, float amount) {
    return ImVec4(a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount,
                  a.z + (b.z - a.z) * amount, a.w + (b.w - a.w) * amount);
}

const char* ApiName(std::uint32_t api) {
    switch (static_cast<Api>(api)) {
    case Api::Dx11: return "DX11";
    case Api::Dx12: return "DX12";
    case Api::Vulkan: return "VULKAN";
    default: return "APP";
    }
}

struct Fonts {
    ImFont* body;
    ImFont* semibold;
    ImFont* display;
    ImFont* numbers;
};

Fonts ContextFonts() {
    Fonts fonts{ImGui::GetFont(), ImGui::GetFont(), ImGui::GetFont(), ImGui::GetFont()};
    // Resolve only the current atlas. This also tolerates additional application fonts
    // preceding the four HUD roles, without sharing pointers between renderers.
    for (auto* font : ImGui::GetIO().Fonts->Fonts) {
        const char* name = font->GetDebugName();
        if (std::strcmp(name, "FrameScope.Body") == 0) fonts.body = font;
        else if (std::strcmp(name, "FrameScope.Semibold") == 0) fonts.semibold = font;
        else if (std::strcmp(name, "FrameScope.Display") == 0) fonts.display = font;
        else if (std::strcmp(name, "FrameScope.Numbers") == 0) fonts.numbers = font;
    }
    return fonts;
}

enum class Tone { Neutral, Cpu, Gpu, Vram, Ram };

struct Palette {
    ImVec4 text;
    ImVec4 muted;
    ImVec4 faint;
    ImVec4 accent;
    ImVec4 background;

    ImVec4 Tint(Tone tone) const {
        ImVec4 color;
        switch (tone) {
        case Tone::Cpu: color = Color(0xff79adff); break;
        case Tone::Gpu: color = Color(0xff8bdfa7); break;
        case Tone::Vram: color = Color(0xffb6a0f5); break;
        case Tone::Ram: color = Color(0xff78d9e9); break;
        default: return accent;
        }
        color.w = text.w;
        return Mix(color, accent, 0.16f);
    }
};

float TextWidth(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

struct Painter {
    ImDrawList* draw;
    ImVec2 origin;
    float scale;
    Fonts fonts;
    Palette colors;

    ImVec2 Point(float x, float y) const {
        return ImVec2(origin.x + x * scale, origin.y + y * scale);
    }

    void Rect(float x, float y, float w, float h, ImVec4 color, float rounding = 0) const {
        if (w > 0 && h > 0) {
            draw->AddRectFilled(Point(x, y), Point(x + w, y + h),
                                ImGui::ColorConvertFloat4ToU32(color), rounding * scale);
        }
    }

    void Line(float x1, float y1, float x2, float y2, ImVec4 color, float thickness = 1) const {
        draw->AddLine(Point(x1, y1), Point(x2, y2), ImGui::ColorConvertFloat4ToU32(color),
                       std::max(0.5f, thickness * scale));
    }

    void Text(ImFont* font, float size, float x, float y, ImVec4 color,
              const char* text) const {
        draw->AddText(font, size * scale, Point(x, y), ImGui::ColorConvertFloat4ToU32(color), text);
    }

    void FitText(ImFont* font, float size, float x, float y, float width, ImVec4 color,
                 const char* text) const {
        const float measured = TextWidth(font, size, text);
        if (measured > width && measured > 0) {
            size *= std::max(0.0f, width) / measured;
        }
        if (size > 0) {
            Text(font, size, x, y, color, text);
        }
    }

    void Right(ImFont* font, float size, float right, float y, ImVec4 color,
               const char* text) const {
        Text(font, size, right - TextWidth(font, size, text), y, color, text);
    }

    // Align units by baseline, not by the top of two differently sized font boxes.
    void Value(const char* value, const char* unit, float right, float y, float size,
               float max_width, bool valid = true, ImFont* font = nullptr,
               float unit_size = 11.0f) const {
        font = font ? font : fonts.numbers;
        const float unit_width = unit[0] ? TextWidth(fonts.body, unit_size, unit) + 5.0f : 0;
        const float value_width = TextWidth(font, size, value);
        if (value_width + unit_width > max_width && value_width > 0) {
            size *= std::max(1.0f, max_width - unit_width) / value_width;
        }
        const float value_right = right - unit_width;
        Right(font, size, value_right, y, valid ? colors.text : colors.muted, value);
        if (unit[0]) {
            const float baseline = size * font->Ascent / font->FontSize;
            const float unit_baseline = unit_size * fonts.body->Ascent / fonts.body->FontSize;
            Text(fonts.body, unit_size, value_right + 5.0f, y + baseline - unit_baseline,
                   colors.muted, unit);
        }
    }

    void Bar(float x, float y, float width, float fraction, Tone tone) const {
        Rect(x, y, width, 2.0f, Fade(colors.text, 0.09f), 1.0f);
        if (std::isfinite(fraction) && fraction > 0) {
            Rect(x, y, width * std::clamp(fraction, 0.0f, 1.0f), 2.0f,
                   Fade(colors.Tint(tone), 0.8f), 1.0f);
        }
    }
};

bool Number(char* output, std::size_t capacity, double value, bool available, int precision = 0) {
    if (!available || !std::isfinite(value) || value < 0) {
        std::snprintf(output, capacity, "N/A");
        return false;
    }
    // Corrupt/extreme input cannot produce hundreds of digits or unbounded layout work.
    if (value >= 1000000.0) {
        std::snprintf(output, capacity, "%.3g", value);
    } else {
        std::snprintf(output, capacity, "%.*f", precision, value);
    }
    return true;
}

struct Metric {
    const char* label = "";
    const char* strip_label = "";
    const char* unit = "";
    char value[80]{};
    char secondary[64]{};
    Tone tone = Tone::Neutral;
    bool valid = false;
    bool bar = false;
    bool intel = false;
    float fraction = -1;
};

struct Model {
    std::array<Metric, 12> metrics{};
    std::size_t count = 0;
    std::size_t system_count = 0;
    char fps[32]{};
    char frametime[32]{};
    char low_1[32]{};
    char low_01[32]{};
    bool fps_valid = false;
    bool frametime_valid = false;
    bool low_1_valid = false;
    bool low_01_valid = false;
};

Model MakeModel(const ControlData& control, const FrameData& frame) {
    Model model;
    const auto has = [&control](Availability bit) { return (control.available_mask & bit) != 0; };
    const bool present = (frame.flags & HasFps) != 0;
    model.fps_valid = Number(model.fps, sizeof(model.fps), frame.fps, present);
    model.frametime_valid = Number(model.frametime, sizeof(model.frametime), frame.frametime_ms,
                                   present, 2);
    model.low_1_valid = Number(model.low_1, sizeof(model.low_1), frame.low_1, frame.flags & HasLow1);
    model.low_01_valid = Number(model.low_01, sizeof(model.low_01), frame.low_01, frame.flags & HasLow01);
    if (!present) {
        std::snprintf(model.fps, sizeof(model.fps), "-");
        std::snprintf(model.frametime, sizeof(model.frametime), "-");
    }
    if (!(frame.flags & HasLow1)) std::snprintf(model.low_1, sizeof(model.low_1), "-");
    if (!(frame.flags & HasLow01)) std::snprintf(model.low_01, sizeof(model.low_01), "-");

    const auto add = [&model](const char* label, const char* unit, Tone tone) -> Metric& {
        auto& metric = model.metrics[model.count++];
        metric.label = metric.strip_label = label;
        metric.unit = unit;
        metric.tone = tone;
        return metric;
    };
    if (control.flags & ShowCpu) {
        auto& metric = add("CPU", "%", Tone::Cpu);
        metric.valid = Number(metric.value, sizeof(metric.value), control.cpu_usage, has(Cpu));
        char maximum[32]{};
        const bool max_valid = Number(maximum, sizeof(maximum), control.cpu_max, has(CpuMax));
        std::snprintf(metric.secondary, sizeof(metric.secondary), "max core %s%s", maximum,
                       max_valid ? "%" : "");
        metric.bar = true;
        metric.fraction = metric.valid ? static_cast<float>(control.cpu_usage / 100.0) : -1;
    }
    if (control.flags & ShowGpu) {
        auto& metric = add("GPU", "%", Tone::Gpu);
        metric.valid = Number(metric.value, sizeof(metric.value), control.gpu_usage, has(Gpu));
        metric.bar = true;
        metric.fraction = metric.valid ? static_cast<float>(control.gpu_usage / 100.0) : -1;
    }
    if (control.flags & ShowVram) {
        auto& metric = add("VRAM", "GiB", Tone::Vram);
        char used[32]{}, total[32]{};
        metric.valid = Number(used, sizeof(used), control.vram_used_gib, has(Vram), 2);
        const bool capacity = has(Vram) && std::isfinite(control.vram_total_gib) &&
                              control.vram_total_gib > 0;
        if (capacity) {
            Number(total, sizeof(total), control.vram_total_gib, true, 1);
            std::snprintf(metric.value, sizeof(metric.value), "%s / %s", used, total);
        } else {
            std::snprintf(metric.value, sizeof(metric.value), "%s", used);
        }
        metric.bar = true;
        metric.fraction = metric.valid && capacity
            ? static_cast<float>(control.vram_used_gib / control.vram_total_gib) : -1;
    }
    if (control.flags & ShowRam) {
        auto& metric = add("RAM", "GiB", Tone::Ram);
        char used[32]{}, total[32]{};
        metric.valid = Number(used, sizeof(used), control.ram_used_gib, has(Ram), 1);
        Number(total, sizeof(total), control.ram_total_gib, has(Ram), 1);
        std::snprintf(metric.value, sizeof(metric.value), "%s / %s", used, total);
        metric.bar = true;
        metric.fraction = metric.valid && std::isfinite(control.ram_total_gib) && control.ram_total_gib > 0
            ? static_cast<float>(control.ram_used_gib / control.ram_total_gib) : -1;
    }
    if (control.flags & ShowTemperatures) {
        auto& cpu = add("CPU TEMP", "\xc2\xb0" "C", Tone::Cpu);
        cpu.valid = Number(cpu.value, sizeof(cpu.value), control.cpu_temperature, has(CpuTemperature));
        auto& gpu = add("GPU TEMP", "\xc2\xb0" "C", Tone::Gpu);
        gpu.valid = Number(gpu.value, sizeof(gpu.value), control.gpu_temperature, has(GpuTemperature));
    }
    model.system_count = model.count;
    if (control.flags & (ShowIntelClocks | ShowIntelPower | ShowIntelBandwidth | ShowIntelThrottle)) {
        thread_local Session intel_reader;
        IntelData intel{};
        // ReadIntel validates the optional block's magic/version and five-second freshness.
        const bool valid = intel_reader.ConnectFromEnvironment() && intel_reader.ReadIntel(intel);
        const auto intel_has = [&](IntelAvailability bit) {
            return valid && (intel.available_mask & bit) != 0;
        };
        const auto pair = [&](const char* label, const char* strip_label, const char* detail,
                              const char* unit, double a, bool has_a, double b, bool has_b,
                              int precision) {
            auto& metric = add(label, unit, Tone::Gpu);
            metric.intel = true;
            metric.strip_label = strip_label;
            char left[32]{}, right[32]{};
            const bool a_valid = Number(left, sizeof(left), a, has_a, precision);
            const bool b_valid = Number(right, sizeof(right), b, has_b, precision);
            metric.valid = a_valid || b_valid;
            std::snprintf(metric.value, sizeof(metric.value), "%s / %s", left, right);
            std::snprintf(metric.secondary, sizeof(metric.secondary), "%s", detail);
        };
        if (control.flags & ShowIntelClocks) {
            pair("CLOCKS", "CLOCK GPU / MEM", "GPU / memory", "MHz",
                   intel.gpu_frequency_mhz, intel_has(IntelGpuClock),
                   intel.memory_frequency_mhz, intel_has(IntelMemoryClock), 0);
        }
        if (control.flags & ShowIntelPower) {
            pair("POWER", "POWER GPU / BOARD", "GPU / board", "W",
                   intel.gpu_power_watts, intel_has(IntelGpuPower),
                   intel.board_power_watts, intel_has(IntelBoardPower), 1);
        }
        if (control.flags & ShowIntelBandwidth) {
            pair("MEM R/W", "MEM READ / WRITE", "read / write", "GB/s",
                   intel.memory_read_gbps, intel_has(IntelMemoryRead),
                   intel.memory_write_gbps, intel_has(IntelMemoryWrite), 1);
        }
        if (control.flags & ShowIntelThrottle) {
            auto& metric = add("THROTTLE", "", Tone::Gpu);
            metric.intel = true;
            metric.valid = intel_has(IntelThrottle);
            if (metric.valid) {
                std::snprintf(metric.value, sizeof(metric.value), "0x%llX",
                               static_cast<unsigned long long>(intel.throttle_flags));
            } else {
                std::snprintf(metric.value, sizeof(metric.value), "N/A");
            }
            std::snprintf(metric.secondary, sizeof(metric.secondary), "reason mask");
        }
    }
    return model;
}

void DrawSurface(const Painter& p, float width, float height, bool horizontal) {
    const float rounding = horizontal ? 10.0f : 12.0f;
    p.Rect(0, 0, width, height, p.colors.background, rounding);
    p.draw->AddRect(p.Point(0.5f, 0.5f), p.Point(width - 0.5f, height - 0.5f),
                     ImGui::ColorConvertFloat4ToU32(Fade(p.colors.text, 0.10f)),
                     rounding * p.scale, 0, std::max(0.5f, p.scale));
    p.Rect(horizontal ? 14.0f : 18.0f, 0, 28, 2, Fade(p.colors.accent, 0.8f), 1);
}

void Badge(const Painter& p, const char* api, float right, float y) {
    const float width = TextWidth(p.fonts.semibold, 10.5f, api) + 14;
    p.Rect(right - width, y, width, 21, Fade(p.colors.accent, 0.10f), 5);
    p.Text(p.fonts.semibold, 10.5f, right - width + 7, y + 4, p.colors.accent, api);
}

float GraphMaximum(const FrameData& frame, float& peak) {
    peak = -1;
    const auto count = std::min<std::uint32_t>(frame.graph_count, kGraphCapacity);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (std::isfinite(frame.graph[i]) && frame.graph[i] >= 0) {
            peak = std::max(peak, frame.graph[i]);
        }
    }
    // No percentile clipping: even a single long stall remains in range. Double
    // intermediates avoid overflow if an external writer sends a very large float.
    const double target = std::max(8.0, static_cast<double>(peak) * 1.12);
    const double step = std::pow(10.0, std::floor(std::log10(target))) * 0.5;
    return static_cast<float>(std::min(static_cast<double>(FLT_MAX), std::ceil(target / step) * step));
}

void GraphLabel(char* output, std::size_t capacity, float value) {
    if (value >= 10000) std::snprintf(output, capacity, "%.3g", static_cast<double>(value));
    else if (value >= 100) std::snprintf(output, capacity, "%.0f", static_cast<double>(value));
    else std::snprintf(output, capacity, "%.1f", static_cast<double>(value));
}

void DrawGraph(const Painter& p, const FrameData& frame, float x, float y,
               float width, float height, bool compact) {
    float peak = 0;
    const float maximum = GraphMaximum(frame, peak);
    char top[32]{}, middle[32]{}, peak_text[48]{};
    GraphLabel(top, sizeof(top), maximum);
    GraphLabel(middle, sizeof(middle), maximum * 0.5f);
    if (compact) {
        p.Text(p.fonts.semibold, 10.5f, x, y, p.colors.muted, "PACING");
        std::snprintf(peak_text, sizeof(peak_text), "%s ms", top);
        p.Right(p.fonts.numbers, 9.5f, x + width, y + 1, p.colors.faint, peak_text);
    } else {
        p.Text(p.fonts.semibold, 10.5f, x, y, p.colors.muted, "FRAME PACING");
        if (peak >= 0) {
            char value[32]{};
            GraphLabel(value, sizeof(value), peak);
            std::snprintf(peak_text, sizeof(peak_text), "peak %s ms", value);
            p.Right(p.fonts.body, 10.5f, x + width, y, p.colors.faint, peak_text);
        } else {
            p.Right(p.fonts.body, 10.5f, x + width, y, p.colors.faint, "present interval / ms");
        }
    }
    const float chart_y = y + (compact ? 17.0f : 23.0f);
    const float chart_h = std::max(1.0f, height - (compact ? 17.0f : 23.0f));
    const float reference_width = compact ? 0.0f : std::max(30.0f, TextWidth(p.fonts.numbers, 9.5f, top) + 5);
    const float chart_w = std::max(1.0f, width - reference_width);
    p.Rect(x, chart_y, chart_w, chart_h, Fade(p.colors.text, 0.025f), 4);
    for (int i = 0; i <= 2; ++i) {
        const float grid_y = chart_y + chart_h * static_cast<float>(i) * 0.5f;
        p.Line(x, grid_y, x + chart_w, grid_y, Fade(p.colors.text, i == 2 ? 0.10f : 0.065f), 0.75f);
    }
    for (int i = 1; i <= 4; ++i) {
        const float grid_x = x + chart_w * static_cast<float>(i) / 5.0f;
        p.Line(grid_x, chart_y, grid_x, chart_y + chart_h, Fade(p.colors.text, 0.035f), 0.6f);
    }
    if (!compact) {
        p.Right(p.fonts.numbers, 9.5f, x + width, chart_y - 4, p.colors.faint, top);
        p.Right(p.fonts.numbers, 9.5f, x + width, chart_y + chart_h * 0.5f - 5, p.colors.faint, middle);
        p.Right(p.fonts.numbers, 9.5f, x + width, chart_y + chart_h - 9, p.colors.faint, "0");
    }
    const auto count = std::min<std::uint32_t>(frame.graph_count, kGraphCapacity);
    bool previous_valid = false;
    bool any_valid = false;
    ImVec2 previous;
    const auto line_color = ImGui::ColorConvertFloat4ToU32(Fade(p.colors.accent, 0.92f));
    const auto area_color = ImGui::ColorConvertFloat4ToU32(Fade(p.colors.accent, 0.10f));
    for (std::uint32_t i = 0; i < count; ++i) {
        const float value = frame.graph[i];
        if (!std::isfinite(value) || value < 0) {
            previous_valid = false; // Missing samples are gaps, not invented zeroes.
            continue;
        }
        const float fraction = std::clamp(value / maximum, 0.0f, 1.0f);
        const float sample_x = x + (count > 1 ? chart_w * static_cast<float>(i) / static_cast<float>(count - 1) : chart_w);
        const ImVec2 point = p.Point(sample_x, chart_y + chart_h * (1.0f - fraction));
        if (previous_valid) {
            const float base = p.Point(0, chart_y + chart_h).y;
            p.draw->AddQuadFilled(ImVec2(previous.x, base), previous, point, ImVec2(point.x, base), area_color);
            p.draw->AddLine(previous, point, line_color, std::max(0.75f, 1.3f * p.scale));
        }
        if (i + 1 == count || count == 1) {
            p.draw->AddCircleFilled(point, std::max(1.0f, 2.0f * p.scale), line_color, 8);
        }
        previous = point;
        previous_valid = any_valid = true;
    }
    if (!any_valid) {
        const char* text = count == 0 ? "collecting intervals" : "N/A";
        const float size = compact ? 9.0f : 11.0f;
        p.FitText(p.fonts.body, size, x + 6, chart_y + (chart_h - size) * 0.5f,
                    chart_w - 12, p.colors.faint, text);
    }
}

float PanelHeight(const ControlData& control, const Model& model) {
    float height = 42;
    if (control.flags & (ShowFps | ShowFrametime)) height += 64;
    if (control.flags & ShowLows) height += 68;
    if (model.system_count) height += 28 + 40 * static_cast<float>(model.system_count);
    if (model.count > model.system_count) height += 28 + 40 * static_cast<float>(model.count - model.system_count);
    if (control.flags & Graph) height += 100;
    return height + 16;
}

void PanelLow(const Painter& p, const char* label, const char* value, bool valid,
              bool eligible, const char* collecting, float x, float y, float width) {
    p.Rect(x, y, width, 60, Fade(p.colors.text, 0.035f), 6);
    p.Text(p.fonts.semibold, 10.5f, x + 11, y + 7, p.colors.muted, label);
    p.Value(value, eligible ? "FPS" : "", x + width - 11, y + 20, 21, width - 22,
              valid, p.fonts.semibold, 10);
    if (!eligible) {
        p.FitText(p.fonts.body, 9.5f, x + 11, y + 45, width - 22, p.colors.faint, collecting);
    }
}

void PanelSection(const Painter& p, const char* title, float y, float width) {
    p.Line(18, y, width - 18, y, Fade(p.colors.text, 0.09f));
    p.Text(p.fonts.semibold, 10.5f, 18, y + 9, p.colors.faint, title);
}

void DrawPanel(const Painter& p, const ControlData& control, const FrameData& frame,
               const Model& model, float width) {
    const float right = width - 18;
    const float inner = width - 36;
    p.Text(p.fonts.semibold, 11.5f, 18, 15, p.colors.text, "FRAMESCOPE");
    p.Text(p.fonts.body, 10.5f, 103, 16, p.colors.faint, "PRESENT");
    Badge(p, ApiName(frame.api), right, 11);
    float y = 42;
    const bool fps = (control.flags & ShowFps) != 0;
    const bool frametime = (control.flags & ShowFrametime) != 0;
    if (fps) {
        const float fps_width = frametime ? inner * 0.57f : inner;
        const float unit_width = TextWidth(p.fonts.semibold, 11, "FPS") + 7;
        float size = 36;
        const float measured = TextWidth(p.fonts.display, size, model.fps);
        if (measured + unit_width > fps_width) size *= (fps_width - unit_width) / measured;
        p.Text(p.fonts.display, size, 18, y, model.fps_valid ? p.colors.text : p.colors.muted, model.fps);
        const float baseline = size * p.fonts.display->Ascent / p.fonts.display->FontSize;
        p.Text(p.fonts.semibold, 11, 18 + TextWidth(p.fonts.display, size, model.fps) + 7,
                  y + baseline - 11 * p.fonts.semibold->Ascent / p.fonts.semibold->FontSize,
                  p.colors.muted, "FPS");
        p.Text(p.fonts.body, 10.5f, 19, y + 43, p.colors.faint,
                  (frame.flags & HasFps) ? "application present" : "collecting intervals");
    }
    if (frametime) {
        if (fps) {
            p.Right(p.fonts.semibold, 10, right, y + 5, p.colors.faint, "FRAME TIME");
            p.Value(model.frametime, "ms", right, y + 21, 23, inner * 0.41f,
                      model.frametime_valid, p.fonts.semibold);
        } else {
            p.Text(p.fonts.semibold, 10.5f, 18, y, p.colors.faint, "FRAME TIME / APPLICATION PRESENT");
            p.Value(model.frametime, "ms", right, y + 18, 32, inner,
                      model.frametime_valid, p.fonts.semibold, 12);
        }
    }
    if (fps || frametime) y += 64;
    if (control.flags & ShowLows) {
        const float cell_width = (inner - 10) * 0.5f;
        PanelLow(p, "1% LOW", model.low_1, model.low_1_valid, frame.flags & HasLow1,
                    "collecting / 100+ intervals", 18, y, cell_width);
        PanelLow(p, "0.1% LOW", model.low_01, model.low_01_valid, frame.flags & HasLow01,
                    "collecting / 1000+ intervals", 28 + cell_width, y, cell_width);
        y += 68;
    }
    for (std::size_t i = 0; i < model.count; ++i) {
        if (i == 0 || i == model.system_count) {
            PanelSection(p, model.metrics[i].intel ? "INTEL TELEMETRY" : "SYSTEM", y, width);
            y += 28;
        }
        const auto& metric = model.metrics[i];
        p.Rect(18, y + 9, 3, 9, p.colors.Tint(metric.tone), 1.5f);
        p.Text(p.fonts.semibold, 16.0f, 29, y + 3, p.colors.text, metric.label);
        const float label_width = TextWidth(p.fonts.semibold, 16.0f, metric.label);
        p.Value(metric.value, metric.unit, right, y + 1, 16.5f,
                  inner - label_width - 24, metric.valid);
        if (metric.bar) p.Bar(29, y + 29, 64, metric.fraction, metric.tone);
        if (metric.secondary[0]) {
            p.Right(p.fonts.body, 10.5f, right, y + 23, p.colors.faint, metric.secondary);
        }
        if (i + 1 != model.count && i + 1 != model.system_count) {
            p.Line(29, y + 39, right, y + 39, Fade(p.colors.text, 0.035f), 0.65f);
        }
        y += 40;
    }
    if (control.flags & Graph) {
        p.Line(18, y + 2, right, y + 2, Fade(p.colors.text, 0.09f));
        DrawGraph(p, frame, 18, y + 15, inner, 78, false);
    }
}

enum class TileKind { Source, Fps, Frametime, Low1, Low01, Metric, Graph };

struct Tile {
    TileKind kind = TileKind::Source;
    std::size_t metric = 0;
    float width = 0;
    float x = 0;
    float y = 0;
    unsigned row = 0;
};

struct StripLayout {
    std::array<Tile, 20> tiles{};
    std::size_t count = 0;
    float width = 0;
    float height = 0;
};

StripLayout StripTiles(const Fonts& fonts, const ControlData& control, const Model& model) {
    StripLayout layout;
    const auto add = [&layout](TileKind kind, float width, std::size_t metric = 0) {
        auto& tile = layout.tiles[layout.count++];
        tile.kind = kind;
        tile.width = width;
        tile.metric = metric;
    };
    const auto value_width = [&fonts](const char* value, const char* unit, float size) {
        return TextWidth(fonts.numbers, size, value) +
               (unit[0] ? TextWidth(fonts.body, 11, unit) + 5 : 0);
    };
    if (control.flags & ShowFps) {
        add(TileKind::Fps, std::max(94.0f, TextWidth(fonts.display, 29, model.fps) + 30));
    } else {
        add(TileKind::Source, 76);
    }
    if (control.flags & ShowFrametime) {
        add(TileKind::Frametime, std::max(84.0f, value_width(model.frametime, "ms", 19)));
    }
    if (control.flags & ShowLows) {
        add(TileKind::Low1, std::max(92.0f, value_width(model.low_1, "FPS", 19)));
        add(TileKind::Low01, std::max(96.0f, value_width(model.low_01, "FPS", 19)));
    }
    for (std::size_t i = 0; i < model.count; ++i) {
        const auto& metric = model.metrics[i];
        float width = std::max(value_width(metric.value, metric.unit, 19),
                               TextWidth(fonts.semibold, 10.5f, metric.strip_label) + 7);
        if (metric.tone == Tone::Cpu && metric.bar) {
            width = std::max(width, TextWidth(fonts.semibold, 10.5f, metric.label) + 17 +
                                     std::max(TextWidth(fonts.body, 9.5f, metric.secondary),
                                                TextWidth(fonts.body, 9.5f, "max core 100%")));
        } else if (metric.tone == Tone::Vram && metric.bar) {
            width = std::max(width, value_width("00.00 / 00.0", metric.unit, 19));
        } else if (metric.tone == Tone::Ram && metric.bar) {
            width = std::max(width, value_width("00.0 / 00.0", metric.unit, 19));
        }
        add(TileKind::Metric, std::max(66.0f, width + 1), i);
    }
    if (control.flags & Graph) add(TileKind::Graph, 148);
    return layout;
}

StripLayout ArrangeStrip(const StripLayout& tiles, float maximum_width) {
    StripLayout layout = tiles;
    constexpr float padding_x = 14;
    constexpr float padding_y = 8;
    constexpr float gap = 20;
    constexpr float row_height = 44;
    constexpr float row_gap = 16;
    const float budget = std::max(1.0f, maximum_width - padding_x * 2);
    float x = 0;
    float used_width = 0;
    unsigned row = 0;
    for (std::size_t i = 0; i < layout.count; ++i) {
        auto& tile = layout.tiles[i];
        tile.width = std::min(tile.width, budget);
        if (x > 0 && x + gap + tile.width > budget) {
            ++row;
            x = 0;
        }
        if (x > 0) x += gap;
        tile.x = padding_x + x;
        tile.y = padding_y + static_cast<float>(row) * (row_height + row_gap);
        tile.row = row;
        x += tile.width;
        used_width = std::max(used_width, x);
    }
    layout.width = used_width + padding_x * 2;
    layout.height = padding_y * 2 + static_cast<float>(row + 1) * row_height + static_cast<float>(row) * row_gap;
    return layout;
}

void DrawStrip(const Painter& p, const FrameData& frame,
               const Model& model, const StripLayout& layout) {
    for (std::size_t i = 0; i < layout.count; ++i) {
        const auto& tile = layout.tiles[i];
        const float x = tile.x;
        const float y = tile.y;
        const float right = x + tile.width;
        if (i > 0 && layout.tiles[i - 1].row == tile.row) {
            p.Line(x - 10, y + 4, x - 10, y + 39, Fade(p.colors.text, 0.10f), 0.75f);
        } else if (i > 0) {
            p.Line(14, y - 8, layout.width - 14, y - 8, Fade(p.colors.text, 0.08f), 0.75f);
        }
        switch (tile.kind) {
        case TileKind::Source:
            p.Text(p.fonts.semibold, 10, x, y + 1, p.colors.faint, "FRAMESCOPE");
            Badge(p, ApiName(frame.api), right, y + 19);
            break;
        case TileKind::Fps: {
            const char* api = ApiName(frame.api);
            const float api_width = TextWidth(p.fonts.semibold, 9.5f, api) + 10;
            p.Rect(x, y, api_width, 15, Fade(p.colors.accent, 0.11f), 3);
            p.Text(p.fonts.semibold, 9.5f, x + 5, y + 1, p.colors.accent, api);
            p.Text(p.fonts.body, 9.5f, x + api_width + 6, y + 1, p.colors.faint,
                      (frame.flags & HasFps) ? "Present" : "collecting");
            p.Value(model.fps, "FPS", right, y + 14, 29, tile.width,
                      model.fps_valid, p.fonts.display, 10);
            break;
        }
        case TileKind::Frametime:
            p.Text(p.fonts.semibold, 10.5f, x, y + 1, p.colors.muted, "FRAME TIME");
            p.Value(model.frametime, "ms", right, y + 18, 19, tile.width, model.frametime_valid);
            break;
        case TileKind::Low1:
        case TileKind::Low01: {
            const bool one = tile.kind == TileKind::Low1;
            const bool eligible = (frame.flags & (one ? HasLow1 : HasLow01)) != 0;
            p.Text(p.fonts.semibold, 10.5f, x, y + 1, p.colors.muted, one ? "1% LOW" : "0.1% LOW");
            p.Value(one ? model.low_1 : model.low_01, eligible ? "FPS" : "", right,
                      y + 16, 19, tile.width, one ? model.low_1_valid : model.low_01_valid);
            if (!eligible) {
                p.FitText(p.fonts.body, 9, x, y + 36, tile.width, p.colors.faint,
                            one ? "collecting / 100+" : "collecting / 1000+");
            }
            break;
        }
        case TileKind::Metric: {
            const auto& metric = model.metrics[tile.metric];
            p.Rect(x, y + 4, 2, 7, p.colors.Tint(metric.tone), 1);
            p.FitText(p.fonts.semibold, 10.5f, x + 7, y + 1, tile.width - 7,
                        p.colors.muted, metric.strip_label);
            if (metric.tone == Tone::Cpu && metric.bar) {
                p.Right(p.fonts.body, 9.5f, right, y + 2, p.colors.faint, metric.secondary);
            }
            p.Value(metric.value, metric.unit, right, y + 18, 19, tile.width, metric.valid);
            if (metric.bar) p.Bar(x, y + 42, tile.width, metric.fraction, metric.tone);
            break;
        }
        case TileKind::Graph:
            DrawGraph(p, frame, x, y + 1, tile.width, 43, true);
            break;
        }
    }
}

} // namespace

void DrawHud(const ControlData& control, const FrameData& frame, float width, float height) {
    if (!(control.flags & Enabled) || !ImGui::GetCurrentContext() ||
        !std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1) {
        return;
    }
    float scale = std::isfinite(control.scale) ? std::clamp(control.scale, 0.5f, 3.0f) : 1.0f;
    const float opacity = std::isfinite(control.opacity) ? std::clamp(control.opacity, 0.0f, 1.0f) : 0.75f;
    const float margin_x = std::isfinite(control.margin_x)
        ? std::clamp(control.margin_x, 0.0f, std::max(0.0f, (width - 1) * 0.5f)) : std::min(16.0f, (width - 1) * 0.5f);
    const float margin_y = std::isfinite(control.margin_y)
        ? std::clamp(control.margin_y, 0.0f, std::max(0.0f, (height - 1) * 0.5f)) : std::min(16.0f, (height - 1) * 0.5f);
    const float maximum_width = std::max(1.0f, width - margin_x * 2);
    const float maximum_height = std::max(1.0f, height - margin_y * 2);
    const bool horizontal = (control.flags & Horizontal) != 0;
    const auto fonts = ContextFonts();
    const auto model = MakeModel(control, frame);
    float logical_width = 360;
    float logical_height = 0;
    StripLayout strip;
    if (horizontal) {
        // Keep even the widest selected metric intact on a narrow window. Wrap whole
        // tiles first; only scale down further when the available height requires it.
        const auto tiles = StripTiles(fonts, control, model);
        float widest = 0;
        for (std::size_t i = 0; i < tiles.count; ++i) widest = std::max(widest, tiles.tiles[i].width);
        scale = std::min(scale, maximum_width / (widest + 28));
        strip = ArrangeStrip(tiles, maximum_width / scale);
        if (strip.height * scale > maximum_height) {
            // Reflow changes the row count. Find the largest fitting scale rather
            // than shrinking for the old row count and leaving a needlessly tiny HUD.
            float lower = maximum_height / strip.height;
            float upper = scale;
            for (int iteration = 0; iteration < 12; ++iteration) {
                const float candidate = (lower + upper) * 0.5f;
                const auto arranged = ArrangeStrip(tiles, maximum_width / candidate);
                if (arranged.height * candidate <= maximum_height) lower = candidate;
                else upper = candidate;
            }
            scale = lower;
            strip = ArrangeStrip(tiles, maximum_width / scale);
        }
        logical_width = strip.width;
        logical_height = strip.height;
    } else {
        logical_width = std::clamp(maximum_width / scale, 300.0f, 360.0f);
        logical_height = PanelHeight(control, model);
        scale = std::min({scale, maximum_width / logical_width, maximum_height / logical_height});
    }
    const ImVec4 text = Color(control.text_color);
    const Palette colors{text, Fade(text, 0.68f), Fade(text, 0.48f), Color(control.accent_color),
                          Color(control.background_color, opacity)};
    const float hud_width = logical_width * scale;
    const float hud_height = logical_height * scale;
    const unsigned corner = std::min(control.corner, 3u);
    const bool right = corner == 1 || corner == 3;
    const bool bottom = corner >= 2;
    const ImVec2 position(right ? width - margin_x - hud_width : margin_x,
                            bottom ? height - margin_y - hud_height : margin_y);
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(hud_width, hud_height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("FrameScope HUD", nullptr, window_flags)) {
        const Painter painter{ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), scale, fonts, colors};
        DrawSurface(painter, logical_width, logical_height, horizontal);
        if (horizontal) DrawStrip(painter, frame, model, strip);
        else DrawPanel(painter, control, frame, model, logical_width);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace framescope
