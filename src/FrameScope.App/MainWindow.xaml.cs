using System.ComponentModel;
using System.Windows;
using System.Windows.Interop;
using FrameScope.App.Services;
using FrameScope.App.ViewModels;

namespace FrameScope.App;

public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel;
    private GlobalHotkeys? _hotkeys;
    private bool _closing, _closed;

    public MainWindow()
    {
        InitializeComponent();
        _viewModel = new(Dispatcher); DataContext = _viewModel;
    }

    private async void WindowLoaded(object sender, RoutedEventArgs e)
    {
        try { await _viewModel.InitializeAsync(); }
        catch (Exception error) { MessageBox.Show(this, error.Message, "FrameScope — запуск", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    private void WindowSourceInitialized(object? sender, EventArgs e)
    {
        try
        {
            _hotkeys = new(new WindowInteropHelper(this).Handle, _viewModel.ToggleHud, _viewModel.CycleProfile);
            _viewModel.HotkeyMessage = _hotkeys.Status;
        }
        catch (Exception error) { _viewModel.HotkeyMessage = $"Горячие клавиши недоступны: {error.Message}"; }
    }

    private async void WindowClosing(object? sender, CancelEventArgs e)
    {
        if (_closed) return;
        e.Cancel = true;
        if (_closing) return;
        _closing = true;
        try
        {
            if (_viewModel.HasUnsavedChanges)
            {
                var choice = MessageBox.Show(this, "Сохранить изменения профилей перед выходом?", "FrameScope", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
                if (choice == MessageBoxResult.Cancel) return;
                if (choice == MessageBoxResult.Yes && !await _viewModel.SaveAsync())
                {
                    MessageBox.Show(this, _viewModel.SettingsMessage, "FrameScope — настройки", MessageBoxButton.OK, MessageBoxImage.Warning); return;
                }
            }
            IsEnabled = false; _hotkeys?.Dispose();
            await _viewModel.DisposeAsync();
            _closed = true; Close();
        }
        catch (Exception error)
        {
            IsEnabled = true;
            MessageBox.Show(this, $"Не удалось корректно завершить сессию: {error.Message}", "FrameScope", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally { _closing = false; }
    }

    private void MinimizeClicked(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void MaximizeClicked(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void CloseClicked(object sender, RoutedEventArgs e) => Close();
}
