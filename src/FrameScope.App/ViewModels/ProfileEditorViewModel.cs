using FrameScope.Core;

namespace FrameScope.App.ViewModels;

internal sealed class ProfileEditorViewModel(HudProfile profile) : ObservableObject
{
    private HudProfile _profile = profile.Normalize();
    public event Action? Edited;
    public HudProfile Snapshot => _profile.Normalize();
    public string Id => _profile.Id;
    public string Name { get => _profile.Name; set => Update(_profile with { Name = value }); }
    public bool Enabled { get => _profile.Enabled; set => Update(_profile with { Enabled = value }); }
    public int LayoutIndex { get => (int)_profile.Layout; set => Update(_profile with { Layout = (HudLayout)value }); }
    public int CornerIndex { get => (int)_profile.Corner; set => Update(_profile with { Corner = (HudCorner)value }); }
    public double Scale { get => _profile.Scale; set => Update(_profile with { Scale = (float)value }); }
    public double Opacity { get => _profile.Opacity; set => Update(_profile with { Opacity = (float)value }); }
    public double MarginX { get => _profile.MarginX; set => Update(_profile with { MarginX = (float)value }); }
    public double MarginY { get => _profile.MarginY; set => Update(_profile with { MarginY = (float)value }); }
    public string TextColor { get => _profile.TextColor; set => Update(_profile with { TextColor = value }); }
    public string AccentColor { get => _profile.AccentColor; set => Update(_profile with { AccentColor = value }); }
    public string BackgroundColor { get => _profile.BackgroundColor; set => Update(_profile with { BackgroundColor = value }); }
    public bool ShowFps { get => _profile.ShowFps; set => Update(_profile with { ShowFps = value }); }
    public bool ShowFrametime { get => _profile.ShowFrametime; set => Update(_profile with { ShowFrametime = value }); }
    public bool ShowCpu { get => _profile.ShowCpu; set => Update(_profile with { ShowCpu = value }); }
    public bool ShowGpu { get => _profile.ShowGpu; set => Update(_profile with { ShowGpu = value }); }
    public bool ShowRam { get => _profile.ShowRam; set => Update(_profile with { ShowRam = value }); }
    public bool ShowVram { get => _profile.ShowVram; set => Update(_profile with { ShowVram = value }); }
    public bool ShowLows { get => _profile.ShowLows; set => Update(_profile with { ShowLows = value }); }
    public bool ShowGraph { get => _profile.ShowGraph; set => Update(_profile with { ShowGraph = value }); }
    public bool ShowTemperatures { get => _profile.ShowTemperatures; set => Update(_profile with { ShowTemperatures = value }); }
    public bool ShowIntelClocks { get => _profile.ShowIntelClocks; set => Update(_profile with { ShowIntelClocks = value }); }
    public bool ShowIntelPower { get => _profile.ShowIntelPower; set => Update(_profile with { ShowIntelPower = value }); }
    public bool ShowIntelBandwidth { get => _profile.ShowIntelBandwidth; set => Update(_profile with { ShowIntelBandwidth = value }); }
    public bool ShowIntelThrottle { get => _profile.ShowIntelThrottle; set => Update(_profile with { ShowIntelThrottle = value }); }
    public bool ColorsValid => HudProfile.IsColor(TextColor) && HudProfile.IsColor(AccentColor) && HudProfile.IsColor(BackgroundColor);
    public string ValidationMessage => ColorsValid ? "Цвета в формате #RRGGBB. Изменения сразу передаются работающему демо." : "Некорректный цвет: требуется #RRGGBB. Для HUD временно используется безопасный цвет.";

    public void Reset()
    {
        _profile = AppSettings.DefaultProfiles().FirstOrDefault(p => p.Id == Id) ?? new() { Id = Id, Name = Name };
        Changed(null); Edited?.Invoke();
    }

    private void Update(HudProfile profile, [System.Runtime.CompilerServices.CallerMemberName] string? property = null)
    {
        if (_profile == profile) return;
        _profile = profile; Changed(property); Changed(nameof(ValidationMessage)); Edited?.Invoke();
    }
}
