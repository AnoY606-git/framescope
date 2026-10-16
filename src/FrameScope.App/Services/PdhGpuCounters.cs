using System.Runtime.InteropServices;
using FrameScope.Core;

namespace FrameScope.App.Services;

internal sealed class PdhGpuCounters : IDisposable
{
    private const uint MoreData = 0x800007D2;
    private const uint FormatDouble = 0x00000200 | 0x00008000; // DOUBLE | NOCAP100
    private IntPtr _query;
    private IntPtr _engine;
    private IntPtr _memory;
    private bool _primed;
    private string _registration = "";
    private static readonly GpuCounterSelection Empty = GpuCounterAggregator.Aggregate([], []);
    public string Diagnostics { get; private set; } = "GPU: счётчики PDH не инициализированы.";

    public PdhGpuCounters()
    {
        var error = PdhOpenQuery(null, UIntPtr.Zero, out _query);
        if (error != 0) { Diagnostics = $"GPU: PdhOpenQuery 0x{error:X8}."; return; }
        var engineError = PdhAddEnglishCounter(_query, @"\GPU Engine(*)\Utilization Percentage", UIntPtr.Zero, out _engine);
        var memoryError = PdhAddEnglishCounter(_query, @"\GPU Adapter Memory(*)\Dedicated Usage", UIntPtr.Zero, out _memory);
        _registration = $"Регистрация GPU Engine: {Status(engineError)}; Dedicated Usage: {Status(memoryError)}.";
        Diagnostics = $"{_registration} Ожидание измерения PDH.";
    }

    public GpuCounterSelection Collect()
    {
        if (_query == IntPtr.Zero) return Empty;
        var error = PdhCollectQueryData(_query);
        if (error != 0)
        {
            _primed = false; Diagnostics = $"{_registration} GPU/VRAM: N/A, PdhCollectQueryData 0x{error:X8}."; return Empty;
        }
        if (!_primed) { _primed = true; Diagnostics = $"{_registration} GPU/VRAM: N/A, прогрев PDH."; return Empty; }
        var engines = ReadArray(_engine);
        var memory = ReadArray(_memory);
        var selection = GpuCounterAggregator.Aggregate(engines, memory);
        Diagnostics = $"{_registration}\n{selection.Reason}\nВыбран: "
            + (selection.Selected is { } selected ? $"LUID {selected.Adapter.Luid}, phys {selected.Adapter.PhysicalIndex}; {selected.EngineCount} физических движков."
                : "N/A — нет валидного адаптера.")
            + " GPU = max движков; каждый движок = сумма уникальных PID, ограниченная 100%. VRAM берётся только у того же физического адаптера; ёмкость N/A.";
        return selection;
    }

    private static List<GpuCounterValue> ReadArray(IntPtr counter)
    {
        var values = new List<GpuCounterValue>();
        if (counter == IntPtr.Zero) return values;
        uint bytes = 0, count = 0;
        if (PdhGetFormattedCounterArray(counter, FormatDouble, ref bytes, out count, IntPtr.Zero) != MoreData
            || bytes == 0 || bytes > 16 * 1024 * 1024) return values;
        var buffer = Marshal.AllocHGlobal((int)bytes);
        try
        {
            if (PdhGetFormattedCounterArray(counter, FormatDouble, ref bytes, out count, buffer) != 0) return values;
            var stride = Marshal.SizeOf<CounterItem>();
            if (count > bytes / stride) return values;
            for (var i = 0; i < count; i++)
            {
                var item = Marshal.PtrToStructure<CounterItem>(IntPtr.Add(buffer, i * stride));
                values.Add(new(Marshal.PtrToStringUni(item.Name) ?? "", item.Value.Value, item.Value.Status));
            }
        }
        finally { Marshal.FreeHGlobal(buffer); }
        return values;
    }

    public void Dispose()
    {
        if (_query != IntPtr.Zero) { PdhCloseQuery(_query); _query = IntPtr.Zero; }
    }

    private static string Status(uint code) => code == 0 ? "доступен" : $"N/A (0x{code:X8})";
    [StructLayout(LayoutKind.Sequential)] private struct CounterValue { public uint Status; public double Value; }
    [StructLayout(LayoutKind.Sequential)] private struct CounterItem { public IntPtr Name; public CounterValue Value; }

    [DllImport("pdh.dll", EntryPoint = "PdhOpenQueryW", CharSet = CharSet.Unicode)]
    private static extern uint PdhOpenQuery(string? source, UIntPtr userData, out IntPtr query);
    [DllImport("pdh.dll", EntryPoint = "PdhAddEnglishCounterW", CharSet = CharSet.Unicode)]
    private static extern uint PdhAddEnglishCounter(IntPtr query, string path, UIntPtr userData, out IntPtr counter);
    [DllImport("pdh.dll")] private static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll", EntryPoint = "PdhGetFormattedCounterArrayW", CharSet = CharSet.Unicode)]
    private static extern uint PdhGetFormattedCounterArray(IntPtr counter, uint format, ref uint bufferSize, out uint itemCount, IntPtr buffer);
    [DllImport("pdh.dll")] private static extern uint PdhCloseQuery(IntPtr query);
}
