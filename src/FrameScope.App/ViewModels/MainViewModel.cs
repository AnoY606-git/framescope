using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Threading;
using FrameScope.App.Services;
using FrameScope.Core;

namespace FrameScope.App.ViewModels;

internal sealed record UiChoice(string Label, string Value);

internal sealed class MainViewModel : ObservableObject, IAsyncDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly DemoRuntime _runtime = new();
    private readonly SettingsStore _store;
    private ProfileEditorViewModel? _selectedProfile;
    private int _selectedPage;
    private bool _busy, _running, _hasLaunched, _nativeConfirmed, _dirty, _initializing = true, _disposed;
    private long _editRevision;
    private string _settingsMessage = "Загрузка настроек…";
    private string _operationMessage = "Первый preview поддерживает только собственное демо FrameScope.";
    private string _hotkeyMessage = "Горячие клавиши ещё не зарегистрированы.";
    private string? _activeSessionId;
    private int _apiIndex, _modeIndex, _durationSeconds;
    private double _cpuLoad, _gpuLoad = 12, _stutterMs;
    private bool _vsync = true;
    private bool _canRetryIntel;

    public MainViewModel(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        _store = new(SettingsLocation.Resolve(AppContext.BaseDirectory,
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)));
        LaunchCommand = new(LaunchAsync, OperationFailed, () => !_busy && !_initializing);
        StopCommand = new(StopAsync, OperationFailed, () => !_busy && _hasLaunched);
        SaveCommand = new(async () => await SaveAsync(), OperationFailed, () => !_initializing);
        ResetCommand = new(() => SelectedProfile?.Reset(), () => SelectedProfile is not null);
        DashboardCommand = new(() => SelectedPage = 0);
        ProfilesCommand = new(() => SelectedPage = 1);
        AboutCommand = new(() => SelectedPage = 2);
        RetryIntelCommand = new(() => OperationMessage = _runtime.RetryIntel()
            ? "Intel helper перезапущен. Ожидаем новый snapshot до 30 с; старые поля не используются."
            : "Intel helper уже работает или ещё действует 30-секундная пауза после ошибки.", () => _runtime.CanRetryIntel);
        _runtime.Updated += OnRuntimeUpdate;
    }

    public DashboardViewModel Dashboard { get; } = new();
    public ObservableCollection<ProfileEditorViewModel> Profiles { get; } = [];
    public IReadOnlyList<UiChoice> Apis { get; } = [new("DirectX 11", "dx11"), new("DirectX 12", "dx12"), new("Vulkan", "vulkan")];
    public IReadOnlyList<UiChoice> Modes { get; } = [new("Оконный", "windowed"), new("Без рамки", "borderless"), new("Эксклюзивный*", "exclusive")];
    public AsyncCommand LaunchCommand { get; }
    public AsyncCommand StopCommand { get; }
    public AsyncCommand SaveCommand { get; }
    public RelayCommand ResetCommand { get; }
    public RelayCommand DashboardCommand { get; }
    public RelayCommand ProfilesCommand { get; }
    public RelayCommand AboutCommand { get; }
    public RelayCommand RetryIntelCommand { get; }
    public int SelectedPage { get => _selectedPage; set => Set(ref _selectedPage, value); }
    public int ApiIndex { get => _apiIndex; set => Set(ref _apiIndex, Math.Clamp(value, 0, Apis.Count - 1)); }
    public int ModeIndex { get => _modeIndex; set => Set(ref _modeIndex, Math.Clamp(value, 0, Modes.Count - 1)); }
    public double CpuLoad { get => _cpuLoad; set => Set(ref _cpuLoad, Math.Clamp(value, 0, 100)); }
    public double GpuLoad { get => _gpuLoad; set => Set(ref _gpuLoad, Math.Clamp(value, 1, 100)); }
    public double StutterMs { get => _stutterMs; set => Set(ref _stutterMs, Math.Clamp(value, 0, 5000)); }
    public bool Vsync { get => _vsync; set => Set(ref _vsync, value); }
    public int DurationSeconds { get => _durationSeconds; set => Set(ref _durationSeconds, Math.Clamp(value, 0, 86400)); }
    public bool IsRunning => _running;
    public bool HasUnsavedChanges => _dirty;
    public string LaunchCaption => _busy ? "Подождите…" : _running ? "Перезапустить демо" : "Запустить живое демо";
    public string RuntimeBadge => _running ? "ДЕМО ЗАПУЩЕНО" : "ЛОКАЛЬНЫЙ PREVIEW";
    public string SettingsMessage { get => _settingsMessage; private set => Set(ref _settingsMessage, value); }
    public string OperationMessage { get => _operationMessage; private set => Set(ref _operationMessage, value); }
    public string HotkeyMessage { get => _hotkeyMessage; set => Set(ref _hotkeyMessage, value); }
    public string SettingsPath => _store.Path;
    public string DirtyLabel => _dirty ? "Есть несохранённые изменения" : "Профили не изменены";

    public ProfileEditorViewModel? SelectedProfile
    {
        get => _selectedProfile;
        set
        {
            if (!Set(ref _selectedProfile, value) || value is null) return;
            _runtime.SetProfile(value.Snapshot);
            MarkEdited(); ResetCommand.Refresh();
        }
    }

    public async Task InitializeAsync()
    {
        var loaded = await _store.LoadAsync();
        if (_disposed) return;
        foreach (var profile in loaded.Settings.Profiles)
        {
            var editor = new ProfileEditorViewModel(profile);
            editor.Edited += () => { MarkEdited(); if (ReferenceEquals(editor, SelectedProfile)) _runtime.SetProfile(editor.Snapshot); };
            Profiles.Add(editor);
        }
        SelectedProfile = Profiles.FirstOrDefault(p => p.Id == loaded.Settings.ActiveProfileId) ?? Profiles[0];
        SettingsMessage = loaded.Status switch
        {
            SettingsLoadStatus.Loaded => "Настройки загружены. Сохранение — только по вашей команде.",
            SettingsLoadStatus.Missing => "Новые настройки. Выберите «Сохранить профили», чтобы создать файл.",
            SettingsLoadStatus.Corrupt => $"Файл повреждён; применены безопасные значения. Оригинал не изменён. Копия: {loaded.PreservedPath ?? "не создана"}. {loaded.Message}",
            _ => $"Применены безопасные значения. {loaded.Message}",
        };
        _dirty = false; _initializing = false; Changed(nameof(DirtyLabel)); RefreshCommands();
        _runtime.StartMonitoring();
    }

    public async Task<bool> SaveAsync()
    {
        if (Profiles.Any(p => !p.ColorsValid)) { SettingsMessage = "Сохранение отменено: проверьте цвета всех профилей (#RRGGBB)."; return false; }
        var revision = _editRevision;
        var snapshot = new AppSettings { ActiveProfileId = SelectedProfile?.Id ?? "compact", Profiles = Profiles.Select(p => p.Snapshot).ToList() };
        try
        {
            await _store.SaveAsync(snapshot);
            _dirty = revision != _editRevision; Changed(nameof(DirtyLabel));
            SettingsMessage = _dirty ? "Снимок профилей сохранён. Более новые изменения ещё не сохранены." : $"Профили сохранены: {DateTime.Now:HH:mm:ss}.";
            return !_dirty;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            SettingsMessage = $"Не удалось сохранить. Исходные настройки не заменены: {error.Message}"; return false;
        }
    }

    public void ToggleHud() { if (SelectedProfile is not null) SelectedProfile.Enabled = !SelectedProfile.Enabled; }
    public void CycleProfile()
    {
        if (Profiles.Count > 0) SelectedProfile = Profiles[(Profiles.IndexOf(SelectedProfile!) + 1) % Profiles.Count];
    }

    private async Task LaunchAsync()
    {
        SetBusy(true);
        _nativeConfirmed = false; _hasLaunched = false; _activeSessionId = null;
        try
        {
            _activeSessionId = await _runtime.LaunchAsync(new(Apis[ApiIndex].Value, Modes[ModeIndex].Value,
                (int)CpuLoad, (int)GpuLoad, (int)StutterMs, Vsync, DurationSeconds));
            _hasLaunched = true;
            OperationMessage = "Запрос запуска передан launcher. Ожидаем свежий нативный снимок: только он подтвердит подключение HUD. Esc — выход, Space — статтер, +/− — GPU draw calls.";
        }
        finally { SetBusy(false); }
    }

    private async Task StopAsync()
    {
        SetBusy(true);
        _activeSessionId = null; _hasLaunched = false; _nativeConfirmed = false;
        try { await _runtime.StopAsync(); OperationMessage = "Остановлена только сессия, запущенная этим экземпляром FrameScope."; }
        finally { SetBusy(false); }
    }

    private void SetBusy(bool value) { _busy = value; Changed(nameof(LaunchCaption)); RefreshCommands(); }
    private void RefreshCommands() { LaunchCommand.Refresh(); StopCommand.Refresh(); SaveCommand.Refresh(); }
    private void MarkEdited()
    {
        if (_initializing) return;
        _editRevision++; _dirty = true; Changed(nameof(DirtyLabel));
    }
    private void OperationFailed(Exception error) => OperationMessage = $"Ошибка: {error.Message}";
    private void OnRuntimeUpdate(RuntimeUpdate update)
    {
        if (_disposed || _dispatcher.HasShutdownStarted) return;
        _dispatcher.BeginInvoke(DispatcherPriority.Background, () =>
        {
            if (_disposed) return;
            if (update.SessionId is not null && update.SessionId != _activeSessionId) return;
            Dashboard.Update(update);
            var canRetryIntel = _runtime.CanRetryIntel;
            if (_canRetryIntel != canRetryIntel) { _canRetryIntel = canRetryIntel; RetryIntelCommand.Refresh(); }
            if (_hasLaunched && !_nativeConfirmed && update.FrameStatus == FrameReadStatus.Ready && update.Frame is { } frame)
            {
                _nativeConfirmed = true;
                OperationMessage = $"Получен свежий нативный снимок {frame.Api}, PID {frame.ProcessId}: модуль подключён. Видимость HUD управляется активным профилем. Esc — выход, Space — статтер, +/− — GPU draw calls.";
            }
            if (_running != update.Running)
            {
                _running = update.Running; Changed(nameof(IsRunning)); Changed(nameof(RuntimeBadge)); Changed(nameof(LaunchCaption));
            }
        });
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true; _runtime.Updated -= OnRuntimeUpdate;
        await _runtime.DisposeAsync();
    }
}
