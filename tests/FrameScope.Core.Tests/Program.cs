using System.Buffers.Binary;
using System.Runtime.InteropServices;
using FrameScope.Core;

// Без стороннего тестового раннера: dotnet run --project tests/FrameScope.Core.Tests.
var tests = new (string Name, Func<Task> Run)[]
{
    ("Protocol sizes and field offsets", Sync(ProtocolLayout)),
    ("Control encoder agrees with native layout", Sync(ControlLayout)),
    ("Settings clamp invalid numbers, enums and colors", Sync(SettingsValidation)),
    ("Settings keep profiles independent and unique", Sync(IndependentProfiles)),
    ("Portable marker selects executable directory", PortableLocation),
    ("Missing settings do not create a file", MissingSettings),
    ("Settings save and load atomically", AtomicRoundTrip),
    ("Corrupt settings are preserved, never silently overwritten", CorruptSettings),
    ("Unsupported settings version is preserved before explicit save", UnsupportedVersion),
    ("Cancelled save leaves original and no temporary file", CancelledSave),
    ("Concurrent saves remain readable", ConcurrentSaves),
    ("Oversized settings are rejected and preserved", OversizedSettings),
    ("Null profiles normalize to safe defaults", NullProfiles),
    ("Frame parses all fields and graph", Sync(ValidFrame)),
    ("Short payload is rejected", Sync(ShortPayload)),
    ("Graph bounds and nonfinite samples are rejected", Sync(GraphBounds)),
    ("Missing, stale and future snapshots are rejected", Sync(StaleFrames)),
    ("Unavailable FPS and lows remain null", Sync(OptionalFrameValues)),
    ("Malformed optional values and flags are rejected", Sync(InvalidFrameValues)),
    ("Parsed graph does not reference the transport buffer", Sync(SnapshotOwnership)),
    ("Seqlock retries are bounded", Sync(BoundedSeqlock)),
    ("Seqlock discards torn data and accepts stable retry", Sync(TornSeqlock)),
    ("Missing and stale seqlocks never return cached values", Sync(NoStaleCache)),
    ("Availability mask never advertises fabricated sensors", Sync(Availability)),
    ("CPU counters require warmup and use busiest logical CPU", Sync(CpuDeltas)),
    ("CPU counter rollback is unavailable", Sync(CpuRollback)),
    ("Impossible CPU idle deltas are unavailable", Sync(CpuImpossibleIdle)),
}.Concat(GpuCounterTests.Cases).Concat(IntelTelemetryTests.Cases).Concat(IntelJsonTests.Cases).ToArray();
var failures = 0;
foreach (var test in tests)
{
    try { await test.Run(); Console.WriteLine($"PASS {test.Name}"); }
    catch (Exception error) { failures++; Console.Error.WriteLine($"FAIL {test.Name}: {error}"); }
}
Console.WriteLine($"{tests.Length - failures}/{tests.Length} passed");
return failures == 0 ? 0 : 1;

static Func<Task> Sync(Action action) => () => { action(); return Task.CompletedTask; };
static void Check(bool condition, string message = "Assertion failed") { if (!condition) throw new InvalidOperationException(message); }
static void Equal<T>(T expected, T actual) { if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new InvalidOperationException($"Expected {expected}; got {actual}"); }
static string TempDirectory() { var path = Path.Combine(Path.GetTempPath(), "opencode", "FrameScope.Tests", Guid.NewGuid().ToString("N")); Directory.CreateDirectory(path); return path; }
static void Cleanup(string path) => Directory.Delete(path, recursive: true);

static void ProtocolLayout()
{
    Equal(128, Marshal.SizeOf<HudControl>());
    Equal(40, Marshal.OffsetOf<HudControl>(nameof(HudControl.CpuUsage)).ToInt32());
    Equal(120, Marshal.OffsetOf<HudControl>(nameof(HudControl.UpdatedQpc)).ToInt32());
    Equal(560, Marshal.SizeOf<TestFrameLayout>());
    Equal(80, Marshal.OffsetOf<TestFrameLayout>(nameof(TestFrameLayout.Graph)).ToInt32());
    Equal(32, NativeProtocol.ControlOffset); Equal(256, NativeProtocol.FrameSequenceOffset);
    Equal(264, NativeProtocol.FrameOffset); Equal(4096, NativeProtocol.MappingSize);
    Check(NativeProtocol.FrameOffset + NativeProtocol.FrameSize < NativeProtocol.MappingSize);
}
static void ControlLayout()
{
    var control = new MetricSample { CpuUsage = 42.5, CpuMax = 80, RamUsedGiB = 12, RamTotalGiB = 32, TimestampQpc = 1234 }
        .ToControl(new() { Corner = HudCorner.BottomRight, Scale = 1.5f });
    var bytes = new byte[128]; ControlEncoder.Write(bytes, control);
    Equal(3u, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(4)));
    Equal(1.5f, BinaryPrimitives.ReadSingleLittleEndian(bytes.AsSpan(8)));
    Equal(42.5, BinaryPrimitives.ReadDoubleLittleEndian(bytes.AsSpan(40)));
    Equal(1234ul, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(120)));
    Equal(0xFFEAF1FFu, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(24)));
    Equal(control.AvailableMask, (MetricAvailability)BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(36)));
}
static void SettingsValidation()
{
    var p = new HudProfile { Scale = float.NaN, Opacity = -10, MarginX = 9999, MarginY = float.PositiveInfinity,
        Corner = (HudCorner)999, Layout = (HudLayout)999, TextColor = "hello", AccentColor = "#aa00ff", Name = "  " }.Normalize();
    Equal(1f, p.Scale); Equal(.15f, p.Opacity); Equal(240f, p.MarginX); Equal(16f, p.MarginY);
    Equal(HudCorner.TopLeft, p.Corner); Equal(HudLayout.Panel, p.Layout); Equal("#EAF1FF", p.TextColor);
    Equal("#AA00FF", p.AccentColor); Equal("Профиль", p.Name);
    Equal(.6f, (p with { Scale = -1 }).Normalize().Scale);
    Equal(2.5f, (p with { Scale = 100 }).Normalize().Scale);
}
static void IndependentProfiles()
{
    var settings = new AppSettings { ActiveProfileId = "absent", Profiles = [new(), new()] }.Normalize();
    Equal(2, settings.Profiles.Select(p => p.Id).Distinct().Count());
    Equal(settings.Profiles[0].Id, settings.ActiveProfileId);
    var changed = settings.Profiles[0] with { Scale = 2 };
    Equal(1f, settings.Profiles[1].Scale); Equal(2f, changed.Scale);
    Equal(3, (new AppSettings { Profiles = [] }).Normalize().Profiles.Count);
}
static async Task PortableLocation()
{
    var root = TempDirectory();
    try
    {
        Equal(Path.Combine(root, "FrameScope", "settings.json"), SettingsLocation.Resolve(root, root));
        await File.WriteAllTextAsync(Path.Combine(root, "portable.txt"), "");
        Equal(Path.Combine(root, "settings.json"), SettingsLocation.Resolve(root, "ignored"));
    }
    finally { Cleanup(root); }
}
static async Task MissingSettings()
{
    var root = TempDirectory();
    try { var store = new SettingsStore(Path.Combine(root, "settings.json")); Equal(SettingsLoadStatus.Missing, (await store.LoadAsync()).Status); Check(!File.Exists(store.Path)); }
    finally { Cleanup(root); }
}
static async Task AtomicRoundTrip()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "nested", "settings.json"));
        var settings = new AppSettings { ActiveProfileId = "detailed" };
        await store.SaveAsync(settings);
        Equal(SettingsLoadStatus.Loaded, (await store.LoadAsync()).Status);
        await store.SaveAsync(settings with { ActiveProfileId = "minimal" });
        Equal("minimal", (await store.LoadAsync()).Settings.ActiveProfileId);
        Equal(0, Directory.GetFiles(Path.GetDirectoryName(store.Path)!, "*.tmp").Length);
    }
    finally { Cleanup(root); }
}
static async Task CorruptSettings()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "settings.json"));
        const string corrupt = "{ invalid user settings";
        await File.WriteAllTextAsync(store.Path, corrupt);
        var result = await store.LoadAsync();
        Equal(SettingsLoadStatus.Corrupt, result.Status); Check(result.PreservedPath is not null);
        Equal(corrupt, await File.ReadAllTextAsync(store.Path));
        Equal(corrupt, await File.ReadAllTextAsync(result.PreservedPath!));
        await store.SaveAsync(new());
        Equal(SettingsLoadStatus.Loaded, (await store.LoadAsync()).Status);
        Equal(corrupt, await File.ReadAllTextAsync(result.PreservedPath!));
    }
    finally { Cleanup(root); }
}
static async Task UnsupportedVersion()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "settings.json"));
        const string original = "{\"Version\":999,\"FutureData\":true}";
        await File.WriteAllTextAsync(store.Path, original);
        Equal(SettingsLoadStatus.UnsupportedVersion, (await store.LoadAsync()).Status);
        Equal(original, await File.ReadAllTextAsync(store.Path));
        await store.SaveAsync(new());
        var backup = Directory.GetFiles(root, "*.preserved-*").Single();
        Equal(original, await File.ReadAllTextAsync(backup));
    }
    finally { Cleanup(root); }
}
static async Task CancelledSave()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "settings.json")); await store.SaveAsync(new());
        var before = await File.ReadAllTextAsync(store.Path);
        using var cts = new CancellationTokenSource(); cts.Cancel();
        try { await store.SaveAsync(new() { ActiveProfileId = "minimal" }, cts.Token); throw new InvalidOperationException("Cancellation ignored"); }
        catch (OperationCanceledException) { }
        Equal(before, await File.ReadAllTextAsync(store.Path));
        Equal(0, Directory.GetFiles(root, "*.tmp").Length);
    }
    finally { Cleanup(root); }
}
static async Task ConcurrentSaves()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "settings.json"));
        await Task.WhenAll(Enumerable.Range(0, 12).Select(i => store.SaveAsync(new() { ActiveProfileId = i % 2 == 0 ? "minimal" : "detailed" })));
        Equal(SettingsLoadStatus.Loaded, (await store.LoadAsync()).Status);
    }
    finally { Cleanup(root); }
}
static async Task OversizedSettings()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "settings.json"));
        await File.WriteAllTextAsync(store.Path, new string(' ', 1024 * 1024 + 1));
        var result = await store.LoadAsync();
        Equal(SettingsLoadStatus.Corrupt, result.Status); Check(result.PreservedPath is not null);
        Equal(new FileInfo(store.Path).Length, new FileInfo(result.PreservedPath!).Length);
    }
    finally { Cleanup(root); }
}
static async Task NullProfiles()
{
    var root = TempDirectory();
    try
    {
        var store = new SettingsStore(Path.Combine(root, "settings.json"));
        await File.WriteAllTextAsync(store.Path, "{\"Version\":1,\"Profiles\":null,\"ActiveProfileId\":null}");
        var result = await store.LoadAsync(); Equal(SettingsLoadStatus.Loaded, result.Status);
        Equal(3, result.Settings.Profiles.Count); Equal("compact", result.Settings.ActiveProfileId);
    }
    finally { Cleanup(root); }
}
static byte[] Frame()
{
    var data = new byte[NativeProtocol.FrameSize];
    U64(data, 0, 125); U64(data, 8, 9500); F64(data, 16, 100); F64(data, 24, 10);
    F64(data, 32, 90); F64(data, 40, 80); F64(data, 48, 52.25);
    U32(data, 56, (uint)GraphicsApi.Dx12); U32(data, 60, 7); U32(data, 64, 3);
    U32(data, 68, 1920); U32(data, 72, 1080); U32(data, 76, 1234);
    for (var i = 0; i < 3; i++) BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(80 + i * 4), 10 + i);
    return data;
}
static void ValidFrame()
{
    Equal(FrameReadStatus.Ready, FrameSnapshotParser.Parse(Frame(), 10000, 1000, out var s)); Check(s is not null);
    Equal(125ul, s!.PresentCount); Equal(100d, s.Fps!.Value); Equal(GraphicsApi.Dx12, s.Api);
    Equal(90d, s.Low1!.Value); Equal(80d, s.Low01!.Value); Equal(52.25, s.HudCpuMicroseconds);
    Equal(1920u, s.Width); Equal(1080u, s.Height); Equal(1234u, s.ProcessId);
    Equal(3, s.Graph.Count); Equal(12f, s.Graph[2]);
}
static void ShortPayload() { for (var i = 0; i < NativeProtocol.FrameSize; i++) Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(new byte[i], 10000, 1000, out _)); }
static void GraphBounds()
{
    var data = Frame(); U32(data, 64, 121); Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
    U32(data, 64, uint.MaxValue); Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
    U32(data, 64, 1); BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(80), float.NaN);
    Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
    BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(80), -1); Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
}
static void StaleFrames()
{
    Equal(FrameReadStatus.Missing, FrameSnapshotParser.Parse(new byte[560], 10000, 1000, out var s)); Check(s is null);
    Equal(FrameReadStatus.Stale, FrameSnapshotParser.Parse(Frame(), 12000, 1000, out s)); Check(s is null);
    Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(Frame(), 9000, 1000, out s)); Check(s is null);
    Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(Frame(), 10000, 0, out _));
}
static void OptionalFrameValues()
{
    var data = Frame(); U32(data, 60, 0); F64(data, 16, double.NaN);
    Equal(FrameReadStatus.Ready, FrameSnapshotParser.Parse(data, 10000, 1000, out var s)); Check(s!.Fps is null && s.Low1 is null && s.Low01 is null);
}
static void InvalidFrameValues()
{
    var data = Frame(); F64(data, 16, double.NaN); Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
    data = Frame(); U32(data, 60, 0x80000000); Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
    data = Frame(); F64(data, 24, -1); Equal(FrameReadStatus.Invalid, FrameSnapshotParser.Parse(data, 10000, 1000, out _));
}
static void SnapshotOwnership()
{
    var data = Frame(); FrameSnapshotParser.Parse(data, 10000, 1000, out var s);
    BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(80), 999); Equal(10f, s!.Graph[0]);
}
static void BoundedSeqlock()
{
    var source = new FakeTransport(Frame(), _ => 1);
    Equal(FrameReadStatus.Busy, SeqlockFrameReader.Read(source, 10000, 1000, out var s));
    Equal(SeqlockFrameReader.MaximumAttempts, source.Reads); Equal(0, source.Copies); Check(s is null);
}
static void TornSeqlock()
{
    var source = new FakeTransport(Frame(), call => call == 1 ? 2ul : 4ul);
    Equal(FrameReadStatus.Ready, SeqlockFrameReader.Read(source, 10000, 1000, out var s));
    Equal(2, source.Copies); Check(s is not null);
}
static void NoStaleCache()
{
    var source = new FakeTransport(Frame(), _ => 2);
    Equal(FrameReadStatus.Ready, SeqlockFrameReader.Read(source, 10000, 1000, out _));
    Equal(FrameReadStatus.Stale, SeqlockFrameReader.Read(source, 20000, 1000, out var s)); Check(s is null);
    var missing = new FakeTransport(Frame(), _ => 0);
    Equal(FrameReadStatus.Missing, SeqlockFrameReader.Read(missing, 10000, 1000, out s)); Check(s is null); Equal(0, missing.Copies);
}
static void Availability()
{
    Equal((MetricAvailability)0, MetricSample.Empty.AvailableMask);
    var m = new MetricSample { CpuUsage = double.NaN, GpuUsage = 101, RamUsedGiB = 10, RamTotalGiB = 32, VramUsedGiB = 0 };
    Equal(MetricAvailability.Ram | MetricAvailability.Vram, m.AvailableMask);
    Equal(0d, m.ToControl(new()).CpuUsage);
    Check(!m.AvailableMask.HasFlag(MetricAvailability.GpuTemperature));
    Check(!m.AvailableMask.HasFlag(MetricAvailability.GpuPower));
}
static void CpuDeltas()
{
    var c = new CpuUsageCalculator();
    var first = c.Sample(new(20, 50, 50), [new(10, 25, 25), new(10, 25, 25)]);
    Check(first.Total is null && first.MaximumLogicalProcessor is null);
    var next = c.Sample(new(60, 100, 100), [new(50, 50, 50), new(10, 50, 50)]);
    Equal(60d, next.Total!.Value); Equal(100d, next.MaximumLogicalProcessor!.Value);
}
static void CpuRollback()
{
    var c = new CpuUsageCalculator(); c.Sample(new(20, 50, 50), []);
    Check(c.Sample(new(10, 25, 25), []).Total is null);
    Check(c.Sample(new(10, 25, 25), []).Total is null);
}
static void CpuImpossibleIdle()
{
    var c = new CpuUsageCalculator(); c.Sample(new(20, 50, 50), []);
    Check(c.Sample(new(500, 60, 60), []).Total is null);
}
static void U32(byte[] data, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(offset), value);
static void U64(byte[] data, int offset, ulong value) => BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(offset), value);
static void F64(byte[] data, int offset, double value) => BinaryPrimitives.WriteDoubleLittleEndian(data.AsSpan(offset), value);

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct TestFrameLayout
{
    public ulong PresentCount, Timestamp;
    public double Fps, Frametime, Low1, Low01, HudCpu;
    public uint Api, Flags, GraphCount, Width, Height, Pid;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 120)] public float[] Graph;
}
internal sealed class FakeTransport(byte[] frame, Func<int, ulong> sequence) : IFrameTransport
{
    public int Reads { get; private set; }
    public int Copies { get; private set; }
    public ulong ReadSequence() => sequence(++Reads);
    public void CopyFrame(byte[] destination) { Copies++; frame.CopyTo(destination, 0); }
}
