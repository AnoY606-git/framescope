using System.Text.Json;
using System.Text.Json.Nodes;
using FrameScope.Core;

internal static class IntelJsonTests
{
    // Только синтетическая in-memory fixture контракта v1. Никогда не является live-источником.
    private const string Luid = "00000000:0000f481";
    private const long Now = 10000;
    private const ulong Qpc = 1_000_000, Frequency = 1000;
    public static IEnumerable<(string Name, Func<Task> Run)> Cases =>
    [
        Test("Intel JSON v1 parses genuine legacy scopes, units and statuses", ValidLegacy),
        Test("Intel JSON rejects unknown schema and source", UnknownContract),
        Test("Intel JSON rejects malformed and duplicate properties", MalformedJson),
        Test("Intel JSON line and depth bounds are enforced", JsonBounds),
        Test("Intel JSON device and reading counts are bounded", CollectionBounds),
        Test("Intel JSON duplicate device/reading identities are rejected", DuplicateIdentity),
        Test("Intel JSON rejects future, stale and pre-process uptime", InvalidTime),
        Test("Intel JSON maps original uptime age to QPC exactly once", OriginalAge),
        Test("Intel JSON sequence guard rejects replay and time rollback", Replay),
        Test("Intel JSON UTC clock adjustments do not rebase monotonic age", WallClock),
        Test("Intel JSON backend failure never exposes numeric sensor values", BackendFailure),
        Test("Intel JSON device loss requests a fresh process, not hardware reset", DeviceLoss),
        Test("Intel JSON numeric strings, null, negative and nonfinite data are N/A", InvalidNumbers),
        Test("Intel JSON unsupported and warming-up readings never reuse values", UnavailableValues),
        Test("Intel JSON wrong units cannot activate control fields", WrongUnits),
        Test("Intel JSON original API windows are preserved, not polling intervals", OriginalWindow),
        Test("Intel JSON counter windows must be positive and present", InvalidWindows),
        Test("Intel JSON exact PCI match does not select device zero", PciSelection),
        Test("Intel JSON rejects nonzero PCI domain and ambiguous devices", PciAmbiguity),
        Test("Intel JSON cannot assign Intel values to AMD or unknown Windows PCI", VendorAndMissingPci),
        Test("Intel JSON requires physical zero and unambiguous LUID", PhysicalIdentity),
        Test("Intel JSON subdevice readings cannot substitute root GPU values", Subdevices),
        Test("Intel JSON multiple root instances remain ambiguous", MultipleReadings),
        Test("Intel JSON package power is never GPU-chip or board power", PowerScopes),
        Test("Intel JSON bandwidth converts B/s to GB/s without x32", BandwidthUnits),
        Test("Intel JSON unproven zero bandwidth is N/A, proven zero stays available", BandwidthZero),
        Test("Intel JSON fan remains unavailable even if a source supplies RPM", FanPolicy),
        Test("Intel JSON throttle zero is valid but fractional masks are rejected", Throttle),
        Test("Intel JSON physical capacity is not allocatable size or used memory", MemorySemantics),
        Test("Intel JSON GPU temperature uses gpu_max with explicit WDDM fallback", TemperatureSource),
        Test("Intel JSON stale/failed enrichment clears all prior Intel fields", ClearOldFields),
        ("Bounded JSON line reader handles buffered lines, CRLF and EOF", BufferedLines),
        ("Bounded JSON line reader rejects oversized lines and cancellation", BoundedLines),
    ];
    private static (string, Func<Task>) Test(string name, Action action) => (name, () => { action(); return Task.CompletedTask; });
    private static void Check(bool value) { if (!value) throw new InvalidOperationException("Assertion failed"); }
    private static void Equal<T>(T expected, T actual) { if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new InvalidOperationException($"Expected {expected}; got {actual}"); }
    private static JsonObject Reading(string key, string scope, string unit, double? value, string status = "available", string? reason = null) => new()
    {
        ["key"] = key, ["scope"] = scope, ["instance"] = $"fixture:{key}:{scope}", ["subdeviceId"] = null,
        ["unit"] = unit, ["value"] = value, ["status"] = status, ["reason"] = reason, ["flags"] = new JsonArray(),
        ["windowMs"] = key is "power.average" or "memory.read_bandwidth" or "memory.write_bandwidth" ? 1087.25 : null,
        ["note"] = key == "frequency.actual" && scope == "memory" ? "driver_reported_memory_frequency_not_verified_as_physical_clock" : null,
    };
    private static JsonObject Fixture()
    {
        var readings = new JsonArray
        {
            Reading("frequency.actual", "gpu", "MHz", 400), Reading("frequency.actual", "memory", "MHz", 19000),
            Reading("voltage", "gpu", "V", .725), Reading("frequency.throttle_reasons", "gpu", "bitmask", 0),
            Reading("temperature", "gpu_max", "C", 56), Reading("temperature", "memory_max", "C", 60),
            Reading("power.average", "card", "W", 22.6), Reading("power.average", "gpu", "W", null, "unsupported", "sensor_scope_not_exposed"),
            Reading("memory.physical_bytes", "device_physical", "B", 12884901888),
            Reading("memory.total_bytes", "device_allocatable", "B", 12706643968), Reading("memory.used_bytes", "device_allocatable", "B", 1200000000),
            Reading("memory.read_bandwidth", "device_allocatable", "B/s", null, "unavailable", "zero_counter_without_validity_flag"),
            Reading("memory.write_bandwidth", "device_allocatable", "B/s", null, "unavailable", "zero_counter_without_validity_flag"),
            Reading("fan.speed", "fan", "RPM", null, "unsupported", "read_only_policy_windows_sysman_fan_getters_can_write"),
        };
        return new()
        {
            ["schemaVersion"] = 1, ["source"] = "level-zero-sysman", ["timestamp"] = "2026-10-16T15:52:55.905Z",
            ["monotonicMs"] = Now, ["sequence"] = 1, ["intervalMs"] = 1000,
            ["backend"] = new JsonObject { ["status"] = "available", ["reason"] = null, ["mode"] = "legacy" },
            ["devices"] = new JsonArray(new JsonObject
            {
                ["id"] = "pci:0000:03:00.0", ["vendorId"] = 0x8086,
                ["pci"] = new JsonObject { ["domain"] = 0, ["bus"] = 3, ["device"] = 0, ["function"] = 0 }, ["readings"] = readings,
            }),
        };
    }
    private static JsonObject Device(JsonObject root) => root["devices"]![0]!.AsObject();
    private static JsonObject Find(JsonObject root, string key, string scope) => Device(root)["readings"]!.AsArray().OfType<JsonObject>().Single(r => (string?)r["key"] == key && (string?)r["scope"] == scope);
    private static IntelDecodeResult Decode(JsonObject root, long arrival = Now, ulong qpc = Qpc, long start = 9000) => IntelJsonDecoder.Decode(root.ToJsonString(), arrival, qpc, Frequency, start);
    private static IntelJsonSnapshot Snapshot(JsonObject? root = null) { var decoded = Decode(root ?? Fixture()); Equal(IntelDecodeStatus.Ready, decoded.Status); return decoded.Snapshot!; }
    private static MetricSample Baseline() => new()
    {
        GpuAdapterLuid = Luid, GpuPhysicalAdapter = 0, GpuPciBus = 3, GpuPciDevice = 0, GpuPciFunction = 0,
        GpuTemperature = 57, GpuTemperatureSource = "WDDM", VramUsedGiB = 1.25, TimestampQpc = Qpc,
    };
    private static MetricSample Enrich(JsonObject? root = null, MetricSample? baseline = null) => IntelSnapshotProjector.Enrich(baseline ?? Baseline(), Snapshot(root), Now, Qpc, Frequency, "test-only in-memory fixture");
    private static void ValidLegacy()
    {
        var sample = Enrich(); var data = sample.IntelControl!.Value;
        Equal(400d, data.GpuFrequencyMhz); Equal(19000d, data.MemoryFrequencyMhz); Equal(.725, data.VoltageVolts); Equal(22.6, data.BoardPowerWatts);
        Equal(60d, data.MemoryTemperature); Check(!data.AvailableMask.HasFlag(IntelAvailability.GpuPower)); Check(data.AvailableMask.HasFlag(IntelAvailability.Throttle));
        Equal(Qpc, data.UpdatedQpc); Equal(Luid, sample.IntelAdapterLuid); Check(sample.GetIntelControl(Qpc, Frequency) is not null);
    }
    private static void UnknownContract()
    {
        var root = Fixture(); root["schemaVersion"] = 2; Equal(IntelDecodeStatus.UnsupportedSchema, Decode(root).Status);
        root = Fixture(); root["source"] = "cached-probe"; Equal(IntelDecodeStatus.UnsupportedSource, Decode(root).Status);
        root = Fixture(); root["backend"]!["mode"] = "mixed"; Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
        root = Fixture(); root["intervalMs"] = 0; Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
    }
    private static void MalformedJson()
    {
        Equal(IntelDecodeStatus.Invalid, IntelJsonDecoder.Decode(null, Now, Qpc, Frequency).Status);
        Equal(IntelDecodeStatus.Invalid, IntelJsonDecoder.Decode("{", Now, Qpc, Frequency).Status);
        Equal(IntelDecodeStatus.Invalid, IntelJsonDecoder.Decode("{\"schemaVersion\":1,\"schemaVersion\":2}", Now, Qpc, Frequency).Status);
        Equal(IntelDecodeStatus.Invalid, IntelJsonDecoder.Decode("[]", Now, Qpc, Frequency).Status);
    }
    private static void JsonBounds()
    {
        Equal(IntelDecodeStatus.Invalid, IntelJsonDecoder.Decode(new string(' ', IntelJsonDecoder.MaximumLineCharacters + 1), Now, Qpc, Frequency).Status);
        var root = Fixture(); root["unknown"] = JsonNode.Parse(new string('[', 20) + "0" + new string(']', 20));
        Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
    }
    private static void CollectionBounds()
    {
        var root = Fixture(); var template = Device(root).DeepClone(); var devices = root["devices"]!.AsArray();
        for (var i = 0; i < IntelJsonDecoder.MaximumDevices; i++) { var item = template.DeepClone(); item["id"] = $"fixture:{i}"; devices.Add(item); }
        Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
        root = Fixture(); var readings = Device(root)["readings"]!.AsArray(); readings.Clear();
        for (var i = 0; i <= IntelJsonDecoder.MaximumReadings; i++) readings.Add(Reading($"unknown.{i}", "gpu", "MHz", 1));
        Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
    }
    private static void DuplicateIdentity()
    {
        var root = Fixture(); Device(root)["readings"]!.AsArray().Add(Find(root, "frequency.actual", "gpu").DeepClone()); Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
        root = Fixture(); root["devices"]!.AsArray().Add(Device(root).DeepClone()); Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
    }
    private static void InvalidTime()
    {
        var root = Fixture(); root["monotonicMs"] = Now + 1; Equal(IntelDecodeStatus.Future, Decode(root).Status);
        root["monotonicMs"] = Now - 5001; Equal(IntelDecodeStatus.Stale, Decode(root, start: 0).Status);
        root["monotonicMs"] = 8999; Equal(IntelDecodeStatus.BeforeProcess, Decode(root).Status);
        root = Fixture(); root["sequence"] = 0; Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
        root = Fixture(); root["monotonicMs"] = Now - 1; Equal(IntelDecodeStatus.Invalid, Decode(root, qpc: 1).Status);
    }
    private static void OriginalAge()
    {
        var root = Fixture(); root["monotonicMs"] = Now - 250; var snapshot = Snapshot(root);
        Equal(Qpc - 250, snapshot.UpdatedQpc);
        var sample = IntelSnapshotProjector.Enrich(Baseline(), snapshot, Now + 500, Qpc + 500, Frequency, "fixture");
        Equal(Qpc - 250, sample.IntelControl!.Value.UpdatedQpc);
        sample = IntelSnapshotProjector.Enrich(Baseline(), snapshot, Now + 5001, Qpc + 5001, Frequency, "fixture"); Check(sample.IntelControl is null);
    }
    private static void Replay()
    {
        var snapshot = Snapshot(); var guard = new IntelSnapshotSequence(); Check(guard.TryAccept(snapshot)); Check(!guard.TryAccept(snapshot));
        Check(!guard.TryAccept(snapshot with { Sequence = 2, MonotonicMs = snapshot.MonotonicMs - 1 }));
        Check(guard.TryAccept(snapshot with { Sequence = 3, MonotonicMs = snapshot.MonotonicMs + 1000 }));
        Check(new IntelSnapshotSequence().TryAccept(snapshot));
    }
    private static void WallClock()
    {
        var root = Fixture(); root["timestamp"] = "2099-01-01T00:00:00.000Z"; Equal(IntelDecodeStatus.Ready, Decode(root).Status);
        root["timestamp"] = "not UTC"; Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
        root["timestamp"] = "2026-10-16T10:00:00+03:00"; Equal(IntelDecodeStatus.Invalid, Decode(root).Status);
    }
    private static void BackendFailure()
    {
        var root = Fixture(); root["backend"]!["status"] = "unavailable"; root["backend"]!["reason"] = "ZE_RESULT_ERROR_DEVICE_LOST";
        var result = Decode(root); Equal(IntelDecodeStatus.BackendUnavailable, result.Status);
        var sample = IntelSnapshotProjector.Enrich(Baseline(), result.Snapshot, Now, Qpc, Frequency, result.Diagnostic);
        Check(sample.IntelControl is null); Check(sample.IntelDiagnostics.Contains("DEVICE_LOST", StringComparison.Ordinal));
    }
    private static void DeviceLoss()
    {
        var root = Fixture(); var reading = Find(root, "frequency.actual", "gpu");
        reading["status"] = "unavailable"; reading["reason"] = "ZE_RESULT_ERROR_DEVICE_LOST:0x70000001";
        Check(Snapshot(root).RequiresRestart);
        Check(Enrich(root).IntelControl is null);
        reading["reason"] = "ZE_RESULT_ERROR_DEVICE_REQUIRES_RESET:0x70000002"; Check(Snapshot(root).RequiresRestart);
        reading["status"] = "warming_up"; reading["reason"] = "counter_reset_or_rollover"; Check(!Snapshot(root).RequiresRestart);
    }
    private static void InvalidNumbers()
    {
        foreach (var value in new JsonNode?[] { JsonValue.Create("400"), null, JsonValue.Create(-1) })
        { var root = Fixture(); Find(root, "frequency.actual", "gpu")["value"] = value?.DeepClone(); Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.GpuClock)); }
        var json = Fixture().ToJsonString().Replace("\"value\":400", "\"value\":1e999", StringComparison.Ordinal);
        var decoded = IntelJsonDecoder.Decode(json, Now, Qpc, Frequency); Equal(IntelDecodeStatus.Ready, decoded.Status);
        Check(decoded.Snapshot!.Devices[0].Readings.Single(r => r.Key == "frequency.actual" && r.Scope == "gpu").Value is null);
    }
    private static void UnavailableValues()
    {
        foreach (var status in new[] { "unsupported", "unavailable", "warming_up", "stale", "invalid_data", "access_denied" })
        {
            var root = Fixture(); Find(root, "frequency.actual", "gpu")["status"] = status;
            Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.GpuClock));
        }
    }
    private static void WrongUnits()
    {
        var root = Fixture(); Find(root, "power.average", "card")["unit"] = "mW"; Find(root, "frequency.actual", "memory")["unit"] = "Hz";
        var data = Enrich(root).IntelControl!.Value; Check(!data.AvailableMask.HasFlag(IntelAvailability.BoardPower)); Check(!data.AvailableMask.HasFlag(IntelAvailability.MemoryClock));
    }
    private static void OriginalWindow()
    {
        var sample = Enrich(); Check(sample.IntelDiagnostics.Contains("API window 1087.25 ms", StringComparison.Ordinal));
        Equal(1087.25, Snapshot().Devices[0].Readings.Single(r => r.Key == "power.average" && r.Scope == "card").WindowMs!.Value);
    }
    private static void InvalidWindows()
    {
        foreach (var window in new double?[] { null, 0, -1 })
        { var root = Fixture(); Find(root, "power.average", "card")["windowMs"] = window; Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.BoardPower)); }
    }
    private static void PciSelection()
    {
        var root = Fixture(); var wrong = Device(root).DeepClone(); wrong["id"] = "pci:0000:04:00.0"; wrong["pci"]!["bus"] = 4;
        wrong["readings"]!.AsArray().OfType<JsonObject>().First(r => (string?)r["key"] == "frequency.actual")["value"] = 999;
        root["devices"]!.AsArray().Insert(0, wrong); Equal(400d, Enrich(root).IntelControl!.Value.GpuFrequencyMhz);
        Check(Enrich(root, Baseline() with { GpuPciBus = 5 }).IntelControl is null);
    }
    private static void PciAmbiguity()
    {
        var root = Fixture(); Device(root)["pci"]!["domain"] = 1; Check(Enrich(root).IntelControl is null);
        root = Fixture(); var duplicate = Device(root).DeepClone(); duplicate["id"] = "other-identity-same-pci"; root["devices"]!.AsArray().Add(duplicate); Check(Enrich(root).IntelControl is null);
    }
    private static void VendorAndMissingPci()
    {
        var root = Fixture(); Device(root)["vendorId"] = 0x1002; Check(Enrich(root).IntelControl is null);
        Check(Enrich(baseline: Baseline() with { GpuPciBus = null }).IntelControl is null);
        root = Fixture(); Device(root)["pci"] = null; Check(Enrich(root).IntelControl is null);
    }
    private static void PhysicalIdentity()
    {
        Check(Enrich(baseline: Baseline() with { GpuPhysicalAdapter = 1 }).IntelControl is null);
        Check(Enrich(baseline: Baseline() with { GpuPhysicalAdapter = null }).IntelControl is null);
        Check(Enrich(baseline: Baseline() with { GpuAdapterIsAmbiguous = true }).IntelControl is null);
    }
    private static void Subdevices()
    {
        var root = Fixture(); Find(root, "frequency.actual", "gpu")["subdeviceId"] = 1;
        Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.GpuClock));
    }
    private static void MultipleReadings()
    {
        var root = Fixture(); var duplicate = Find(root, "frequency.actual", "gpu").DeepClone(); duplicate["instance"] = "other-root-frequency";
        Device(root)["readings"]!.AsArray().Add(duplicate); Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.GpuClock));
    }
    private static void PowerScopes()
    {
        var root = Fixture(); Find(root, "power.average", "card")["scope"] = "package";
        var data = Enrich(root).IntelControl!.Value; Check(!data.AvailableMask.HasFlag(IntelAvailability.BoardPower)); Check(!data.AvailableMask.HasFlag(IntelAvailability.GpuPower));
        root = Fixture(); var gpu = Find(root, "power.average", "gpu"); gpu["status"] = "available"; gpu["value"] = 15; gpu["reason"] = null;
        data = Enrich(root).IntelControl!.Value; Equal(15d, data.GpuPowerWatts); Equal(22.6, data.BoardPowerWatts);
    }
    private static void BandwidthUnits()
    {
        var root = Fixture(); var read = Find(root, "memory.read_bandwidth", "device_allocatable"); read["status"] = "available"; read["value"] = 2_000_000_000; read["reason"] = null;
        var data = Enrich(root).IntelControl!.Value; Equal(2d, data.MemoryReadGbps); Check(data.AvailableMask.HasFlag(IntelAvailability.MemoryRead));
    }
    private static void BandwidthZero()
    {
        var root = Fixture(); var read = Find(root, "memory.read_bandwidth", "device_allocatable"); read["value"] = 0;
        Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.MemoryRead));
        read["status"] = "available"; Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.MemoryRead));
        read["reason"] = null; var data = Enrich(root).IntelControl!.Value; Check(data.AvailableMask.HasFlag(IntelAvailability.MemoryRead)); Equal(0d, data.MemoryReadGbps);
    }
    private static void FanPolicy()
    {
        var root = Fixture(); var fan = Find(root, "fan.speed", "fan"); fan["status"] = "available"; fan["reason"] = null; fan["value"] = 27;
        var data = Enrich(root).IntelControl!.Value; Check(!data.AvailableMask.HasFlag(IntelAvailability.Fan)); Equal(0d, data.FanRpm);
    }
    private static void Throttle()
    {
        var data = Enrich().IntelControl!.Value; Check(data.AvailableMask.HasFlag(IntelAvailability.Throttle)); Equal(0ul, data.ThrottleFlags);
        var root = Fixture(); Find(root, "frequency.throttle_reasons", "gpu")["value"] = .5; Check(!Enrich(root).IntelControl!.Value.AvailableMask.HasFlag(IntelAvailability.Throttle));
    }
    private static void MemorySemantics()
    {
        var sample = Enrich(); Equal(12d, sample.VramTotalGiB!.Value); Equal(1.25, sample.VramUsedGiB!.Value); Equal("Sysman physical", sample.VramCapacitySource);
        sample = Enrich(baseline: Baseline() with { VramTotalGiB = 16, VramCapacitySource = "WDDM dedicated (driver-reported)" }); Equal(16d, sample.VramTotalGiB!.Value);
        var root = Fixture(); Find(root, "memory.physical_bytes", "device_physical")["status"] = "unsupported";
        sample = Enrich(root); Check(sample.VramTotalGiB is null); Equal(1.25, sample.VramUsedGiB!.Value);
    }
    private static void TemperatureSource()
    {
        var sample = Enrich(); Equal(56d, sample.GpuTemperature!.Value); Equal("Sysman gpu_max", sample.GpuTemperatureSource);
        var root = Fixture(); Find(root, "temperature", "gpu_max")["scope"] = "global_max";
        sample = Enrich(root); Equal(57d, sample.GpuTemperature!.Value); Equal("WDDM", sample.GpuTemperatureSource);
    }
    private static void ClearOldFields()
    {
        var enriched = Enrich();
        var empty = IntelSnapshotProjector.Enrich(enriched, null, Now, Qpc, Frequency, "blocked 4551");
        Check(empty.IntelControl is null && empty.IntelAdapterLuid is null && empty.GpuTemperature is null && empty.VramTotalGiB is null);
        var stale = IntelSnapshotProjector.Enrich(Baseline(), Snapshot(), Now + 5001, Qpc + 5001, Frequency, "stale");
        Check(stale.IntelControl is null); Equal(57d, stale.GpuTemperature!.Value); Equal("WDDM", stale.GpuTemperatureSource);
    }
    private static async Task BufferedLines()
    {
        using var text = new StringReader("first\r\nsecond\nlast"); var reader = new BoundedLineReader(text, 10);
        Equal("first", await reader.ReadLineAsync()); Equal("second", await reader.ReadLineAsync()); Equal("last", await reader.ReadLineAsync()); Check(await reader.ReadLineAsync() is null);
    }
    private static async Task BoundedLines()
    {
        using var text = new StringReader(new string('a', 11) + "\n"); var reader = new BoundedLineReader(text, 10);
        try { await reader.ReadLineAsync(); throw new InvalidOperationException("Expected bounded-line error"); } catch (InvalidDataException) { }
        using var cancel = new CancellationTokenSource(); cancel.Cancel();
        try { await reader.ReadLineAsync(cancel.Token); throw new InvalidOperationException("Expected cancellation"); } catch (OperationCanceledException) { }
    }
}
