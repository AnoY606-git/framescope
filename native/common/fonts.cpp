// SPDX-License-Identifier: GPL-3.0-only
#include "framescope/hud_core.hpp"

#include <imgui.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace framescope {
namespace {

// Static ranges, not an on-demand glyph cache: the 1.91 atlas is uploaded only once.
constexpr ImWchar kHudGlyphs[] = {0x0020, 0x007e, 0x00b0, 0x00b0, 0x00b7, 0x00b7,
                                 0x2013, 0x2014, 0};

ImFont* LoadInstalledFont(ImFontAtlas& atlas, const std::wstring& directory,
                          const wchar_t* filename, const ImFontConfig& config) {
    if (directory.empty()) {
        return nullptr;
    }
    const std::wstring path = directory + filename;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 12 ||
        length.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(file);
        return nullptr;
    }
    const auto size = static_cast<DWORD>(length.QuadPart);
    void* data = IM_ALLOC(size);
    if (!data) {
        CloseHandle(file);
        return nullptr;
    }
    DWORD read = 0;
    const bool success = ReadFile(file, data, size, &read, nullptr) && read == size;
    CloseHandle(file);
    if (!success) {
        IM_FREE(data);
        return nullptr;
    }
    // Atlas ownership includes the font bytes; no application-lifetime font pointers.
    return atlas.AddFontFromMemoryTTF(data, static_cast<int>(size), config.SizePixels,
                                       &config, kHudGlyphs);
}

ImFont* AddRole(ImFontAtlas& atlas, const std::wstring& directory, const char* name,
                 const wchar_t* filename, const wchar_t* fallback, float size) {
    ImFontConfig config;
    config.SizePixels = size;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = false;
    config.GlyphRanges = kHudGlyphs;
    std::snprintf(config.Name, sizeof(config.Name), "%s", name);
    if (auto* font = LoadInstalledFont(atlas, directory, filename, config)) {
        return font;
    }
    if (auto* font = LoadInstalledFont(atlas, directory, fallback, config)) {
        return font;
    }
    return atlas.AddFontDefault(&config);
}

} // namespace

void ConfigureHudFonts() {
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    auto* atlas = io.Fonts;
    if (!atlas) {
        return;
    }
    // Role names and pointers live in this atlas, never in a process-global context map.
    // On a new HUD context Fonts[0..3] are Body, Semibold, Display and Numbers.
    for (const auto& source : atlas->Sources) {
        if (std::strcmp(source.Name, "FrameScope.Body") == 0) {
            return;
        }
    }
    // This entry point is initialization-only. Never invalidate an uploaded/locked atlas.
    if (atlas->Locked || atlas->IsBuilt() || atlas->TexID != 0) {
        return;
    }
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring directory = length > 0 && length < MAX_PATH
        ? std::wstring(windows, length) + L"\\Fonts\\" : std::wstring{};
    atlas->Flags |= ImFontAtlasFlags_NoMouseCursors;
    atlas->TexDesiredWidth = 1024;
    io.FontDefault = AddRole(*atlas, directory, "FrameScope.Body", L"segoeui.ttf",
                             L"tahoma.ttf", 40.0f);
    AddRole(*atlas, directory, "FrameScope.Semibold", L"seguisb.ttf", L"segoeui.ttf", 40.0f);
    AddRole(*atlas, directory, "FrameScope.Display", L"seguisb.ttf", L"segoeui.ttf", 80.0f);
    AddRole(*atlas, directory, "FrameScope.Numbers", L"consola.ttf", L"segoeui.ttf", 40.0f);
    // Widget fallback is a readable 16 px. DrawHud uses explicit sizes from these
    // oversampled fonts, so changing HUD scale never rebuilds or resizes the atlas.
    io.FontGlobalScale = 16.0f / 40.0f;
}

} // namespace framescope
