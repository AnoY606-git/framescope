using FrameScope.Core;
using FrameScope.WindowsSensors;

internal static class GpuCounterTests
{
    private const string A = "00000000:000000aa", B = "00000000:000000bb";
    public static IEnumerable<(string Name, Func<Task> Run)> Cases =>
    [
        Test("GPU sums PIDs of the same physical engine before maximum", SharedEngine),
        Test("GPU never sums independent physical engines", SeparateEngines),
        Test("GPU caps a physical engine at 100 percent", SaturatedEngine),
        Test("GPU deduplicates PID aliases and duplicate instance IDs", DuplicateInstances),
        Test("GPU filters invalid CStatus, NaN, infinity and negative values", InvalidCounters),
        Test("GPU rejects malformed engine and adapter identities", MalformedNames),
        Test("GPU selection keeps VRAM on the busiest physical adapter", MultipleAdapters),
        Test("GPU selection falls back to largest dedicated usage", MemoryFallback),
        Test("GPU never borrows missing VRAM from a different adapter", MissingSelectedMemory),
        Test("GPU distinguishes phys indices and refuses ambiguous LUID sensor mapping", PhysicalAdapters),
        Test("GPU deduplicates adapter memory instances", DuplicateMemory),
        Test("GPU does not add PID-less aggregates twice", AggregateAndPid),
        Test("GPU ties select a deterministic explicit adapter", DeterministicTies),
        Test("Adapter LUID comparison keeps high and low halves exact", LuidComparison),
        Test("WDDM matching requires exactly one reading for the selected LUID", UniqueSensorMatch),
        Test("WDDM temperature decoding is genuine deci-Celsius", TemperatureDecode),
    ];

    private static (string, Func<Task>) Test(string name, Action action) => (name, () => { action(); return Task.CompletedTask; });
    private static void Check(bool value) { if (!value) throw new InvalidOperationException("Assertion failed"); }
    private static void Equal<T>(T expected, T actual) { if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new InvalidOperationException($"Expected {expected}; got {actual}"); }
    private static string Engine(int pid, int engine = 0, string luid = A, int phys = 0, string alias = "3D") =>
        $"pid_{pid}_luid_0x{luid[..8]}_0x{luid[9..]}_phys_{phys}_eng_{engine}_engtype_{alias}";
    private static string Memory(string luid = A, int phys = 0) => $"luid_0x{luid[..8]}_0x{luid[9..]}_phys_{phys}";
    private static GpuCounterSelection Select(IEnumerable<GpuCounterValue> engines, IEnumerable<GpuCounterValue>? memory = null) => GpuCounterAggregator.Aggregate(engines, memory ?? []);
    private static void SharedEngine()
    {
        var selection = Select([new(Engine(11), 50), new(Engine(22), 30), new(Engine(33, 1), 70)]);
        Equal(80d, selection.Selected!.Usage!.Value); Equal(2, selection.Selected.EngineCount); Equal(A, selection.AdapterLuid);
    }
    private static void SeparateEngines()
    {
        var selected = Select([new(Engine(11), 50), new(Engine(11, 1), 30), new(Engine(22, 2), 20)]).Selected!;
        Equal(50d, selected.Usage!.Value); Equal(3, selected.EngineCount);
    }
    private static void SaturatedEngine() => Equal(100d, Select([new(Engine(11), 90), new(Engine(22), 40), new(Engine(33, 1), 80)]).Selected!.Usage!.Value);
    private static void DuplicateInstances()
    {
        var selected = Select([new(Engine(11), 50), new(Engine(11) + "#1", 50), new(Engine(11, alias: "Graphics"), 40), new(Engine(22), 30)]).Selected!;
        Equal(80d, selected.Usage!.Value); Equal(1, selected.EngineCount);
    }
    private static void InvalidCounters()
    {
        var selected = Select([new(Engine(11), double.NaN), new(Engine(22), double.PositiveInfinity), new(Engine(33), -20),
            new(Engine(44), 99, 2), new(Engine(55), 100, 0xC0000BC6), new(Engine(66), 12, 1)],
            [new(Memory(), double.NaN), new(Memory(), -1), new(Memory(), 999, 2)]).Selected!;
        Equal(12d, selected.Usage!.Value); Check(selected.DedicatedUsageBytes is null);
    }
    private static void MalformedNames()
    {
        var selection = Select([new("_Total", 100), new("pid_1_luid_bogus_phys_0_eng_0", 100),
            new("pid_999999999999_luid_0x0_0xaa_phys_0_eng_0", 100), new("pid_1_luid_0x0_0xaa_eng_0", 100)], [new("_Total", 999)]);
        Check(selection.Selected is null); Equal(0, selection.Adapters.Count);
    }
    private static void MultipleAdapters()
    {
        var selection = Select([new(Engine(11), 50), new(Engine(22), 30), new(Engine(33, luid: B), 70)], [new(Memory(), 2e9), new(Memory(B), 12e9)]);
        Equal(A, selection.AdapterLuid); Equal(80d, selection.Selected!.Usage!.Value); Equal(2e9, selection.Selected.DedicatedUsageBytes!.Value); Check(!selection.IsLuidAmbiguous);
    }
    private static void MemoryFallback()
    {
        var selection = Select([], [new(Memory(), 2e9), new(Memory(B), 12e9)]);
        Equal(B, selection.AdapterLuid); Check(selection.Selected!.Usage is null); Equal(12e9, selection.Selected.DedicatedUsageBytes!.Value);
    }
    private static void MissingSelectedMemory()
    {
        var selection = Select([new(Engine(11), 80), new(Engine(22, luid: B), 70)], [new(Memory(B), 12e9)]);
        Equal(A, selection.AdapterLuid); Check(selection.Selected!.DedicatedUsageBytes is null);
    }
    private static void PhysicalAdapters()
    {
        var selection = Select([new(Engine(11), 30), new(Engine(22, phys: 1), 70)]);
        Equal(70d, selection.Selected!.Usage!.Value); Equal(1u, selection.Selected.Adapter.PhysicalIndex); Check(selection.IsLuidAmbiguous);
        Check(Select([new(Engine(22, phys: 1), 70)]).IsLuidAmbiguous);
    }
    private static void DuplicateMemory()
    {
        var selected = Select([], [new(Memory(), 2e9), new(Memory() + "#1", 2e9)]).Selected!;
        Equal(2e9, selected.DedicatedUsageBytes!.Value);
    }
    private static void AggregateAndPid()
    {
        var selected = Select([new(Engine(11), 50), new(Engine(22), 30), new("luid_0x0_0xaa_phys_0_eng_0_engtype_3D", 80)]).Selected!;
        Equal(80d, selected.Usage!.Value);
    }
    private static void DeterministicTies()
    {
        Equal(A, Select([new(Engine(11, luid: B), 0), new(Engine(22), 0)]).AdapterLuid);
        Equal(B, Select([new(Engine(11), 0), new(Engine(22, luid: B), 0)], [new(Memory(B), 100)]).AdapterLuid);
    }
    private static void LuidComparison()
    {
        Check(AdapterLuid.Matches("ABCDEF01:23456789", "abcdef01:23456789"));
        Check(!AdapterLuid.Matches("abcdef01:23456789", "23456789:abcdef01"));
        Check(!AdapterLuid.Matches(null, null)); Check(!AdapterLuid.Matches("abc:123", "abc:123"));
        Check(!AdapterLuid.TryNormalize(" 0000000:000000aa", out _));
    }
    private static void UniqueSensorMatch()
    {
        var a = new WddmGpuReading(A.ToUpperInvariant(), 57, 1_000_000_000);
        var b = new WddmGpuReading(B, 80, 2_000_000_000);
        Equal(a, AdapterLuid.SelectUnique(A, false, [a, b], reading => reading.AdapterLuid));
        Check(AdapterLuid.SelectUnique(A, false, new[] { b }, reading => reading.AdapterLuid) is null);
        Check(AdapterLuid.SelectUnique(A, false, new[] { a, a with { TemperatureCelsius = 90 } }, reading => reading.AdapterLuid) is null);
        Check(AdapterLuid.SelectUnique(A, true, new[] { a }, reading => reading.AdapterLuid) is null);
    }
    private static void TemperatureDecode()
    {
        Equal(57d, WddmGpuSensor.DecodeTemperature(570)!.Value); Equal(200d, WddmGpuSensor.DecodeTemperature(2000)!.Value);
        Check(WddmGpuSensor.DecodeTemperature(0) is null); Check(WddmGpuSensor.DecodeTemperature(2001) is null); Check(WddmGpuSensor.DecodeTemperature(uint.MaxValue) is null);
    }
}
