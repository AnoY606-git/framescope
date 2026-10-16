// SPDX-License-Identifier: GPL-3.0-only
#include "demo.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace framescope::demo {
namespace {
struct Color { float r, g, b; };
struct Point { float x, y, z; };

Vertex MakeVertex(float x, float y, Color c) {
    return {x, y, 0.5f, c.r, c.g, c.b};
}

void Line(std::vector<Vertex>& out, float ax, float ay, float bx, float by,
          float aspect, float thickness, Color c) {
    const float dx = (bx - ax) * aspect;
    const float dy = by - ay;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.00001f) return;
    const float nx = -dy / length * thickness / aspect;
    const float ny = dx / length * thickness;
    const Vertex a = MakeVertex(ax + nx, ay + ny, c);
    const Vertex b = MakeVertex(ax - nx, ay - ny, c);
    const Vertex d = MakeVertex(bx + nx, by + ny, c);
    const Vertex e = MakeVertex(bx - nx, by - ny, c);
    out.insert(out.end(), {a, b, d, d, b, e});
}
} // namespace

// Original, deterministic scene shared by all APIs. Projection and painter's
// ordering are deliberately shared too: the GPU runs the same colored-triangle
// workload regardless of the selected graphics backend. No text or HUD exists.
std::vector<Vertex> BuildScene(double seconds, unsigned width, unsigned height) {
    const float t = static_cast<float>(std::fmod(seconds, 3600.0));
    const float aspect = static_cast<float>(std::max(width, 1u)) /
                         static_cast<float>(std::max(height, 1u));
    const float lineWidth = 1.15f / static_cast<float>(std::max(height, 1u));
    std::vector<Vertex> out;
    out.reserve(512);

    // A quiet perspective grid, made from actual narrow triangle rectangles.
    for (int i = -16; i <= 16; ++i) {
        const float x = static_cast<float>(i) * 0.14f;
        Line(out, x, -0.96f, x * 0.12f, -0.16f, aspect, lineWidth,
             {0.075f, 0.19f, 0.25f});
    }
    for (int i = 0; i < 14; ++i) {
        const float v = static_cast<float>(i) / 13.0f;
        const float y = -0.96f + (1.0f - (1.0f - v) * (1.0f - v)) * 0.8f;
        Line(out, -1.0f, y, 1.0f, y, aspect, lineWidth,
             {0.075f, 0.19f, 0.25f});
    }

    // Six small orbiting triangles make motion visible even at low frame rates.
    constexpr float pi = 3.14159265358979323846f;
    for (int i = 0; i < 6; ++i) {
        const float angle = t * 0.35f + static_cast<float>(i) * pi / 3.0f;
        const float x = std::cos(angle) * 1.1f / aspect;
        const float y = 0.13f + std::sin(angle) * 0.62f;
        for (int corner = 0; corner < 3; ++corner) {
            const float a = -t + static_cast<float>(corner) * 2.0f * pi / 3.0f;
            const Color color = corner == 0 ? Color{0.2f, 0.8f, 0.95f} :
                                corner == 1 ? Color{0.8f, 0.35f, 0.95f} :
                                              Color{0.95f, 0.75f, 0.25f};
            out.push_back(MakeVertex(x + std::cos(a) * 0.045f / aspect,
                                     y + std::sin(a) * 0.045f, color));
        }
    }

    const float cy = std::cos(t * 0.67f), sy = std::sin(t * 0.67f);
    const float cx = std::cos(t * 0.39f), sx = std::sin(t * 0.39f);
    std::array<Point, 8> transformed{};
    std::array<Vertex, 8> projected{};
    for (unsigned i = 0; i < 8; ++i) {
        const float x = (i & 1u) ? 0.72f : -0.72f;
        const float y = (i & 2u) ? 0.72f : -0.72f;
        const float z = (i & 4u) ? 0.72f : -0.72f;
        const float rx = x * cy + z * sy;
        const float rz = -x * sy + z * cy;
        transformed[i] = {rx, y * cx - rz * sx, y * sx + rz * cx};
        const auto& p = transformed[i];
        const float scale = 2.1f / (p.z + 4.0f);
        projected[i] = MakeVertex(p.x * scale / aspect, p.y * scale + 0.15f,
                                  {1.0f, 1.0f, 1.0f});
    }
    struct Face { unsigned a, b, c, d; Color color; float depth; };
    std::array<Face, 6> faces{{
        {0, 1, 3, 2, {0.95f, 0.30f, 0.22f}, 0},
        {4, 6, 7, 5, {0.20f, 0.70f, 0.95f}, 0},
        {0, 4, 5, 1, {0.75f, 0.30f, 0.92f}, 0},
        {2, 3, 7, 6, {0.25f, 0.90f, 0.62f}, 0},
        {0, 2, 6, 4, {0.95f, 0.70f, 0.20f}, 0},
        {1, 5, 7, 3, {0.30f, 0.45f, 0.95f}, 0},
    }};
    for (auto& f : faces) {
        f.depth = transformed[f.a].z + transformed[f.b].z +
                  transformed[f.c].z + transformed[f.d].z;
    }
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) {
        return a.depth > b.depth;
    });
    for (const auto& f : faces) {
        auto a = projected[f.a], b = projected[f.b];
        auto c = projected[f.c], d = projected[f.d];
        const auto tint = [&](Vertex& v, float brightness) {
            v.r = f.color.r * brightness;
            v.g = f.color.g * brightness;
            v.b = f.color.b * brightness;
        };
        tint(a, 0.65f); tint(b, 0.85f); tint(c, 1.0f); tint(d, 0.8f);
        out.insert(out.end(), {a, b, c, a, c, d});
    }
    return out;
}

const char* ApiName(Api api) noexcept {
    switch (api) {
    case Api::Dx11: return "dx11";
    case Api::Dx12: return "dx12";
    case Api::Vulkan: return "vulkan";
    }
    return "unknown";
}
} // namespace framescope::demo
