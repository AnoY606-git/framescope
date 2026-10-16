#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace framescope {

inline constexpr std::uint32_t kMagic = 0x46534350;
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMappingSize = 4096;
inline constexpr std::size_t kControlSequenceOffset = 24;
inline constexpr std::size_t kControlOffset = 32;
inline constexpr std::size_t kFrameSequenceOffset = 256;
inline constexpr std::size_t kFrameOffset = 264;
inline constexpr std::size_t kGraphCapacity = 120;
inline constexpr std::size_t kIntelSequenceOffset = 1016;
inline constexpr std::size_t kIntelOffset = 1024;
inline constexpr std::uint32_t kIntelMagic = 0x494E544C;

enum HudFlags : std::uint32_t {
    Enabled = 1u << 0,
    Horizontal = 1u << 1,
    Graph = 1u << 2,
    ShowFps = 1u << 3,
    ShowFrametime = 1u << 4,
    ShowCpu = 1u << 5,
    ShowGpu = 1u << 6,
    ShowRam = 1u << 7,
    ShowVram = 1u << 8,
    ShowLows = 1u << 9,
    ShowTemperatures = 1u << 10,
    ShowIntelClocks = 1u << 11,
    ShowIntelPower = 1u << 12,
    ShowIntelBandwidth = 1u << 13,
    ShowIntelThrottle = 1u << 14,
};

enum Availability : std::uint32_t {
    Cpu = 1u << 0,
    CpuMax = 1u << 1,
    Gpu = 1u << 2,
    Ram = 1u << 3,
    Vram = 1u << 4,
    GpuTemperature = 1u << 5,
    CpuTemperature = 1u << 6,
    GpuPower = 1u << 7,
};

enum class Api : std::uint32_t { Unknown = 0, Dx11 = 1, Dx12 = 2, Vulkan = 3 };
enum FrameFlags : std::uint32_t { HasFps = 1u << 0, HasLow1 = 1u << 1, HasLow01 = 1u << 2 };

enum IntelAvailability : std::uint64_t {
    IntelGpuClock = 1ull << 0, IntelMemoryClock = 1ull << 1,
    IntelGpuPower = 1ull << 2, IntelBoardPower = 1ull << 3,
    IntelFan = 1ull << 4, IntelVoltage = 1ull << 5,
    IntelMemoryRead = 1ull << 6, IntelMemoryWrite = 1ull << 7,
    IntelMemoryTemperature = 1ull << 8, IntelThrottle = 1ull << 9,
};

#pragma pack(push, 8)
struct ControlData {
    std::uint32_t flags;
    std::uint32_t corner;
    float scale;
    float opacity;
    float margin_x;
    float margin_y;
    std::uint32_t text_color;
    std::uint32_t accent_color;
    std::uint32_t background_color;
    std::uint32_t available_mask;
    double cpu_usage;
    double cpu_max;
    double gpu_usage;
    double ram_used_gib;
    double ram_total_gib;
    double vram_used_gib;
    double vram_total_gib;
    double gpu_temperature;
    double cpu_temperature;
    double gpu_power_watts;
    std::uint64_t updated_qpc;
};

struct FrameData {
    std::uint64_t present_count;
    std::uint64_t timestamp_qpc;
    double fps;
    double frametime_ms;
    double low_1;
    double low_01;
    double hud_cpu_us;
    std::uint32_t api;
    std::uint32_t flags;
    std::uint32_t graph_count;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t process_id;
    float graph[kGraphCapacity];
};

// Необязательное расширение v1: прежние участники протокола игнорируют этот отдельный блок.
struct IntelData {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t available_mask;
    std::uint64_t updated_qpc;
    double gpu_frequency_mhz;
    double memory_frequency_mhz;
    double gpu_power_watts;
    double board_power_watts;
    double fan_rpm;
    double voltage_volts;
    double memory_read_gbps;
    double memory_write_gbps;
    double memory_temperature;
    std::uint64_t throttle_flags;
    std::uint64_t reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(ControlData) == 128);
static_assert(offsetof(ControlData, cpu_usage) == 40);
static_assert(offsetof(ControlData, updated_qpc) == 120);
static_assert(sizeof(FrameData) == 560);
static_assert(offsetof(FrameData, graph) == 80);
static_assert(sizeof(IntelData) == 128);

inline std::wstring MappingName(const std::wstring& session) {
    return L"Local\\FrameScope-" + session;
}

inline std::wstring EventName(const std::wstring& session, const wchar_t* suffix) {
    return MappingName(session) + L"-" + suffix;
}

bool ValidSession(const std::wstring& session);
std::wstring SessionFromEnvironment();
std::uint64_t QpcNow();
std::uint64_t QpcFrequency();

} // namespace framescope
