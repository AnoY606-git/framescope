using System.Globalization;

namespace FrameScope.Core;

public static class IntelSnapshotProjector
{
    public static MetricSample Enrich(MetricSample baseline, IntelJsonSnapshot? snapshot, long nowTickMs, ulong nowQpc, ulong frequency, string providerDiagnostic)
    {
        // Каждый проход начинается с чистого расширения, а не с предыдущих значений поставщика.
        var oldSysmanTemperature = baseline.GpuTemperatureSource.StartsWith("Sysman", StringComparison.Ordinal);
        var oldSysmanCapacity = baseline.VramCapacitySource.StartsWith("Sysman", StringComparison.Ordinal);
        var sample = baseline with
        {
            IntelControl = null, IntelAdapterLuid = null, IntelStatus = providerDiagnostic, IntelDiagnostics = providerDiagnostic,
            GpuTemperature = oldSysmanTemperature ? null : baseline.GpuTemperature,
            GpuTemperatureSource = oldSysmanTemperature ? "N/A" : baseline.GpuTemperatureSource,
            VramTotalGiB = oldSysmanCapacity ? null : baseline.VramTotalGiB,
            VramCapacitySource = oldSysmanCapacity ? "N/A" : baseline.VramCapacitySource,
        };
        MetricSample Missing(string why) => sample with { IntelStatus = $"Intel: N/A — {why}", IntelDiagnostics = $"{providerDiagnostic}\n{why}" };
        if (snapshot is null) return sample;
        if (snapshot.BackendStatus != "available") return Missing($"backend {snapshot.BackendStatus}: {snapshot.BackendReason}");
        if (snapshot.RequiresRestart) return Missing("Sysman DEVICE_LOST / DEVICE_REQUIRES_RESET: нужен новый helper-процесс, старые поля удалены; hardware reset не вызывается.");
        if (nowTickMs < 0 || snapshot.MonotonicMs > (ulong)nowTickMs || (ulong)nowTickMs - snapshot.MonotonicMs > IntelJsonDecoder.MaximumAgeMilliseconds
            || frequency == 0 || snapshot.UpdatedQpc == 0 || snapshot.UpdatedQpc > nowQpc
            || (nowQpc - snapshot.UpdatedQpc) / (double)frequency > IntelTelemetry.MaximumAgeSeconds) return Missing("snapshot устарел или нарушена идентичность времени; старые поля удалены.");
        if (!AdapterLuid.TryNormalize(sample.GpuAdapterLuid, out var luid) || sample.GpuAdapterIsAmbiguous || sample.GpuPhysicalAdapter != 0)
            return Missing("нет однозначного LUID / физического адаптера 0.");
        if (sample.GpuPciBus is not <= 255 || sample.GpuPciDevice is not <= 31 || sample.GpuPciFunction is not <= 7)
            return Missing("Windows не сообщил однозначный PCI bus/device/function выбранного LUID.");
        var matches = snapshot.Devices.Where(device => device.Pci is { Domain: 0 } pci && pci.Bus == sample.GpuPciBus
            && pci.Device == sample.GpuPciDevice && pci.Function == sample.GpuPciFunction).ToArray();
        if (matches.Length != 1 || matches[0].VendorId != 0x8086) return Missing("нет единственного Intel vendor 0x8086 с тем же PCI domain 0 / bus / device / function.");
        var device = matches[0];
        var diagnostics = new List<string> { $"{providerDiagnostic} Sysman {snapshot.Mode}, PCI {device.Pci!.Domain:x4}:{device.Pci.Bus:x2}:{device.Pci.Device:x2}.{device.Pci.Function:x}; sequence {snapshot.Sequence}." };
        double? Field(string label, string key, string scope, string unit, bool positive = false)
        {
            var readings = device.Readings.Where(reading => reading.Key == key && reading.Scope == scope && reading.SubdeviceId is null).ToArray();
            if (readings.Length != 1) { diagnostics.Add($"{label}: N/A — {(readings.Length == 0 ? "нет root-domain reading" : "неоднозначные root-domain instances")}."); return null; }
            var reading = readings[0];
            if (reading.Status != "available" || reading.Value is not double value)
            { diagnostics.Add($"{label}: N/A — {reading.Status}, {reading.Reason ?? "нет значения"}."); return null; }
            if (reading.Unit != unit || !double.IsFinite(value) || value < 0 || positive && value == 0)
            { diagnostics.Add($"{label}: N/A — invalid value/unit ({reading.Unit}; требуется {unit})."); return null; }
            diagnostics.Add($"{label}: {value.ToString("G8", CultureInfo.InvariantCulture)} {unit} · {reading.Instance}"
                + (reading.WindowMs is { } window ? $" · API window {window.ToString("G8", CultureInfo.InvariantCulture)} ms" : "")
                + (reading.Note is { } note ? $" · {note}" : ""));
            return value;
        }
        var gpuClock = Field("GPU clock", "frequency.actual", "gpu", "MHz");
        var memoryClock = Field("Memory rate (driver-reported/effective, не physical oscillator)", "frequency.actual", "memory", "MHz");
        var gpuPower = Field("GPU-chip power", "power.average", "gpu", "W");
        var boardPower = Field("Card/board power", "power.average", "card", "W");
        var voltage = Field("GPU voltage", "voltage", "gpu", "V");
        var memoryTemp = Field("Memory temperature", "temperature", "memory_max", "C");
        var gpuTemp = Field("GPU temperature (Sysman gpu_max)", "temperature", "gpu_max", "C");
        var read = Field("Memory read bandwidth", "memory.read_bandwidth", "device_allocatable", "B/s");
        var write = Field("Memory write bandwidth", "memory.write_bandwidth", "device_allocatable", "B/s");
        var throttle = Field("GPU throttle flags", "frequency.throttle_reasons", "gpu", "bitmask");
        if (throttle is { } bits && (Math.Truncate(bits) != bits || bits > uint.MaxValue)) { throttle = null; diagnostics.Add("GPU throttle: N/A — bitmask должен быть целым uint32."); }
        var physical = Field("Physical device memory", "memory.physical_bytes", "device_physical", "B", positive: true);
        if (physical is { } capacity && (capacity >= 1UL << 50 || Math.Truncate(capacity) != capacity)) physical = null;
        diagnostics.Add("Fan: N/A — read_only_policy_windows_sysman_fan_getters_can_write; getter не вызывается, RPM не подставляется.");
        diagnostics.Add("Package power не переименовывается в GPU/card power. Allocatable memory не подменяет physical capacity; PDH usage не изменяется.");
        var control = new IntelControl { Magic = NativeProtocol.IntelMagic, Version = 1, UpdatedQpc = snapshot.UpdatedQpc };
        void Set(IntelAvailability bit, double? value, Action<double> assign)
        { if (value is { } n) { control.AvailableMask |= bit; assign(n); } }
        Set(IntelAvailability.GpuClock, gpuClock, n => control.GpuFrequencyMhz = n);
        Set(IntelAvailability.MemoryClock, memoryClock, n => control.MemoryFrequencyMhz = n);
        Set(IntelAvailability.GpuPower, gpuPower, n => control.GpuPowerWatts = n);
        Set(IntelAvailability.BoardPower, boardPower, n => control.BoardPowerWatts = n);
        Set(IntelAvailability.Voltage, voltage, n => control.VoltageVolts = n);
        Set(IntelAvailability.MemoryTemperature, memoryTemp, n => control.MemoryTemperature = n);
        Set(IntelAvailability.MemoryRead, read, n => control.MemoryReadGbps = n / 1e9);
        Set(IntelAvailability.MemoryWrite, write, n => control.MemoryWriteGbps = n / 1e9);
        Set(IntelAvailability.Throttle, throttle, n => control.ThrottleFlags = (ulong)n);
        var validGpuTemp = gpuTemp is >= 0 and <= 200;
        return sample with
        {
            IntelControl = control, IntelAdapterLuid = luid,
            IntelStatus = $"LIVE · Sysman {snapshot.Mode} · PCI 0000:{device.Pci.Bus:x2}:{device.Pci.Device:x2}.{device.Pci.Function:x} · sequence {snapshot.Sequence}",
            IntelDiagnostics = string.Join(Environment.NewLine, diagnostics),
            GpuTemperature = validGpuTemp ? gpuTemp : sample.GpuTemperature,
            GpuTemperatureSource = validGpuTemp ? "Sysman gpu_max" : sample.GpuTemperatureSource,
            VramTotalGiB = sample.VramTotalGiB is > 0 ? sample.VramTotalGiB : physical / (1024 * 1024 * 1024d),
            VramCapacitySource = sample.VramTotalGiB is > 0 ? sample.VramCapacitySource : physical is not null ? "Sysman physical" : sample.VramCapacitySource,
        };
    }
}
