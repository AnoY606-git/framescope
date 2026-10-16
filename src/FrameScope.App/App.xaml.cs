using System.Runtime.InteropServices;
using System.Windows;
using FrameScope.App.Services;

[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]

namespace FrameScope.App;

public partial class App : Application
{
    protected override async void OnStartup(StartupEventArgs e)
    {
        if (!Environment.Is64BitProcess)
        {
            MessageBox.Show("FrameScope требует процесс x64.", "FrameScope", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1); return;
        }
        if (e.Args.Contains("--diagnostics-intel", StringComparer.Ordinal))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            base.OnStartup(e);
            var seconds = 40;
            var index = Array.IndexOf(e.Args, "--diagnostics-seconds");
            if (index >= 0 && index + 1 < e.Args.Length && int.TryParse(e.Args[index + 1], out var requested)) seconds = Math.Clamp(requested, 1, 90);
            try { var code = await IntelDiagnosticsRunner.RunAsync(seconds); Environment.ExitCode = code; Shutdown(code); }
            catch (Exception error) { Console.Error.WriteLine($"Intel controller diagnostics: {error.Message}"); Environment.ExitCode = 1; Shutdown(1); }
            return;
        }
        base.OnStartup(e);
        MainWindow = new MainWindow(); MainWindow.Show();
    }
}
