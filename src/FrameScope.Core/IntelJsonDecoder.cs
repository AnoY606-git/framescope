using System.Globalization;
using System.Text;
using System.Text.Json;

namespace FrameScope.Core;

public enum IntelDecodeStatus { Ready, BackendUnavailable, Invalid, UnsupportedSchema, UnsupportedSource, Future, Stale, BeforeProcess, Replay }
public sealed record IntelPciAddress(uint Domain, uint Bus, uint Device, uint Function);
public sealed record IntelJsonReading(string Key, string Scope, string Instance, uint? SubdeviceId, string Unit,
    string Status, double? Value, string? Reason, double? WindowMs, string? Note, IReadOnlyList<string> Flags);
public sealed record IntelJsonDevice(string Id, uint VendorId, IntelPciAddress? Pci, IReadOnlyList<IntelJsonReading> Readings);
public sealed record IntelJsonSnapshot(ulong MonotonicMs, ulong Sequence, ulong UpdatedQpc, string Mode,
    string BackendStatus, string? BackendReason, IReadOnlyList<IntelJsonDevice> Devices)
{
    public bool RequiresRestart => BackendStatus != "available" || Devices.Any(device => device.Readings.Any(reading =>
        reading.Reason?.Contains("ZE_RESULT_ERROR_DEVICE_LOST", StringComparison.Ordinal) == true
        || reading.Reason?.Contains("ZE_RESULT_ERROR_DEVICE_REQUIRES_RESET", StringComparison.Ordinal) == true));
}
public sealed record IntelDecodeResult(IntelDecodeStatus Status, IntelJsonSnapshot? Snapshot, string Diagnostic);

public static class IntelJsonDecoder
{
    public const int MaximumLineCharacters = 512 * 1024;
    public const int MaximumJsonBytes = 1024 * 1024;
    public const int MaximumDevices = 16;
    public const int MaximumReadings = 512;
    public const ulong MaximumAgeMilliseconds = 5000;

    public static IntelDecodeResult Decode(string? json, long arrivalTickMs, ulong arrivalQpc, ulong frequency, long processStartTickMs = 0)
    {
        if (json is null) return Fail(IntelDecodeStatus.Invalid, "Нет Intel JSON Line.");
        if (json.Length > MaximumLineCharacters || Encoding.UTF8.GetByteCount(json) > MaximumJsonBytes)
            return Fail(IntelDecodeStatus.Invalid, "JSON Line превышает допустимый размер.");
        try
        {
            using var document = JsonDocument.Parse(json, new() { MaxDepth = 16, AllowTrailingCommas = false, CommentHandling = JsonCommentHandling.Disallow });
            var root = document.RootElement;
            UniqueProperties(root);
            if (Unsigned(root, "schemaVersion") != 1) return Fail(IntelDecodeStatus.UnsupportedSchema, "Неизвестная schemaVersion Intel JSON.");
            if (Text(root, "source") != "level-zero-sysman") return Fail(IntelDecodeStatus.UnsupportedSource, "Неизвестный source Intel JSON.");
            var backend = Object(root, "backend");
            var backendStatus = Text(backend, "status");
            var reason = OptionalText(backend, "reason");
            var mode = Text(backend, "mode");
            var devices = Array(root, "devices", MaximumDevices);
            if (backendStatus != "available")
                return new(IntelDecodeStatus.BackendUnavailable, new(0, 0, 0, mode, backendStatus, reason, []),
                    $"Sysman backend: {backendStatus}; {reason ?? "причина не сообщена"}.");
            if (mode is not ("legacy" or "standalone")) throw new JsonException("Неизвестный init mode.");
            var timestamp = Text(root, "timestamp");
            if (!DateTimeOffset.TryParse(timestamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out var utc)
                || utc.Offset != TimeSpan.Zero || !timestamp.EndsWith('Z')) throw new JsonException("Некорректный UTC timestamp.");
            var monotonic = U64(root, "monotonicMs"); var sequence = U64(root, "sequence");
            var interval = Unsigned(root, "intervalMs");
            if (interval is < 500 or > 5000) throw new JsonException("Некорректный intervalMs.");
            if (arrivalTickMs < 0 || processStartTickMs < 0 || frequency == 0 || arrivalQpc == 0 || sequence == 0)
                throw new JsonException("Некорректная идентичность времени/sequence.");
            if (monotonic > (ulong)arrivalTickMs) return Fail(IntelDecodeStatus.Future, "monotonicMs находится в будущем.");
            if (monotonic < (ulong)processStartTickMs) return Fail(IntelDecodeStatus.BeforeProcess, "Snapshot старше запущенного helper-процесса.");
            var age = (ulong)arrivalTickMs - monotonic;
            if (age > MaximumAgeMilliseconds) return Fail(IntelDecodeStatus.Stale, "Snapshot Intel устарел (>5 с).");
            var qpcAge = (UInt128)age * frequency / 1000;
            if (qpcAge >= arrivalQpc) throw new JsonException("Возраст невозможно сопоставить с QPC.");
            var output = new List<IntelJsonDevice>();
            var deviceIds = new HashSet<string>(StringComparer.Ordinal);
            foreach (var device in devices.EnumerateArray())
            {
                var id = Text(device, "id");
                if (!deviceIds.Add(id)) throw new JsonException("Повторная идентичность устройства.");
                var vendor = Unsigned(device, "vendorId");
                IntelPciAddress? pci = null;
                if (!device.TryGetProperty("pci", out var address)) throw new JsonException("Нет PCI identity.");
                if (address.ValueKind != JsonValueKind.Null)
                {
                    var domain = Unsigned(address, "domain"); var bus = Unsigned(address, "bus");
                    var slot = Unsigned(address, "device"); var function = Unsigned(address, "function");
                    if (domain > 65535 || bus > 255 || slot > 31 || function > 7) throw new JsonException("Некорректный PCI address.");
                    pci = new(domain, bus, slot, function);
                }
                var readings = new List<IntelJsonReading>();
                var identities = new HashSet<(string, string, string, uint?)>();
                foreach (var item in Array(device, "readings", MaximumReadings).EnumerateArray())
                {
                    var key = Text(item, "key"); var scope = Text(item, "scope"); var instance = Text(item, "instance");
                    uint? subdevice = null;
                    if (!item.TryGetProperty("subdeviceId", out var sub)) throw new JsonException("Нет subdeviceId.");
                    if (sub.ValueKind != JsonValueKind.Null) subdevice = sub.GetUInt32();
                    if (!identities.Add((key, scope, instance, subdevice))) throw new JsonException("Повторная идентичность reading.");
                    var unit = Text(item, "unit"); var status = Text(item, "status");
                    var readingReason = OptionalText(item, "reason");
                    double? value = null;
                    if (!item.TryGetProperty("value", out var number)) throw new JsonException("Нет value.");
                    if (status == "available")
                    {
                        if (number.ValueKind == JsonValueKind.Number && number.TryGetDouble(out var n) && double.IsFinite(n) && n >= 0) value = n;
                        else { status = "invalid_data"; readingReason = "nonfinite_negative_or_missing_numeric_value"; }
                    }
                    double? window = null;
                    if (item.TryGetProperty("windowMs", out var w) && w.ValueKind != JsonValueKind.Null)
                    {
                        if (w.ValueKind != JsonValueKind.Number || !w.TryGetDouble(out var milliseconds) || !double.IsFinite(milliseconds) || milliseconds <= 0)
                        { value = null; status = "invalid_data"; readingReason = "invalid_window_ms"; }
                        else window = milliseconds;
                    }
                    if (status == "available")
                    {
                        var expected = key switch
                        {
                            "temperature" => "C", "frequency.actual" or "frequency.requested" => "MHz", "voltage" => "V",
                            "frequency.throttle_reasons" => "bitmask", "power.average" => "W", "fan.speed" => "RPM",
                            "memory.physical_bytes" or "memory.total_bytes" or "memory.free_bytes" or "memory.used_bytes" => "B",
                            "memory.read_bandwidth" or "memory.write_bandwidth" => "B/s", "engine.utilization" => "%", _ => unit,
                        };
                        if (unit != expected) { value = null; status = "invalid_data"; readingReason = "unit_mismatch"; }
                        else if (key is "power.average" or "memory.read_bandwidth" or "memory.write_bandwidth" && window is null)
                        { value = null; status = "invalid_data"; readingReason = "counter_window_missing"; }
                        else if (readingReason == "zero_counter_without_validity_flag")
                        { value = null; status = "unavailable"; }
                        else if (key == "temperature" && value > 200)
                        { value = null; status = "invalid_data"; readingReason = "temperature_out_of_range"; }
                    }
                    var flags = Array(item, "flags", 64).EnumerateArray().Select(flag =>
                        flag.ValueKind == JsonValueKind.String && flag.GetString() is { Length: <= 128 } s ? s : throw new JsonException("Некорректный flags элемент.")).ToArray();
                    readings.Add(new(key, scope, instance, subdevice, unit, status, value, readingReason, window, OptionalText(item, "note"), System.Array.AsReadOnly(flags)));
                }
                output.Add(new(id, vendor, pci, readings.AsReadOnly()));
            }
            return new(IntelDecodeStatus.Ready, new(monotonic, sequence, arrivalQpc - (ulong)qpcAge, mode, backendStatus, reason, output.AsReadOnly()), "Свежий Sysman snapshot.");
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or FormatException or OverflowException)
        { return Fail(IntelDecodeStatus.Invalid, $"Некорректный Intel JSON: {error.Message}"); }
    }

    private static IntelDecodeResult Fail(IntelDecodeStatus status, string diagnostic) => new(status, null, diagnostic);
    private static JsonElement Object(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.Object ? value : throw new JsonException($"Нет объекта {key}.");
    private static JsonElement Array(JsonElement parent, string key, int maximum) => parent.TryGetProperty(key, out var value)
        && value.ValueKind == JsonValueKind.Array && value.GetArrayLength() <= maximum ? value : throw new JsonException($"Некорректный/слишком большой массив {key}.");
    private static string Text(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String
        && value.GetString() is { Length: <= 512 } text ? text : throw new JsonException($"Нет строки {key}.");
    private static string? OptionalText(JsonElement parent, string key) => !parent.TryGetProperty(key, out var value) || value.ValueKind == JsonValueKind.Null ? null : Text(parent, key);
    private static uint Unsigned(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) && value.TryGetUInt32(out var number) ? number : throw new JsonException($"Некорректный uint {key}.");
    private static ulong U64(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) && value.TryGetUInt64(out var number) ? number : throw new JsonException($"Некорректный uint64 {key}.");
    private static void UniqueProperties(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var keys = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
            {
                if (!keys.Add(property.Name)) throw new JsonException("Повторное JSON property.");
                UniqueProperties(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array) foreach (var item in element.EnumerateArray()) UniqueProperties(item);
    }
}

public sealed class IntelSnapshotSequence
{
    private ulong _sequence, _monotonic;
    public bool TryAccept(IntelJsonSnapshot snapshot)
    {
        if (snapshot.Sequence == 0 || snapshot.Sequence <= _sequence || snapshot.MonotonicMs <= _monotonic) return false;
        _sequence = snapshot.Sequence; _monotonic = snapshot.MonotonicMs; return true;
    }
}
