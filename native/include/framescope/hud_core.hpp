#pragma once

#include "framescope/protocol.hpp"

#include <deque>
#include <mutex>
#include <vector>

namespace framescope {

class FrameStatistics {
public:
    explicit FrameStatistics(std::uint64_t frequency);
    void Observe(std::uint64_t timestamp);
    FrameData Snapshot() const;

private:
    std::uint64_t frequency_;
    std::uint64_t last_timestamp_ = 0;
    std::uint64_t count_ = 0;
    std::deque<double> intervals_;
    std::deque<std::uint64_t> timestamps_;
    mutable std::uint64_t lows_updated_at_ = 0;
    mutable double low_1_ = 0;
    mutable double low_01_ = 0;
    mutable std::uint32_t low_flags_ = 0;
};

// Один Session принадлежит одному выбранному swapchain. Драйверы и датчики здесь не опрашиваются.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool ConnectFromEnvironment();
    bool ReadControl(ControlData& result) const;
    bool ReadIntel(IntelData& result) const;
    FrameData Snapshot() const;
    void ObservePresent(std::uint64_t timestamp, Api api, std::uint32_t width,
                        std::uint32_t height, double hud_cpu_us = 0.0);

private:
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    mutable std::mutex mutex_;
    FrameStatistics statistics_;
    FrameData latest_{};
};

// Вызывать между ImGui::NewFrame() и ImGui::Render() с context, принадлежащим backend.
// Цвета в протоколе — 0xAARRGGBB. Отсутствующие данные никогда не заменяются нулём.
void DrawHud(const ControlData& control, const FrameData& frame, float width, float height);
// Один раз в новом ImGui context до создания GPU atlas; используются установленные шрифты Windows.
void ConfigureHudFonts();

} // namespace framescope
