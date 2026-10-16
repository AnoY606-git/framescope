using System.Buffers.Binary;
using System.Runtime.InteropServices;
using FrameScope.Core;

internal static class IntelTelemetryTests
{
    private const string Luid = "00000000:000000aa";
    public static IEnumerable<(string Name, Func<Task> Run)> Cases =>
    [
        Test("Intel optional block keeps legacy 128/560 byte layouts", Layout),
        Test("Intel encoder writes exact native field offsets", Encoding),
        Test("Intel encoder rejects short buffers", ShortBuffer),
        Test("Intel telemetry rejects missing, malformed, stale and future data", Freshness),
        Test("Intel telemetry masks invalid or unavailable values", InvalidValues),
        Test("Intel throttle zero remains genuine available data", ZeroThrottle),
        Test("Intel sample publication requires exact unambiguous adapter identity", AdapterGuard),
        Test("Intel timestamp is never refreshed by validation", PreserveTimestamp),
        ("Intel visibility flags round-trip independently per profile", SettingsRoundTrip),
    ];
    private static (string, Func<Task>) Test(string name, Action action) => (name, () => { action(); return Task.CompletedTask; });
    private static void Check(bool value) { if (!value) throw new InvalidOperationException("Assertion failed"); }
    private static void Equal<T>(T expected, T actual) { if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new InvalidOperationException($"Expected {expected}; got {actual}"); }
    private static IntelControl Sample() => new()
    {
        Magic = NativeProtocol.IntelMagic, Version = 1, UpdatedQpc = 9500,
        AvailableMask = IntelAvailability.GpuClock | IntelAvailability.MemoryClock | IntelAvailability.GpuPower | IntelAvailability.BoardPower
            | IntelAvailability.Fan | IntelAvailability.Voltage | IntelAvailability.MemoryRead | IntelAvailability.MemoryWrite | IntelAvailability.MemoryTemperature | IntelAvailability.Throttle,
        GpuFrequencyMhz = 1800, MemoryFrequencyMhz = 2000, GpuPowerWatts = 85.5, BoardPowerWatts = 100,
        FanRpm = 1200, VoltageVolts = 1.05, MemoryReadGbps = 100, MemoryWriteGbps = 30, MemoryTemperature = 60, ThrottleFlags = 42,
    };
    private static void Layout()
    {
        Equal(128, Marshal.SizeOf<HudControl>()); Equal(560, NativeProtocol.FrameSize); Equal(128, Marshal.SizeOf<IntelControl>());
        Equal(1016, NativeProtocol.IntelSequenceOffset); Equal(1024, NativeProtocol.IntelOffset);
        Equal(16, Marshal.OffsetOf<IntelControl>(nameof(IntelControl.UpdatedQpc)).ToInt32());
        Equal(24, Marshal.OffsetOf<IntelControl>(nameof(IntelControl.GpuFrequencyMhz)).ToInt32());
        Equal(96, Marshal.OffsetOf<IntelControl>(nameof(IntelControl.ThrottleFlags)).ToInt32());
        Check(NativeProtocol.IntelOffset > NativeProtocol.FrameOffset + NativeProtocol.FrameSize);
        Check(NativeProtocol.IntelOffset + IntelControlEncoder.Size < NativeProtocol.MappingSize);
    }
    private static void Encoding()
    {
        var bytes = new byte[128]; var sample = Sample(); IntelControlEncoder.Write(bytes, sample);
        Equal(NativeProtocol.IntelMagic, BinaryPrimitives.ReadUInt32LittleEndian(bytes)); Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(4)));
        Equal((ulong)sample.AvailableMask, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8))); Equal(9500ul, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16)));
        var expected = new[] { 1800d, 2000, 85.5, 100, 1200, 1.05, 100, 30, 60 };
        for (var i = 0; i < expected.Length; i++) Equal(expected[i], BinaryPrimitives.ReadDoubleLittleEndian(bytes.AsSpan(24 + 8 * i)));
        Equal(42ul, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(96)));
        Equal(0ul, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(120)));
    }
    private static void ShortBuffer()
    {
        try { IntelControlEncoder.Write(new byte[127], Sample()); throw new InvalidOperationException("Expected short-buffer error"); }
        catch (ArgumentException) { }
    }
    private static void Freshness()
    {
        Check(IntelTelemetry.Validate(null, 10000, 1000) is null);
        var sample = Sample(); Check(IntelTelemetry.Validate(sample, 10000, 1000) is not null);
        Check(IntelTelemetry.Validate(sample, 10000, 0) is null); Check(IntelTelemetry.Validate(sample, 15001, 1000) is null);
        Check(IntelTelemetry.Validate(sample, 9000, 1000) is null);
        sample.Magic = 0; Check(IntelTelemetry.Validate(sample, 10000, 1000) is null);
        sample = Sample(); sample.Version = 2; Check(IntelTelemetry.Validate(sample, 10000, 1000) is null);
        sample = Sample(); sample.UpdatedQpc = 0; Check(IntelTelemetry.Validate(sample, 10000, 1000) is null);
    }
    private static void InvalidValues()
    {
        var sample = Sample(); sample.GpuFrequencyMhz = double.NaN; sample.GpuPowerWatts = -1; sample.MemoryWriteGbps = double.PositiveInfinity;
        sample.Reserved1 = 100; sample.AvailableMask |= (IntelAvailability)(1ul << 63);
        var valid = IntelTelemetry.Validate(sample, 10000, 1000)!.Value;
        Check(!valid.AvailableMask.HasFlag(IntelAvailability.GpuClock)); Check(!valid.AvailableMask.HasFlag(IntelAvailability.GpuPower));
        Check(!valid.AvailableMask.HasFlag(IntelAvailability.MemoryWrite)); Equal(0d, valid.GpuFrequencyMhz); Equal(0ul, valid.Reserved1);
        Equal(0ul, (ulong)valid.AvailableMask & (1ul << 63));
        sample = Sample(); sample.AvailableMask = 0;
        valid = IntelTelemetry.Validate(sample, 10000, 1000)!.Value; Equal((IntelAvailability)0, valid.AvailableMask); Equal(0d, valid.GpuPowerWatts); Equal(0ul, valid.ThrottleFlags);
    }
    private static void ZeroThrottle()
    {
        var sample = Sample(); sample.ThrottleFlags = 0;
        var validated = IntelTelemetry.Validate(sample, 10000, 1000)!.Value;
        Check(validated.AvailableMask.HasFlag(IntelAvailability.Throttle)); Equal(0ul, validated.ThrottleFlags);
    }
    private static void AdapterGuard()
    {
        var sample = new MetricSample { GpuAdapterLuid = Luid, IntelAdapterLuid = Luid.ToUpperInvariant(), IntelControl = Sample() };
        Check(sample.GetIntelControl(10000, 1000) is not null);
        Check((sample with { IntelAdapterLuid = "00000000:000000bb" }).GetIntelControl(10000, 1000) is null);
        Check((sample with { IntelAdapterLuid = null }).GetIntelControl(10000, 1000) is null);
        Check((sample with { GpuAdapterIsAmbiguous = true }).GetIntelControl(10000, 1000) is null);
        Check((sample with { GpuPhysicalAdapter = 1 }).GetIntelControl(10000, 1000) is null);
        Check(MetricSample.Empty.GetIntelControl(10000, 1000) is null);
    }
    private static void PreserveTimestamp()
    {
        Equal(9500ul, IntelTelemetry.Validate(Sample(), 12000, 1000)!.Value.UpdatedQpc);
        Check(IntelTelemetry.Validate(Sample(), 15000, 1000) is null);
    }
    private static async Task SettingsRoundTrip()
    {
        var directory = Path.Combine(Path.GetTempPath(), "opencode", "FrameScope.Tests", Guid.NewGuid().ToString("N")); Directory.CreateDirectory(directory);
        try
        {
            var profile = new HudProfile { ShowIntelClocks = true, ShowIntelPower = true, ShowIntelBandwidth = true, ShowIntelThrottle = true };
            var flags = profile.Flags;
            Check(flags.HasFlag(HudFlags.ShowIntelClocks) && flags.HasFlag(HudFlags.ShowIntelPower) && flags.HasFlag(HudFlags.ShowIntelBandwidth) && flags.HasFlag(HudFlags.ShowIntelThrottle));
            var store = new SettingsStore(Path.Combine(directory, "settings.json"));
            await store.SaveAsync(new() { Profiles = [profile, new() { Id = "other" }] });
            var loaded = await store.LoadAsync(); Equal(flags, loaded.Settings.Profiles[0].Flags);
            Check(!loaded.Settings.Profiles[1].ShowIntelClocks && !loaded.Settings.Profiles[1].ShowIntelPower
                && !loaded.Settings.Profiles[1].ShowIntelBandwidth && !loaded.Settings.Profiles[1].ShowIntelThrottle);
        }
        finally { Directory.Delete(directory, recursive: true); }
    }
}
