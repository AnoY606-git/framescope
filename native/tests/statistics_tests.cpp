#include "framescope/hud_core.hpp"

#include <windows.h>

#include <cmath>
#include <iostream>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {
int failures = 0;
void Check(bool value, const char* description) {
    if (!value) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}
bool Near(double actual, double expected, double tolerance = 0.0001) {
    return std::abs(actual - expected) <= tolerance;
}

void CheckSharedMemory() {
    using namespace framescope;
    std::wostringstream name;
    name << std::hex << std::setfill(L'0') << std::setw(16) << GetCurrentProcessId()
         << std::setw(16) << QpcNow();
    const auto session_id = name.str();
    wchar_t original[256]{};
    const DWORD original_length = GetEnvironmentVariableW(L"FRAMESCOPE_SESSION", original, 256);
    SetEnvironmentVariableW(L"FRAMESCOPE_SESSION", session_id.c_str());
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(kMappingSize), MappingName(session_id).c_str());
    Check(handle != nullptr, "controller can create unprivileged session mapping");
    if (!handle) { return; }
    auto* data = static_cast<std::uint8_t*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, kMappingSize));
    Check(data != nullptr, "controller can map session view");
    if (!data) { CloseHandle(handle); return; }
    {
        auto* header = reinterpret_cast<std::uint32_t*>(data);
        header[0] = kMagic;
        header[1] = kProtocolVersion + 1;
        header[2] = static_cast<std::uint32_t>(kMappingSize);
        Session incompatible;
        Check(!incompatible.ConnectFromEnvironment(), "incompatible ABI is rejected");
        header[1] = kProtocolVersion;
        Session reader;
        Check(reader.ConnectFromEnvironment(), "compatible session connects");
        ControlData control{};
        control.flags = Enabled;
        control.scale = 1.0f;
        control.updated_qpc = QpcNow();
        std::memcpy(data + kControlOffset, &control, sizeof(control));
        auto* sequence = reinterpret_cast<volatile LONG64*>(data + kControlSequenceOffset);
        InterlockedExchange64(sequence, 1);
        ControlData read{};
        Check(!reader.ReadControl(read), "partially written control is not consumed");
        InterlockedExchange64(sequence, 2);
        Check(reader.ReadControl(read) && (read.flags & Enabled), "stable control snapshot is consumed");
        control.updated_qpc = 1;
        std::memcpy(data + kControlOffset, &control, sizeof(control));
        Check(reader.ReadControl(read) && !(read.flags & Enabled), "stale broker disables rendering");
        IntelData intel{};
        intel.magic = kIntelMagic;
        intel.version = 1;
        intel.available_mask = IntelGpuClock;
        intel.updated_qpc = QpcNow();
        intel.gpu_frequency_mhz = 2400.0; // Явный fixture, не источник рабочей телеметрии.
        std::memcpy(data + kIntelOffset, &intel, sizeof(intel));
        IntelData intel_read{};
        Check(reader.ReadIntel(intel_read) && Near(intel_read.gpu_frequency_mhz, 2400.0),
              "optional Intel block has independent versioned snapshot");
        intel.magic = 0;
        std::memcpy(data + kIntelOffset, &intel, sizeof(intel));
        Check(!reader.ReadIntel(intel_read), "uninitialized Intel data is not available");
        const auto timestamp = QpcNow();
        reader.ObservePresent(timestamp, Api::Dx12, 1280, 720);
        FrameData frame{};
        std::memcpy(&frame, data + kFrameOffset, sizeof(frame));
        Check(frame.present_count == 1 && frame.api == 2 && frame.width == 1280,
              "native producer publishes frame metadata at agreed offsets");
        Check((*reinterpret_cast<LONG64*>(data + kFrameSequenceOffset) & 1) == 0,
              "native producer commits an even sequence");
    }
    UnmapViewOfFile(data);
    CloseHandle(handle);
    SetEnvironmentVariableW(L"FRAMESCOPE_SESSION", original_length > 0 && original_length < 256 ? original : nullptr);
}
} // namespace

int main() {
    using namespace framescope;
    bool rejected = false;
    try { FrameStatistics invalid(0); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "zero frequency is rejected");
    FrameStatistics empty(1000);
    Check(empty.Snapshot().flags == 0, "empty stream has no invented FPS");
    empty.Observe(10);
    Check(empty.Snapshot().present_count == 1 && empty.Snapshot().flags == 0,
          "first present has insufficient intervals");
    empty.Observe(10);
    empty.Observe(9);
    Check(empty.Snapshot().present_count == 1, "duplicate/backward clocks are not frames");
    empty.Observe(20);
    Check(Near(empty.Snapshot().fps, 100), "FPS uses measured intervals");
    Check(!(empty.Snapshot().flags & HasLow1), "low needs enough samples");

    FrameStatistics uniform(1000000);
    for (std::uint64_t i = 0; i <= 1000; ++i) { uniform.Observe(i * 10000); }
    const auto uniform_data = uniform.Snapshot();
    Check(Near(uniform_data.fps, 100), "uniform FPS");
    Check(Near(uniform_data.frametime_ms, 10), "interval units are milliseconds");
    Check(Near(uniform_data.low_1, 100) && Near(uniform_data.low_01, 100), "uniform low values");
    Check((uniform_data.flags & (HasLow1 | HasLow01)) == (HasLow1 | HasLow01), "low availability flags");
    Check(uniform_data.graph_count == kGraphCapacity, "graph has bounded history");

    FrameStatistics spike(1000000);
    std::uint64_t timestamp = 0;
    spike.Observe(timestamp);
    for (int i = 0; i < 999; ++i) { timestamp += 10000; spike.Observe(timestamp); }
    spike.Observe(timestamp + 100000);
    const auto spike_data = spike.Snapshot();
    Check(Near(spike_data.low_01, 10), "0.1% low is inverse worst-tail mean");
    Check(Near(spike_data.low_1, 1000.0 / 19.0), "1% low averages slowest ten intervals, not FPS");
    Check(Near(spike_data.graph[spike_data.graph_count - 1], 100), "graph retains stutter");

    FrameStatistics rollover(1000000);
    for (std::uint64_t i = 0; i <= 15000; ++i) { rollover.Observe(i * 1000); }
    const auto rollover_data = rollover.Snapshot();
    Check(rollover_data.present_count == 15001, "bounded history retains lifetime frame count");
    Check(Near(rollover_data.low_01, 1000), "bounded history has stable lows");
    Check(sizeof(ControlData) == 128 && sizeof(FrameData) == 560, "ABI sizes");
    Check(ValidSession(L"0123456789abcdef0123456789abcdef"), "session identifiers accept only hex UUIDs");
    Check(!ValidSession(L"..\\different-process"), "session names cannot escape namespace");
    CheckSharedMemory();
    if (failures == 0) { std::cout << "Native statistics/protocol checks passed\n"; }
    return failures == 0 ? 0 : 1;
}
