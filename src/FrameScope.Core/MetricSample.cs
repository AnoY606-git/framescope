namespace FrameScope.Core;

public sealed record MetricSample
{
    public static MetricSample Empty { get; } = new();
    public double? CpuUsage { get; init; }
    public double? CpuMax { get; init; }
    public double? GpuUsage { get; init; }
    public double? RamUsedGiB { get; init; }
    public double? RamTotalGiB { get; init; }
    public double? VramUsedGiB { get; init; }
    public double? VramTotalGiB { get; init; }
    public double? GpuTemperature { get; init; }
    public string GpuTemperatureSource { get; init; } = "N/A";
    public double? CpuTemperature { get; init; }
    public double? GpuPowerWatts { get; init; }
    public string? GpuAdapterLuid { get; init; }
    public uint? GpuPhysicalAdapter { get; init; }
    public uint? GpuPciBus { get; init; }
    public uint? GpuPciDevice { get; init; }
    public uint? GpuPciFunction { get; init; }
    public bool GpuAdapterIsAmbiguous { get; init; }
    public ulong? WddmMemoryFrequencyHz { get; init; }
    public IntelControl? IntelControl { get; init; }
    public string? IntelAdapterLuid { get; init; }
    public string IntelDiagnostics { get; init; } = "";
    public string IntelStatus { get; init; } = "Intel: N/A — ожидание поставщика.";
    public string VramCapacitySource { get; init; } = "N/A";
    public ulong TimestampQpc { get; init; }
    public string Diagnostics { get; init; } = "Ожидание первого измерения.";

    public IntelControl? GetIntelControl(ulong nowQpc, ulong frequency) => !GpuAdapterIsAmbiguous && GpuPhysicalAdapter is not > 0
        && AdapterLuid.Matches(GpuAdapterLuid, IntelAdapterLuid) ? IntelTelemetry.Validate(IntelControl, nowQpc, frequency) : null;

    public MetricAvailability AvailableMask =>
        (Valid(CpuUsage, 100) ? MetricAvailability.Cpu : 0)
        | (Valid(CpuMax, 100) ? MetricAvailability.CpuMax : 0)
        | (Valid(GpuUsage, 100) ? MetricAvailability.Gpu : 0)
        | (Valid(RamUsedGiB) && Valid(RamTotalGiB) && RamTotalGiB > 0 && RamUsedGiB <= RamTotalGiB ? MetricAvailability.Ram : 0)
        | (Valid(VramUsedGiB) ? MetricAvailability.Vram : 0)
        | (Valid(GpuTemperature, 200) ? MetricAvailability.GpuTemperature : 0)
        | (Valid(CpuTemperature, 200) ? MetricAvailability.CpuTemperature : 0)
        | (Valid(GpuPowerWatts, 5000) ? MetricAvailability.GpuPower : 0);

    public HudControl ToControl(HudProfile profile)
    {
        profile = profile.Normalize();
        return new()
        {
            Flags = profile.Flags, Corner = (uint)profile.Corner, Scale = profile.Scale,
            Opacity = profile.Opacity, MarginX = profile.MarginX, MarginY = profile.MarginY,
            TextColor = HudProfile.PackColor(profile.TextColor), AccentColor = HudProfile.PackColor(profile.AccentColor),
            BackgroundColor = HudProfile.PackColor(profile.BackgroundColor), AvailableMask = AvailableMask,
            CpuUsage = Safe(CpuUsage), CpuMax = Safe(CpuMax), GpuUsage = Safe(GpuUsage),
            RamUsedGiB = Safe(RamUsedGiB), RamTotalGiB = Safe(RamTotalGiB),
            VramUsedGiB = Safe(VramUsedGiB), VramTotalGiB = Safe(VramTotalGiB),
            GpuTemperature = Safe(GpuTemperature), CpuTemperature = Safe(CpuTemperature),
            GpuPowerWatts = Safe(GpuPowerWatts), UpdatedQpc = TimestampQpc,
        };
    }

    private static bool Valid(double? value, double maximum = double.MaxValue) => value is double v && double.IsFinite(v) && v >= 0 && v <= maximum;
    private static double Safe(double? value) => Valid(value) ? value!.Value : 0;
}
