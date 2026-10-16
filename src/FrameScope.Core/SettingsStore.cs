using System.Text.Json;
using System.Text.Json.Serialization;

namespace FrameScope.Core;

public enum SettingsLoadStatus { Missing, Loaded, Corrupt, UnsupportedVersion, ReadError }

public sealed record SettingsLoadResult(AppSettings Settings, SettingsLoadStatus Status, string? Message = null, string? PreservedPath = null);

public static class SettingsLocation
{
    public static string Resolve(string executableDirectory, string localApplicationData) =>
        Path.Combine(File.Exists(Path.Combine(executableDirectory, "portable.txt"))
            ? executableDirectory : Path.Combine(localApplicationData, "FrameScope"), "settings.json");
}

public sealed class SettingsStore(string path)
{
    private const long MaximumFileBytes = 1024 * 1024;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true, PropertyNameCaseInsensitive = true,
        Converters = { new JsonStringEnumConverter() },
    };
    public string Path { get; } = System.IO.Path.GetFullPath(path);

    public async Task<SettingsLoadResult> LoadAsync(CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!File.Exists(Path)) return new(new(), SettingsLoadStatus.Missing);
            try
            {
                if (new FileInfo(Path).Length > MaximumFileBytes) throw new JsonException("Файл настроек превышает 1 МиБ.");
                await using var stream = new FileStream(Path, FileMode.Open, FileAccess.Read, FileShare.Read);
                var settings = await JsonSerializer.DeserializeAsync<AppSettings>(stream, JsonOptions, cancellationToken).ConfigureAwait(false)
                    ?? throw new JsonException("Пустой документ настроек.");
                if (settings.Version != AppSettings.CurrentVersion)
                    return new(new(), SettingsLoadStatus.UnsupportedVersion,
                        $"Версия настроек {settings.Version} не поддерживается. Исходный файл не изменён.");
                return new(settings.Normalize(), SettingsLoadStatus.Loaded);
            }
            catch (JsonException error)
            {
                return PreserveCorrupt(error.Message);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                return new(new(), SettingsLoadStatus.ReadError, error.Message);
            }
        }
        finally { _gate.Release(); }
    }

    // Вызывается только после явного «Сохранить»: загрузка никогда не перезаписывает исходный файл.
    public async Task SaveAsync(AppSettings settings, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(settings);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        string? temporary = null;
        try
        {
            var directory = System.IO.Path.GetDirectoryName(Path)!;
            Directory.CreateDirectory(directory);
            await PreserveUnrecognizedExistingAsync(cancellationToken).ConfigureAwait(false);
            temporary = System.IO.Path.Combine(directory, $".settings-{Guid.NewGuid():N}.tmp");
            await using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                4096, FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(stream, settings.Normalize(), JsonOptions, cancellationToken).ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
                stream.Flush(flushToDisk: true);
            }
            cancellationToken.ThrowIfCancellationRequested();
            if (File.Exists(Path)) File.Replace(temporary, Path, null);
            else File.Move(temporary, Path);
            temporary = null;
        }
        finally
        {
            if (temporary is not null)
            {
                try { File.Delete(temporary); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
            _gate.Release();
        }
    }

    private SettingsLoadResult PreserveCorrupt(string message)
    {
        try
        {
            var preserved = BackupPath("corrupt");
            File.Copy(Path, preserved, overwrite: false);
            return new(new(), SettingsLoadStatus.Corrupt, message, preserved);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return new(new(), SettingsLoadStatus.Corrupt, $"{message} Копия не создана: {error.Message}");
        }
    }

    private async Task PreserveUnrecognizedExistingAsync(CancellationToken cancellationToken)
    {
        if (!File.Exists(Path)) return;
        var preserve = false;
        try
        {
            if (new FileInfo(Path).Length > MaximumFileBytes) preserve = true;
            else
            {
                await using var stream = File.OpenRead(Path);
                var existing = await JsonSerializer.DeserializeAsync<AppSettings>(stream, JsonOptions, cancellationToken).ConfigureAwait(false);
                preserve = existing is null || existing.Version != AppSettings.CurrentVersion;
            }
        }
        catch (JsonException) { preserve = true; }
        // Если сохранить оригинал не удалось, операция прерывается, а не уничтожает его.
        if (preserve) File.Copy(Path, BackupPath("preserved"), overwrite: false);
    }

    private string BackupPath(string reason) => $"{Path}.{reason}-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}";
}
