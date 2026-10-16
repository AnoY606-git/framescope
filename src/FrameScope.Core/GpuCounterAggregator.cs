using System.Globalization;
using System.Text.RegularExpressions;

namespace FrameScope.Core;

public readonly record struct GpuCounterValue(string InstanceName, double Value, uint Status = 0);
public readonly record struct GpuAdapterKey(string Luid, uint PhysicalIndex);
public sealed record GpuAdapterCounters(GpuAdapterKey Adapter, double? Usage, double? DedicatedUsageBytes, int EngineCount);
public sealed record GpuCounterSelection(GpuAdapterCounters? Selected, IReadOnlyList<GpuAdapterCounters> Adapters, string Reason)
{
    public string? AdapterLuid => Selected?.Adapter.Luid;
    public bool IsLuidAmbiguous => Selected is { } selected
        && (selected.Adapter.PhysicalIndex != 0 || Adapters.Count(adapter => adapter.Adapter.Luid == selected.Adapter.Luid) > 1);
}

public static class AdapterLuid
{
    public static bool TryNormalize(string? value, out string normalized)
    {
        normalized = "";
        if (value is not { Length: 17 } || value[8] != ':'
            || !uint.TryParse(value.AsSpan(0, 8), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var high)
            || !uint.TryParse(value.AsSpan(9, 8), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var low)) return false;
        normalized = $"{high:x8}:{low:x8}";
        return true;
    }

    public static bool Matches(string? first, string? second) => TryNormalize(first, out var a) && TryNormalize(second, out var b) && a == b;

    public static T? SelectUnique<T>(string? selectedLuid, bool physicalAmbiguity, IEnumerable<T> readings, Func<T, string?> getLuid) where T : class
    {
        ArgumentNullException.ThrowIfNull(readings); ArgumentNullException.ThrowIfNull(getLuid);
        if (physicalAmbiguity || !TryNormalize(selectedLuid, out var selected)) return null;
        T? matched = null;
        foreach (var reading in readings)
        {
            if (reading is null || !Matches(selected, getLuid(reading))) continue;
            if (matched is not null) return null;
            matched = reading;
        }
        return matched;
    }
}

public static partial class GpuCounterAggregator
{
    public static GpuCounterSelection Aggregate(IEnumerable<GpuCounterValue> engineCounters, IEnumerable<GpuCounterValue> memoryCounters)
    {
        ArgumentNullException.ThrowIfNull(engineCounters);
        ArgumentNullException.ThrowIfNull(memoryCounters);
        var engines = new Dictionary<EngineKey, EngineUsage>();
        foreach (var counter in engineCounters)
        {
            if (!Valid(counter) || !TryEngine(counter.InstanceName, out var key, out var pid)) continue;
            if (!engines.TryGetValue(key, out var engine)) engines[key] = engine = new();
            var value = Math.Min(counter.Value, 100);
            // Один PID/phys/eng может иметь повторный экземпляр или псевдоним engtype: не считаем дважды.
            if (pid is uint process) engine.Processes[process] = Math.Max(engine.Processes.GetValueOrDefault(process), value);
            else engine.Aggregate = Math.Max(engine.Aggregate ?? 0, value);
        }
        var physical = new Dictionary<GpuAdapterKey, (double Maximum, int Count)>();
        foreach (var (key, engine) in engines)
        {
            // Складываем занятость процессов ТОЛЬКО внутри одного физического движка.
            // Если провайдер также дал агрегат без PID, он не прибавляется повторно к процессам.
            var usage = Math.Min(100, Math.Max(engine.Processes.Values.Sum(), engine.Aggregate ?? 0));
            var previous = physical.GetValueOrDefault(key.Adapter);
            physical[key.Adapter] = (Math.Max(previous.Maximum, usage), previous.Count + 1);
        }
        var memory = new Dictionary<GpuAdapterKey, double>();
        foreach (var counter in memoryCounters)
        {
            if (!Valid(counter) || !TryAdapter(counter.InstanceName, out var key)) continue;
            // Dedicated Usage уже агрегирован адаптером; повторный экземпляр не увеличивает объём.
            memory[key] = Math.Max(memory.GetValueOrDefault(key), counter.Value);
        }
        var adapters = physical.Keys.Concat(memory.Keys).Distinct()
            .OrderBy(key => key.Luid, StringComparer.Ordinal).ThenBy(key => key.PhysicalIndex)
            .Select(key => new GpuAdapterCounters(key, physical.TryGetValue(key, out var busy) ? busy.Maximum : null,
                memory.TryGetValue(key, out var bytes) ? bytes : null, physical.GetValueOrDefault(key).Count)).ToArray();
        var selected = adapters.Where(adapter => adapter.Usage is not null)
            .OrderByDescending(adapter => adapter.Usage).ThenByDescending(adapter => adapter.DedicatedUsageBytes)
            .ThenBy(adapter => adapter.Adapter.Luid, StringComparer.Ordinal).ThenBy(adapter => adapter.Adapter.PhysicalIndex).FirstOrDefault();
        var reason = "Самый занятый физический адаптер (max движков после суммы PID одного движка).";
        if (selected is null)
        {
            selected = adapters.Where(adapter => adapter.DedicatedUsageBytes is not null)
                .OrderByDescending(adapter => adapter.DedicatedUsageBytes).ThenBy(adapter => adapter.Adapter.Luid, StringComparer.Ordinal)
                .ThenBy(adapter => adapter.Adapter.PhysicalIndex).FirstOrDefault();
            reason = selected is null ? "Нет валидных GPU Engine / Dedicated Usage экземпляров."
                : "Нет валидной загрузки: выбран адаптер с максимальным Dedicated Usage.";
        }
        return new(selected, Array.AsReadOnly(adapters), reason);
    }

    private static bool Valid(GpuCounterValue counter) => counter.Status <= 1 && double.IsFinite(counter.Value) && counter.Value >= 0;
    private static bool TryEngine(string? instance, out EngineKey key, out uint? pid)
    {
        key = default; pid = null;
        if (instance is null) return false;
        var match = EnginePattern().Match(instance);
        if (!match.Success || !TryKey(match, out var adapter)
            || !uint.TryParse(match.Groups["engine"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var engine)) return false;
        if (match.Groups["pid"].Success)
        {
            if (!uint.TryParse(match.Groups["pid"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var process)) return false;
            pid = process;
        }
        key = new(adapter, engine); return true;
    }
    private static bool TryAdapter(string? instance, out GpuAdapterKey key)
    {
        key = default;
        return instance is not null && TryKey(AdapterPattern().Match(instance), out key);
    }
    private static bool TryKey(Match match, out GpuAdapterKey key)
    {
        key = default;
        if (!match.Success || !uint.TryParse(match.Groups["high"].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var high)
            || !uint.TryParse(match.Groups["low"].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var low)
            || !uint.TryParse(match.Groups["phys"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var physical)) return false;
        key = new($"{high:x8}:{low:x8}", physical); return true;
    }

    private readonly record struct EngineKey(GpuAdapterKey Adapter, uint Engine);
    private sealed class EngineUsage
    {
        public Dictionary<uint, double> Processes { get; } = [];
        public double? Aggregate { get; set; }
    }
    [GeneratedRegex(@"^(?:pid_(?<pid>[0-9]+)_)?luid_0x(?<high>[0-9a-f]{1,8})_0x(?<low>[0-9a-f]{1,8})_phys_(?<phys>[0-9]+)_eng_(?<engine>[0-9]+)(?:_engtype_[^#]*)?(?:#[0-9]+)?$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex EnginePattern();
    [GeneratedRegex(@"^luid_0x(?<high>[0-9a-f]{1,8})_0x(?<low>[0-9a-f]{1,8})_phys_(?<phys>[0-9]+)(?:#[0-9]+)?$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex AdapterPattern();
}
