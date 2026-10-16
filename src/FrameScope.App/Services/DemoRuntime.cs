using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using FrameScope.Core;

namespace FrameScope.App.Services;

internal sealed record DemoLaunchOptions(string Api, string Mode, int CpuLoad, int GpuLoad, int StutterMilliseconds, bool Vsync, int DurationSeconds);
internal sealed record RuntimeUpdate(MetricSample Metrics, FrameReadStatus FrameStatus, FrameSnapshot? Frame, bool Running, string Status, string Log, string? SessionId = null);

internal sealed class DemoRuntime : IAsyncDisposable
{
    private readonly object _state = new();
    private readonly SemaphoreSlim _operations = new(1, 1);
    private readonly CancellationTokenSource _shutdown = new();
    private readonly IntelProvider _intelProvider = new();
    private readonly string _nativeDirectory = Path.Combine(AppContext.BaseDirectory, "native");
    private readonly Queue<string> _log = new();
    private HudProfile _profile = new();
    private MetricSample _metrics = MetricSample.Empty;
    private SharedSession? _session;
    private Process? _launcher;
    private Process? _demo;
    private DateTime _launchedUtc;
    private string _status = "Демо не запущено. FPS появится только из настоящих Present-вызовов.";
    private Task? _poller;
    public event Action<RuntimeUpdate>? Updated;

    public void StartMonitoring() { _intelProvider.TryStart(); _poller ??= Task.Run(PollAsync); }
    public bool CanRetryIntel => _intelProvider.Info.CanRetry;
    public bool RetryIntel() => _intelProvider.TryStart();
    public void SetProfile(HudProfile profile) { lock (_state) _profile = profile.Normalize(); }

    public async Task<string> LaunchAsync(DemoLaunchOptions options)
    {
        if (options.Api is not ("dx11" or "dx12" or "vulkan") || options.Mode is not ("windowed" or "borderless" or "exclusive"))
            throw new ArgumentException("Неизвестный API или оконный режим.", nameof(options));
        await _operations.WaitAsync(_shutdown.Token);
        try
        {
            await StopCoreAsync();
            var launcherPath = Path.Combine(_nativeDirectory, "FrameScope.Launcher.exe");
            var demoPath = Path.Combine(_nativeDirectory, "FrameScope.Demo.exe");
            if (!File.Exists(launcherPath)) throw new FileNotFoundException($"Не найден нативный launcher: {launcherPath}. Используйте собранную папку приложения с каталогом native.", launcherPath);
            if (!File.Exists(demoPath)) throw new FileNotFoundException($"Не найдено собственное демо: {demoPath}.", demoPath);
            var session = new SharedSession();
            Process? launcher = null;
            try
            {
                lock (_state)
                {
                    session.WriteControl(_profile, _metrics with { TimestampQpc = (ulong)Stopwatch.GetTimestamp() });
                    session.WriteIntel(_metrics.GetIntelControl((ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency));
                }
                var start = new ProcessStartInfo(launcherPath)
                {
                    WorkingDirectory = _nativeDirectory, UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true,
                };
                Add(start, "--demo", demoPath); Add(start, "--api", options.Api);
                Add(start, "--session", session.Id); Add(start, "--mode", options.Mode);
                Add(start, "--cpu-load", Math.Clamp(options.CpuLoad, 0, 100));
                Add(start, "--gpu-load", Math.Clamp(options.GpuLoad, 1, 100));
                Add(start, "--stutter-ms", Math.Clamp(options.StutterMilliseconds, 0, 5000));
                Add(start, "--vsync", options.Vsync ? "on" : "off");
                if (options.DurationSeconds > 0) Add(start, "--duration", Math.Clamp(options.DurationSeconds, 1, 86400));
                launcher = new Process { StartInfo = start };
                launcher.OutputDataReceived += OnNativeOutput;
                launcher.ErrorDataReceived += OnNativeOutput;
                _launchedUtc = DateTime.UtcNow;
                if (!launcher.Start()) throw new InvalidOperationException("Launcher не запущен.");
                lock (_state)
                {
                    _session = session; _launcher = launcher;
                    _log.Clear(); _status = $"Запуск {options.Api.ToUpperInvariant()} · {options.Mode}; ожидание нативного HUD…";
                }
                launcher.BeginOutputReadLine(); launcher.BeginErrorReadLine();
                return session.Id;
            }
            catch
            {
                // Process создан нами, поэтому только его дерево допустимо остановить при ошибке старта.
                if (launcher is not null) { TryKillOwned(launcher, tree: true); launcher.Dispose(); }
                lock (_state) { if (ReferenceEquals(_session, session)) { _session = null; _launcher = null; } }
                session.Dispose(); throw;
            }
        }
        finally { _operations.Release(); }
    }

    public async Task StopAsync()
    {
        await _operations.WaitAsync();
        try { await StopCoreAsync(); }
        finally { _operations.Release(); }
    }

    private async Task StopCoreAsync()
    {
        Process? demo, launcher;
        SharedSession? session;
        lock (_state)
        {
            demo = _demo; launcher = _launcher; session = _session;
            _demo = null; _launcher = null; _session = null;
            _status = "Демо остановлено. Нативные FPS недоступны.";
        }
        try
        {
            if (demo is not null && IsAlive(demo))
            {
                try { demo.CloseMainWindow(); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { }
                if (!await WaitForExitAsync(demo, TimeSpan.FromSeconds(2))) TryKillOwned(demo, tree: false);
            }
            if (launcher is not null && !await WaitForExitAsync(launcher, TimeSpan.FromSeconds(1)))
                TryKillOwned(launcher, tree: true);
            if (demo is not null) await WaitForExitAsync(demo, TimeSpan.FromSeconds(1));
            if (launcher is not null) await WaitForExitAsync(launcher, TimeSpan.FromSeconds(1));
        }
        finally { demo?.Dispose(); launcher?.Dispose(); session?.Dispose(); }
    }

    private async Task PollAsync()
    {
        using var metrics = new WindowsMetricService();
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(100));
        var ticks = 0;
        try
        {
            do
            {
                if (ticks++ % 5 == 0)
                {
                    try { var sample = _intelProvider.Enrich(metrics.Collect()); lock (_state) _metrics = sample; }
                    catch (Exception error) when (error is System.ComponentModel.Win32Exception or ExternalException or InvalidOperationException)
                    {
                        var unavailable = _intelProvider.Enrich(new() { TimestampQpc = (ulong)Stopwatch.GetTimestamp(), Diagnostics = $"Сбор метрик: {error.Message}" });
                        lock (_state) _metrics = unavailable;
                    }
                }
                RuntimeUpdate update;
                lock (_state)
                {
                    FrameSnapshot? frame = null;
                    var result = FrameReadStatus.Missing;
                    if (_session is not null)
                    {
                        _session.WriteControl(_profile, _metrics);
                        _session.WriteIntel(_metrics.GetIntelControl((ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency));
                        result = _session.ReadFrame(out frame);
                        if (frame is not null)
                        {
                            CaptureOwnedDemo(frame.ProcessId);
                            _status = $"Нативный HUD · {ApiName(frame.Api)} · {frame.Width}×{frame.Height} · PID {frame.ProcessId}";
                        }
                        else if (result == FrameReadStatus.Stale)
                            _status = "Нет свежих Present > 2 с: окно свёрнуто, демо завершено или рендеринг остановлен. FPS: N/A.";
                        else if (_launcher is not null && !IsAlive(_launcher) && !IsAlive(_demo))
                            _status = $"Launcher завершён (код {_launcher.ExitCode}). Проверьте журнал: нативный HUD не подтверждён.";
                    }
                    update = new(_metrics, result, frame, IsAlive(_launcher) || IsAlive(_demo), _status, string.Join(Environment.NewLine, _log), _session?.Id);
                }
                Updated?.Invoke(update);
            } while (await timer.WaitForNextTickAsync(_shutdown.Token));
        }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
        catch (Exception error)
        {
            Updated?.Invoke(new(MetricSample.Empty with { Diagnostics = error.Message }, FrameReadStatus.Invalid, null, false,
                $"Мониторинг остановлен: {error.Message}", ""));
        }
    }

    private void CaptureOwnedDemo(uint pid)
    {
        if (_demo is not null || pid == 0 || pid > int.MaxValue) return;
        Process? process = null;
        try
        {
            process = Process.GetProcessById((int)pid);
            var path = process.MainModule?.FileName;
            var expected = Path.Combine(_nativeDirectory, "FrameScope.Demo.exe");
            if (path is null || !string.Equals(Path.GetFullPath(path), expected, StringComparison.OrdinalIgnoreCase)
                || process.StartTime.ToUniversalTime() < _launchedUtc.AddSeconds(-1)) return;
            _ = process.SafeHandle; // Сохраняем дескриптор именно этого процесса, а не повторно используемый PID.
            _demo = process; process = null;
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException) { }
        finally { process?.Dispose(); }
    }

    private void OnNativeOutput(object sender, DataReceivedEventArgs args)
    {
        if (args.Data is null) return;
        lock (_state)
        {
            _log.Enqueue(args.Data.Length > 1000 ? args.Data[..1000] : args.Data);
            while (_log.Count > 40) _log.Dequeue();
        }
    }

    private static string ApiName(GraphicsApi api) => api switch { GraphicsApi.Dx11 => "DX11", GraphicsApi.Dx12 => "DX12", GraphicsApi.Vulkan => "Vulkan", _ => "Present" };
    private static void Add(ProcessStartInfo start, string key, object value)
    {
        start.ArgumentList.Add(key); start.ArgumentList.Add(Convert.ToString(value, CultureInfo.InvariantCulture)!);
    }
    private static bool IsAlive(Process? process)
    {
        try { return process is not null && !process.HasExited; }
        catch (InvalidOperationException) { return false; }
    }
    private static void TryKillOwned(Process process, bool tree)
    {
        try { if (!process.HasExited) process.Kill(entireProcessTree: tree); }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
    }
    private static async Task<bool> WaitForExitAsync(Process process, TimeSpan timeout)
    {
        if (!IsAlive(process)) return true;
        using var cancellation = new CancellationTokenSource(timeout);
        try { await process.WaitForExitAsync(cancellation.Token); return true; }
        catch (OperationCanceledException) { return false; }
        catch (InvalidOperationException) { return true; }
        catch (System.ComponentModel.Win32Exception) { return false; }
    }

    public async ValueTask DisposeAsync()
    {
        await _shutdown.CancelAsync();
        if (_poller is not null) await _poller;
        await _intelProvider.DisposeAsync();
        await StopAsync();
        _shutdown.Dispose(); _operations.Dispose();
    }
}
