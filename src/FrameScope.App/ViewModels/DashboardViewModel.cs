using System.Globalization;
using System.Diagnostics;
using FrameScope.App.Services;
using FrameScope.Core;

namespace FrameScope.App.ViewModels;

internal sealed class DashboardViewModel : ObservableObject
{
    private MetricSample _metrics = MetricSample.Empty;
    private FrameSnapshot? _frame;
    private IntelControl? _intel;
    private string _status = "Демо не запущено";
    private string _log = "";
    public string Fps => Number(_frame?.Fps, "0");
    public string Frametime => Number(_frame?.Fps is not null ? _frame.FrametimeMs : null, "0.00");
    public string Cpu => Number(Has(MetricAvailability.Cpu) ? _metrics.CpuUsage : null, "0");
    public string CpuDetail => $"Макс. логический: {Number(Has(MetricAvailability.CpuMax) ? _metrics.CpuMax : null, "0")} %";
    public string Gpu => Number(Has(MetricAvailability.Gpu) ? _metrics.GpuUsage : null, "0");
    public string Ram => Number(Has(MetricAvailability.Ram) ? _metrics.RamUsedGiB : null, "0.0");
    public string RamDetail => $"из {Number(_metrics.RamTotalGiB, "0.0")} GiB · физическая память";
    public string Vram => Number(Has(MetricAvailability.Vram) ? _metrics.VramUsedGiB : null, "0.00");
    public string VramDetail => _metrics.VramTotalGiB is > 0 ? $"из {Number(_metrics.VramTotalGiB, "0.00")} GiB · {_metrics.VramCapacitySource}" : "Ёмкость N/A · выбранный адаптер";
    public string GpuAdapter => _metrics.GpuAdapterLuid is { } luid
        ? $"LUID {luid} · phys {_metrics.GpuPhysicalAdapter?.ToString(CultureInfo.InvariantCulture) ?? "N/A"}" : "Адаптер не выбран: нет валидного LUID от PDH.";
    public string GpuTemperature => Number(Has(MetricAvailability.GpuTemperature) ? _metrics.GpuTemperature : null, "0.0");
    public string GpuTemperatureLabel => $"GPU TEMP · °C · {_metrics.GpuTemperatureSource}";
    public string IntelStatus => _metrics.IntelStatus;
    public string WddmMemoryClock => Number(_metrics.WddmMemoryFrequencyHz is { } hz ? hz / 1_000_000d : null, "0.0");
    public string IntelGpuClock => IntelNumber(IntelAvailability.GpuClock, data => data.GpuFrequencyMhz, "0");
    public string IntelMemoryClock => IntelNumber(IntelAvailability.MemoryClock, data => data.MemoryFrequencyMhz, "0");
    public string IntelGpuPower => IntelNumber(IntelAvailability.GpuPower, data => data.GpuPowerWatts, "0.0");
    public string IntelBoardPower => IntelNumber(IntelAvailability.BoardPower, data => data.BoardPowerWatts, "0.0");
    public string IntelBandwidth => IntelNumber(IntelAvailability.MemoryRead, data => data.MemoryReadGbps, "0.0") + " / "
        + IntelNumber(IntelAvailability.MemoryWrite, data => data.MemoryWriteGbps, "0.0");
    public string IntelThrottle => _intel is { } data && data.AvailableMask.HasFlag(IntelAvailability.Throttle) ? $"0x{data.ThrottleFlags:X}" : "N/A";
    public string IntelFan => IntelNumber(IntelAvailability.Fan, data => data.FanRpm, "0");
    public string IntelVoltage => IntelNumber(IntelAvailability.Voltage, data => data.VoltageVolts, "0.000");
    public string IntelMemoryTemperature => IntelNumber(IntelAvailability.MemoryTemperature, data => data.MemoryTemperature, "0.0");
    public string IntelDiagnostics
    {
        get
        {
            if (!string.IsNullOrWhiteSpace(_metrics.IntelDiagnostics)) return _metrics.IntelDiagnostics;
            var reason = _intel is not null ? "Получен свежий Intel-блок для выбранного LUID. Доступность каждого поля — по маске поставщика."
                : _metrics.IntelControl is null ? "Intel advanced: N/A — поставщик расширенных метрик не подключён."
                : _metrics.GpuAdapterIsAmbiguous || _metrics.GpuPhysicalAdapter is > 0 ? "Intel advanced: N/A — неоднозначный физический адаптер."
                : !AdapterLuid.Matches(_metrics.GpuAdapterLuid, _metrics.IntelAdapterLuid) ? "Intel advanced: N/A — LUID поставщика не совпадает с выбранным GPU."
                : "Intel advanced: N/A — блок устарел или не прошёл проверку протокола.";
            return reason;
        }
    }
    public string Low1 => Number(_frame?.Low1, "0.0");
    public string Low01 => Number(_frame?.Low01, "0.0");
    public string HudCost => _frame is null ? "N/A" : Number(_frame.HudCpuMicroseconds, "0.0") + " μs CPU";
    public string PresentCount => _frame?.PresentCount.ToString("N0", CultureInfo.CurrentCulture) ?? "N/A";
    public string Status => _status;
    public string Diagnostics => _metrics.Diagnostics;
    public string NativeLog => string.IsNullOrWhiteSpace(_log) ? "Журнал пока пуст. Запустите собственное демо." : _log;
    public IReadOnlyList<float> Graph => _frame?.Graph ?? Array.Empty<float>();
    public string FrameState { get; private set; } = "Нет данных";

    public void Update(RuntimeUpdate update)
    {
        _metrics = update.Metrics; _frame = update.Frame; _status = update.Status; _log = update.Log;
        _intel = _metrics.GetIntelControl((ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency);
        FrameState = update.FrameStatus switch
        {
            FrameReadStatus.Ready => "LIVE · реальные Present-интервалы",
            FrameReadStatus.Stale => "Нет свежих кадров · N/A",
            FrameReadStatus.Busy => "Снимок обновляется · N/A",
            FrameReadStatus.Invalid => "Невалидный снимок · N/A",
            _ => "Ожидание нативного демо · N/A",
        };
        Changed(null);
    }

    private bool Has(MetricAvailability availability) => _metrics.AvailableMask.HasFlag(availability);
    private string IntelNumber(IntelAvailability availability, Func<IntelControl, double> read, string format) =>
        Number(_intel is { } data && data.AvailableMask.HasFlag(availability) ? read(data) : null, format);
    private static string Number(double? value, string format) => value is double v && double.IsFinite(v) ? v.ToString(format, CultureInfo.CurrentCulture) : "N/A";
}
