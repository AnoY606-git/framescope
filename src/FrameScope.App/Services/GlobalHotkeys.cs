using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace FrameScope.App.Services;

internal sealed class GlobalHotkeys : IDisposable
{
    private readonly HwndSource _source;
    private readonly Action _toggle, _cycle;
    private readonly bool _toggleRegistered, _cycleRegistered;
    private const int ToggleId = 0x4650, CycleId = 0x4651;
    public string Status { get; }

    public GlobalHotkeys(IntPtr window, Action toggle, Action cycle)
    {
        _source = HwndSource.FromHwnd(window) ?? throw new InvalidOperationException("Нет HWND для горячих клавиш.");
        _toggle = toggle; _cycle = cycle;
        _source.AddHook(WindowMessage);
        _toggleRegistered = RegisterHotKey(window, ToggleId, 0x4000, 0x77);
        var toggleError = _toggleRegistered ? 0 : Marshal.GetLastWin32Error();
        _cycleRegistered = RegisterHotKey(window, CycleId, 0x4000 | 0x2, 0x77);
        var cycleError = _cycleRegistered ? 0 : Marshal.GetLastWin32Error();
        Status = $"F8 — HUD: {Describe(toggleError)}; Ctrl+F8 — профиль: {Describe(cycleError)}.";
    }

    private IntPtr WindowMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (message != 0x0312) return IntPtr.Zero;
        if (wParam.ToInt32() == ToggleId) { _toggle(); handled = true; }
        if (wParam.ToInt32() == CycleId) { _cycle(); handled = true; }
        return IntPtr.Zero;
    }

    public void Dispose()
    {
        if (_toggleRegistered) UnregisterHotKey(_source.Handle, ToggleId);
        if (_cycleRegistered) UnregisterHotKey(_source.Handle, CycleId);
        _source.RemoveHook(WindowMessage);
    }
    private static string Describe(int error) => error switch
    {
        0 => "включено", 1409 => "занято другим приложением", _ => $"N/A (Win32 {error})",
    };
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RegisterHotKey(IntPtr window, int id, uint modifiers, uint key);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterHotKey(IntPtr window, int id);
}
