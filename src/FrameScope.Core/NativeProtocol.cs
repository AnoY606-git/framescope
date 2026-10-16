using System.Runtime.InteropServices;

namespace FrameScope.Core;

public static class NativeProtocol
{
    public const uint Magic = 0x46534350;
    public const uint Version = 1;
    public const int MappingSize = 4096;
    public const int ControlSequenceOffset = 24;
    public const int ControlOffset = 32;
    public const int FrameSequenceOffset = 256;
    public const int FrameOffset = 264;
    public const int FrameSize = 560;
    public const int GraphCapacity = 120;
    public const int IntelSequenceOffset = 1016;
    public const int IntelOffset = 1024;
    public const uint IntelMagic = 0x494E544C;

    public static string MappingName(string session) => $@"Local\FrameScope-{session}";
}

[Flags]
public enum HudFlags : uint
{
    Enabled = 1 << 0, Horizontal = 1 << 1, Graph = 1 << 2,
    ShowFps = 1 << 3, ShowFrametime = 1 << 4, ShowCpu = 1 << 5,
    ShowGpu = 1 << 6, ShowRam = 1 << 7, ShowVram = 1 << 8,
    ShowLows = 1 << 9, ShowTemperatures = 1 << 10,
    ShowIntelClocks = 1 << 11, ShowIntelPower = 1 << 12,
    ShowIntelBandwidth = 1 << 13, ShowIntelThrottle = 1 << 14,
}

[Flags]
public enum MetricAvailability : uint
{
    Cpu = 1 << 0, CpuMax = 1 << 1, Gpu = 1 << 2, Ram = 1 << 3,
    Vram = 1 << 4, GpuTemperature = 1 << 5, CpuTemperature = 1 << 6, GpuPower = 1 << 7,
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct HudControl
{
    public HudFlags Flags;
    public uint Corner;
    public float Scale;
    public float Opacity;
    public float MarginX;
    public float MarginY;
    public uint TextColor;
    public uint AccentColor;
    public uint BackgroundColor;
    public MetricAvailability AvailableMask;
    public double CpuUsage;
    public double CpuMax;
    public double GpuUsage;
    public double RamUsedGiB;
    public double RamTotalGiB;
    public double VramUsedGiB;
    public double VramTotalGiB;
    public double GpuTemperature;
    public double CpuTemperature;
    public double GpuPowerWatts;
    public ulong UpdatedQpc;
}

[Flags]
public enum IntelAvailability : ulong
{
    GpuClock = 1UL << 0, MemoryClock = 1UL << 1, GpuPower = 1UL << 2,
    BoardPower = 1UL << 3, Fan = 1UL << 4, Voltage = 1UL << 5,
    MemoryRead = 1UL << 6, MemoryWrite = 1UL << 7,
    MemoryTemperature = 1UL << 8, Throttle = 1UL << 9,
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct IntelControl
{
    public uint Magic;
    public uint Version;
    public IntelAvailability AvailableMask;
    public ulong UpdatedQpc;
    public double GpuFrequencyMhz;
    public double MemoryFrequencyMhz;
    public double GpuPowerWatts;
    public double BoardPowerWatts;
    public double FanRpm;
    public double VoltageVolts;
    public double MemoryReadGbps;
    public double MemoryWriteGbps;
    public double MemoryTemperature;
    public ulong ThrottleFlags;
    public ulong Reserved1;
    public ulong Reserved2;
    public ulong Reserved3;
}
