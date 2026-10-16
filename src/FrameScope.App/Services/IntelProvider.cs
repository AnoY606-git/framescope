using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using FrameScope.Core;

namespace FrameScope.App.Services;

internal enum IntelProviderState { NotStarted, Starting, Live, Missing, Blocked, Faulted, Stopped }
internal sealed record IntelProviderInfo(IntelProviderState State, string Diagnostic, int? ProcessId, bool CanRetry);

internal sealed class IntelProvider : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly CancellationTokenSource _shutdown = new();
    private readonly string _executable;
    private Task? _worker;
    private Task? _cleanup;
    private Process? _process;
    private IntelJsonSnapshot? _latest;
    private IntelProviderState _state;
    private string _diagnostic = "Intel: N/A — поставщик ещё не запущен.";
    private string _stderr = "";
    private long _retryAfter;
    private bool _disposed;
    private const int RetryCooldownMilliseconds = 30000;

    public IntelProvider() : this(Path.Combine(AppContext.BaseDirectory, "native", "FrameScope.Intel.exe")) { }
    internal IntelProvider(string executable) => _executable = Path.GetFullPath(executable);

    public IntelProviderInfo Info
    {
        get
        {
            lock (_gate) return new(_state, _diagnostic, _process is { } process ? TryId(process) : null,
                !_disposed && (_worker is null || _worker.IsCompleted) && Environment.TickCount64 >= _retryAfter);
        }
    }

    public bool TryStart()
    {
        lock (_gate)
        {
            if (_disposed || _worker is { IsCompleted: false } || Environment.TickCount64 < _retryAfter) return false;
            _latest = null; _stderr = ""; _state = IntelProviderState.Starting;
            _cleanup = null;
            _diagnostic = "Intel: N/A — запуск --watch --legacy; первый snapshot может потребовать до 30 с.";
            _worker = Task.Run(RunAsync); return true;
        }
    }

    public MetricSample Enrich(MetricSample baseline)
    {
        IntelJsonSnapshot? snapshot; string diagnostic;
        lock (_gate) { snapshot = _latest; diagnostic = _diagnostic + (_stderr.Length == 0 ? "" : $"\nstderr: {_stderr}"); }
        return IntelSnapshotProjector.Enrich(baseline, snapshot, Environment.TickCount64, (ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency, diagnostic);
    }

    private async Task RunAsync()
    {
        Process? process = null;
        Task? stderr = null;
        using var drainCancellation = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
        try
        {
            if (!File.Exists(_executable))
            { Fail(IntelProviderState.Missing, $"Intel: N/A — не найден {_executable}. После установки native helper нажмите «Повторить Intel». Запись из .json не используется.", cooldown: false); return; }
            var start = new ProcessStartInfo(_executable)
            {
                WorkingDirectory = Path.GetDirectoryName(_executable)!, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = new UTF8Encoding(false, true), StandardErrorEncoding = Encoding.UTF8,
            };
            start.ArgumentList.Add("--watch"); start.ArgumentList.Add("--legacy");
            start.ArgumentList.Add("--interval-ms"); start.ArgumentList.Add("1000");
            process = new Process { StartInfo = start };
            var startTick = Environment.TickCount64;
            if (!process.Start()) throw new InvalidOperationException("CreateProcess не запустил Intel helper.");
            _ = process.SafeHandle; // Удерживаем дескриптор созданного процесса, а не позднее переиспользованный PID.
            lock (_gate) _process = process;
            stderr = DrainStderrAsync(process.StandardError, drainCancellation.Token);
            var reader = new BoundedLineReader(process.StandardOutput);
            var sequence = new IntelSnapshotSequence();
            var first = true;
            while (!_shutdown.IsCancellationRequested)
            {
                using var deadline = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
                deadline.CancelAfter(first ? TimeSpan.FromSeconds(30) : TimeSpan.FromSeconds(6));
                string? line;
                try { line = await reader.ReadLineAsync(deadline.Token).ConfigureAwait(false); }
                catch (OperationCanceledException) when (!_shutdown.IsCancellationRequested)
                { Fail(IntelProviderState.Faulted, first ? "Intel: N/A — timeout первого snapshot (30 с)." : "Intel: N/A — watch не прислал строку за 6 с; старые поля удалены."); return; }
                if (line is null)
                { Fail(IntelProviderState.Faulted, $"Intel: N/A — stdout закрыт, helper завершился{ExitDescription(process)}. Повтор возможен через 30 с."); return; }
                var decoded = IntelJsonDecoder.Decode(line, Environment.TickCount64, (ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency, startTick);
                if (decoded.Status != IntelDecodeStatus.Ready || decoded.Snapshot is not { } snapshot)
                { Fail(IntelProviderState.Faulted, $"Intel: N/A — {decoded.Diagnostic} Повтор возможен через 30 с."); return; }
                if (snapshot.Mode != "legacy" || !sequence.TryAccept(snapshot))
                { Fail(IntelProviderState.Faulted, "Intel: N/A — неожиданный mode или повторная/обратная sequence/monotonic identity. Старые поля удалены."); return; }
                if (snapshot.RequiresRestart)
                { Fail(IntelProviderState.Faulted, "Intel: N/A — Sysman сообщил DEVICE_LOST / DEVICE_REQUIRES_RESET (hotplug/driver reset). Данные очищены; через 30 с можно повторить helper. Reset API не вызывается."); return; }
                lock (_gate)
                {
                    _latest = snapshot; _state = IntelProviderState.Live;
                    _diagnostic = "Intel: живой read-only Level Zero Sysman --legacy; данные только из stdout текущего helper.";
                }
                first = false;
            }
        }
        catch (Win32Exception error) when (error.NativeErrorCode == 4551)
        { Fail(IntelProviderState.Blocked, "Windows Smart App Control заблокировал Intel провайдер (4551). Intel-поля: N/A. Нужна разрешённая/доверенная сборка; защита не отключается, старые probe-файлы не используются."); }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
        catch (Exception error) when (error is Win32Exception or IOException or InvalidOperationException or DecoderFallbackException or ObjectDisposedException)
        {
            if (!_shutdown.IsCancellationRequested) Fail(IntelProviderState.Faulted, $"Intel: N/A — {error.Message}. Повтор возможен через 30 с.");
        }
        finally
        {
            if (process is not null) await CloseOwnedAsync(process).ConfigureAwait(false);
            await drainCancellation.CancelAsync().ConfigureAwait(false);
            if (stderr is not null)
            {
                try { await stderr.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false); }
                catch (Exception error) when (error is TimeoutException or OperationCanceledException or IOException or ObjectDisposedException) { }
            }
            lock (_gate)
            {
                _process = null; _latest = null;
                if (_shutdown.IsCancellationRequested) { _state = IntelProviderState.Stopped; _diagnostic = "Intel: N/A — поставщик остановлен."; }
            }
            process?.Dispose();
        }
    }

    private async Task DrainStderrAsync(TextReader reader, CancellationToken cancellationToken)
    {
        var buffer = new char[1024];
        try
        {
            int count;
            while ((count = await reader.ReadAsync(buffer.AsMemory(), cancellationToken).ConfigureAwait(false)) > 0)
            {
                lock (_gate)
                {
                    _stderr += new string(buffer, 0, count);
                    if (_stderr.Length > 8192) _stderr = _stderr[^8192..];
                }
            }
        }
        catch (Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
    }

    private void Fail(IntelProviderState state, string message, bool cooldown = true)
    { lock (_gate) { _latest = null; _state = state; _diagnostic = message; _retryAfter = Environment.TickCount64 + (cooldown ? RetryCooldownMilliseconds : 0); } }
    private static int? TryId(Process process) { try { return process.Id; } catch (InvalidOperationException) { return null; } }
    private static string ExitDescription(Process process)
    { try { return process.HasExited ? $" (код {process.ExitCode})" : ""; } catch (InvalidOperationException) { return ""; } }
    private Task CloseOwnedAsync(Process process)
    {
        lock (_gate) return _cleanup ??= CloseProcessAsync(process);
    }

    private static async Task CloseProcessAsync(Process process)
    {
        // Закрытие pipe выполняется вне UI и одновременно с grace-period: vendor call может зависнуть.
        var closeOutput = Task.Run(() =>
        {
            try { process.StandardOutput.Dispose(); } catch (InvalidOperationException) { } catch (IOException) { }
        });
        try
        {
            if (!process.HasExited)
            {
                try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false); }
                catch (TimeoutException)
                {
                    // Никакой PID enumeration / Kill(tree): завершаем только удерживаемый собственный CLI.
                    process.Kill(entireProcessTree: false);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
                }
            }
        }
        catch (Exception error) when (error is InvalidOperationException or Win32Exception or TimeoutException) { }
        try { await closeOutput.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false); } catch (TimeoutException) { }
        try { process.StandardError.Dispose(); } catch (InvalidOperationException) { } catch (IOException) { }
    }

    public async ValueTask DisposeAsync()
    {
        Task? worker; Process? process;
        lock (_gate) { if (_disposed) return; _disposed = true; worker = _worker; process = _process; }
        await _shutdown.CancelAsync().ConfigureAwait(false);
        if (process is not null) await CloseOwnedAsync(process).ConfigureAwait(false);
        if (worker is not null) await worker.ConfigureAwait(false);
        _shutdown.Dispose();
    }
}
