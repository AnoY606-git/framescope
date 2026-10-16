// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace framescope::demo {

enum class Api { Dx11, Dx12, Vulkan };
enum class WindowMode { Windowed, Borderless, Exclusive };

struct Options {
    Api api = Api::Dx11;
    WindowMode mode = WindowMode::Windowed;
    double duration = 0.0;
    unsigned cpuLoad = 0;
    unsigned gpuLoad = 1;
    unsigned stutterMs = 0;
    bool vsync = true;
};

struct Vertex {
    float x, y, z;
    float r, g, b;
};
static_assert(sizeof(Vertex) == 24);
inline constexpr std::size_t kMaxVertices = 4096;
inline constexpr float kClearColor[4] = {0.025f, 0.035f, 0.065f, 1.0f};

// A successful present means the API accepted the presentation, not that a
// monitor displayed it. Occluded/out-of-date/minimized frames are not counted.
enum class PresentResult { Presented, Skipped };

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void Initialize(HWND window, unsigned width, unsigned height,
                            const Options& options) = 0;
    virtual void Resize(unsigned width, unsigned height) = 0;
    virtual PresentResult Render(const std::vector<Vertex>& vertices,
                                 unsigned drawCount) = 0;
    virtual unsigned Width() const noexcept = 0;
    virtual unsigned Height() const noexcept = 0;
    virtual bool FullscreenAcquired() const noexcept = 0;
};

std::unique_ptr<Renderer> MakeDx11Renderer();
std::unique_ptr<Renderer> MakeDx12Renderer();
std::unique_ptr<Renderer> MakeVulkanRenderer();
std::vector<Vertex> BuildScene(double seconds, unsigned width, unsigned height);
const char* ApiName(Api api) noexcept;

} // namespace framescope::demo
