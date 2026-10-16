using System.Diagnostics;
using System.Runtime.InteropServices;
using FrameScope.Core;
using FrameScope.WindowsSensors;

namespace FrameScope.App.Services;

internal sealed class WindowsMetricService : IDisposable
{
    private readonly CpuUsageCalculator _cpu = new();
    private readonly PdhGpuCounters _gpu = new();
    private const double GiB = 1024 * 1024 * 1024d;

    // Весь сбор выполняется рабочим потоком; UI получает только готовый неизменяемый снимок.
    public MetricSample Collect()
    {
        CpuUsage cpu = default;
        var cpuDiagnostic = "CPU: GetSystemTimes недоступен.";
        if (GetSystemTimes(out var idle, out var kernel, out var user))
        {
            var processors = ReadProcessorTimes();
            cpu = _cpu.Sample(new(idle, kernel, user), processors);
            cpuDiagnostic = cpu.Total is null ? "CPU: прогрев первого измерения." :
                processors.Count == 0 ? "CPU: общий доступен, логические процессоры N/A." : "CPU: общий / самый занятый логический процессор.";
        }
        else _cpu.Reset();

        var memory = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
        var hasMemory = GlobalMemoryStatusEx(ref memory) && memory.TotalPhysical > 0 && memory.AvailablePhysical <= memory.TotalPhysical;
        var gpu = _gpu.Collect();
        var wddm = MatchWddm(gpu);
        return new()
        {
            CpuUsage = cpu.Total, CpuMax = cpu.MaximumLogicalProcessor,
            GpuUsage = gpu.Selected?.Usage,
            GpuAdapterLuid = gpu.AdapterLuid,
            GpuPhysicalAdapter = gpu.Selected?.Adapter.PhysicalIndex,
            GpuAdapterIsAmbiguous = gpu.IsLuidAmbiguous,
            GpuPciBus = wddm.Reading?.PciBus,
            GpuPciDevice = wddm.Reading?.PciDevice,
            GpuPciFunction = wddm.Reading?.PciFunction,
            GpuTemperature = wddm.Reading?.TemperatureCelsius,
            GpuTemperatureSource = wddm.Reading?.TemperatureCelsius is not null ? "WDDM" : "N/A (WDDM)",
            WddmMemoryFrequencyHz = wddm.Reading?.MemoryFrequencyHz,
            RamUsedGiB = hasMemory ? (memory.TotalPhysical - memory.AvailablePhysical) / GiB : null,
            RamTotalGiB = hasMemory ? memory.TotalPhysical / GiB : null,
            VramUsedGiB = gpu.Selected?.DedicatedUsageBytes / GiB,
            // Размер выделенной видеопамяти — метаданные того же LUID, не бюджет вызывающего процесса.
            VramTotalGiB = wddm.Reading?.DedicatedVideoMemoryBytes is { } capacity ? capacity / GiB : null,
            VramCapacitySource = wddm.Reading?.DedicatedVideoMemoryBytes is > 0 ? "WDDM dedicated (driver-reported)" : "N/A",
            TimestampQpc = (ulong)Stopwatch.GetTimestamp(),
            Diagnostics = $"{cpuDiagnostic}\n{_gpu.Diagnostics}\n{wddm.Diagnostic}\nWDDM не сообщает мощность в ваттах: поле Power не используется как W. CPU temperature: N/A.",
        };
    }

    private static (WddmGpuReading? Reading, string Diagnostic) MatchWddm(GpuCounterSelection gpu)
    {
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 17134)) return (null, "WDDM telemetry: N/A — нужна Windows 10 1803 или новее.");
        var readings = WddmGpuSensor.ReadAll();
        if (gpu.AdapterLuid is null) return (null, "WDDM telemetry: N/A — нет выбранного LUID от PDH.");
        if (gpu.IsLuidAmbiguous) return (null, "WDDM telemetry: N/A — выбранный физический адаптер нельзя однозначно сопоставить с WDDM phys 0 по LUID.");
        var match = AdapterLuid.SelectUnique(gpu.AdapterLuid, gpu.IsLuidAmbiguous, readings, reading => reading.AdapterLuid);
        if (match is null) return (null, $"WDDM telemetry: N/A — нет единственного точного совпадения LUID {gpu.AdapterLuid}.");
        return (match, $"WDDM: точное совпадение LUID {gpu.AdapterLuid}; GPU temperature: {(match.TemperatureCelsius is null ? "N/A — драйвер не сообщил" : "доступна")}; "
            + $"memory frequency: {(match.MemoryFrequencyHz is null ? "N/A — драйвер не сообщил" : "доступна (Hz, показана отдельно)")}.");
    }

    private static IReadOnlyList<ProcessorTimes> ReadProcessorTimes()
    {
        var count = GetActiveProcessorCount(0xFFFF);
        if (count == 0 || count > 4096) return [];
        var stride = Marshal.SizeOf<ProcessorPerformance>();
        var bytes = checked((int)count * stride);
        var buffer = Marshal.AllocHGlobal(bytes);
        try
        {
            var status = NtQuerySystemInformation(8, buffer, (uint)bytes, out var returned);
            if (status < 0 || returned == 0 || returned > bytes || returned % stride != 0) return [];
            var result = new ProcessorTimes[returned / stride];
            for (var i = 0; i < result.Length; i++)
            {
                var value = Marshal.PtrToStructure<ProcessorPerformance>(IntPtr.Add(buffer, i * stride));
                if (value.Idle < 0 || value.Kernel < 0 || value.User < 0) return [];
                result[i] = new((ulong)value.Idle, (ulong)value.Kernel, (ulong)value.User);
            }
            return result;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    public void Dispose() => _gpu.Dispose();

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessorPerformance
    {
        public long Idle, Kernel, User, Dpc, Interrupt;
        public uint InterruptCount;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatus
    {
        public uint Length, Load;
        public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile, TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
    }
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);
    [DllImport("kernel32.dll")] private static extern uint GetActiveProcessorCount(ushort groupNumber);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
    [DllImport("ntdll.dll")] private static extern int NtQuerySystemInformation(int informationClass, IntPtr information, uint length, out uint returnedLength);
}
