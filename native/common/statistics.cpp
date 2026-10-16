#include "framescope/hud_core.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace framescope {

FrameStatistics::FrameStatistics(std::uint64_t frequency) : frequency_(frequency) {
    if (frequency == 0) {
        throw std::invalid_argument("QPC frequency must be positive");
    }
}

void FrameStatistics::Observe(std::uint64_t timestamp) {
    if (count_ != 0 && timestamp <= last_timestamp_) {
        return;
    }
    if (count_ != 0) {
        const double interval = static_cast<double>(timestamp - last_timestamp_) * 1000.0 /
                                static_cast<double>(frequency_);
        intervals_.push_back(interval);
        timestamps_.push_back(timestamp);
        // Ограничены одновременно возраст и объём: высокие FPS не растят память без границ.
        while (!timestamps_.empty() &&
               (timestamps_.size() > 10000 || timestamp - timestamps_.front() > frequency_ * 60)) {
            timestamps_.pop_front();
            intervals_.pop_front();
        }
    }
    last_timestamp_ = timestamp;
    ++count_;
}

FrameData FrameStatistics::Snapshot() const {
    FrameData result{};
    result.present_count = count_;
    result.timestamp_qpc = last_timestamp_;
    if (intervals_.empty()) {
        return result;
    }
    result.frametime_ms = intervals_.back();
    double interval_sum = 0;
    std::size_t sample_count = 0;
    for (auto it = intervals_.rbegin(); it != intervals_.rend(); ++it) {
        interval_sum += *it;
        ++sample_count;
        if (interval_sum >= 1000.0) {
            break;
        }
    }
    result.fps = 1000.0 * static_cast<double>(sample_count) / interval_sum;
    result.flags = HasFps;

    const bool newly_eligible = (intervals_.size() >= 100 && !(low_flags_ & HasLow1)) ||
                                (intervals_.size() >= 1000 && !(low_flags_ & HasLow01));
    if (newly_eligible || lows_updated_at_ == 0 ||
        last_timestamp_ - lows_updated_at_ >= frequency_ / 4) {
        std::vector<double> sorted(intervals_.begin(), intervals_.end());
        std::sort(sorted.begin(), sorted.end(), std::greater<>());
        low_flags_ = 0;
        const auto low = [&sorted](std::size_t count) {
            const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(count);
            return 1000.0 * static_cast<double>(count) / std::accumulate(sorted.begin(), end, 0.0);
        };
        if (sorted.size() >= 100) {
            low_1_ = low((sorted.size() + 99) / 100);
            low_flags_ |= HasLow1;
        }
        if (sorted.size() >= 1000) {
            low_01_ = low((sorted.size() + 999) / 1000);
            low_flags_ |= HasLow01;
        }
        lows_updated_at_ = last_timestamp_;
    }
    result.flags |= low_flags_;
    result.low_1 = low_1_;
    result.low_01 = low_01_;
    result.graph_count = static_cast<std::uint32_t>(std::min(intervals_.size(), kGraphCapacity));
    const auto graph_start = intervals_.end() - static_cast<std::ptrdiff_t>(result.graph_count);
    std::transform(graph_start, intervals_.end(), result.graph,
                   [](double value) { return static_cast<float>(value); });
    return result;
}

} // namespace framescope
