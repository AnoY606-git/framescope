using System.Diagnostics;
using System.Text.Json;
using FrameScope.Core;

namespace FrameScope.App.Services;

internal static class IntelDiagnosticsRunner
{
    // Headless проверка настоящего controller: тот же exe/path/decoder/PCI match, без тестовых значений.
    public static async Task<int> RunAsync(int seconds)
    {
        await using var provider = new IntelProvider();
        using var metrics = new WindowsMetricService();
        provider.TryStart();
        var deadline = Environment.TickCount64 + seconds * 1000L;
        var lastState = "";
        while (Environment.TickCount64 < deadline)
        {
            var sample = await Task.Run(() => provider.Enrich(metrics.Collect()));
            var info = provider.Info;
            var intel = sample.GetIntelControl((ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency);
            var state = $"{info.State}:{sample.IntelStatus}";
            if (state != lastState)
            {
                Console.WriteLine(JsonSerializer.Serialize(new
                {
                    kind = "framescope-intel-controller", state = info.State.ToString(), helperPid = info.ProcessId,
                    selectedLuid = sample.GpuAdapterLuid, physicalAdapter = sample.GpuPhysicalAdapter,
                    pciBus = sample.GpuPciBus, pciDevice = sample.GpuPciDevice, pciFunction = sample.GpuPciFunction,
                    availableMask = intel is { } data ? (ulong)data.AvailableMask : 0,
                    gpuClockMhz = Available(intel, IntelAvailability.GpuClock, data => data.GpuFrequencyMhz),
                    memoryDriverEffectiveMhz = Available(intel, IntelAvailability.MemoryClock, data => data.MemoryFrequencyMhz),
                    gpuPowerWatts = Available(intel, IntelAvailability.GpuPower, data => data.GpuPowerWatts),
                    boardPowerWatts = Available(intel, IntelAvailability.BoardPower, data => data.BoardPowerWatts),
                    gpuTemperature = sample.GpuTemperature, temperatureSource = sample.GpuTemperatureSource,
                    status = sample.IntelStatus, diagnostics = sample.IntelDiagnostics,
                }));
                lastState = state;
            }
            if (intel is not null) return 0;
            if (info.State is IntelProviderState.Blocked or IntelProviderState.Missing or IntelProviderState.Faulted) return 2;
            await Task.Delay(500);
        }
        Console.WriteLine(JsonSerializer.Serialize(new { kind = "framescope-intel-controller", state = "Timeout", diagnostic = "Не получен свежий сопоставленный Intel snapshot за отведённое время." }));
        return 3;
    }
    private static double? Available(IntelControl? intel, IntelAvailability flag, Func<IntelControl, double> value) =>
        intel is { } data && data.AvailableMask.HasFlag(flag) ? value(data) : null;
}
