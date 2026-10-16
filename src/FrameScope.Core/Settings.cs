using System.Globalization;
using System.Text.Json.Serialization;

namespace FrameScope.Core;

public enum HudCorner { TopLeft, TopRight, BottomLeft, BottomRight }
public enum HudLayout { Panel, Strip }

public sealed record HudProfile
{
    public string Id { get; init; } = "compact";
    public string Name { get; init; } = "Компактный";
    public bool Enabled { get; init; } = true;
    public HudLayout Layout { get; init; } = HudLayout.Strip;
    public HudCorner Corner { get; init; } = HudCorner.TopLeft;
    public float Scale { get; init; } = 1;
    public float Opacity { get; init; } = .88f;
    public float MarginX { get; init; } = 16;
    public float MarginY { get; init; } = 16;
    public string TextColor { get; init; } = "#EAF1FF";
    public string AccentColor { get; init; } = "#5EEAD4";
    public string BackgroundColor { get; init; } = "#111827";
    public bool ShowFps { get; init; } = true;
    public bool ShowFrametime { get; init; } = true;
    public bool ShowCpu { get; init; } = true;
    public bool ShowGpu { get; init; } = true;
    public bool ShowRam { get; init; } = true;
    public bool ShowVram { get; init; } = true;
    public bool ShowLows { get; init; }
    public bool ShowGraph { get; init; } = true;
    public bool ShowTemperatures { get; init; }
    public bool ShowIntelClocks { get; init; }
    public bool ShowIntelPower { get; init; }
    public bool ShowIntelBandwidth { get; init; }
    public bool ShowIntelThrottle { get; init; }

    [JsonIgnore]
    public HudFlags Flags => (Enabled ? HudFlags.Enabled : 0)
        | (Layout == HudLayout.Strip ? HudFlags.Horizontal : 0)
        | (ShowGraph ? HudFlags.Graph : 0) | (ShowFps ? HudFlags.ShowFps : 0)
        | (ShowFrametime ? HudFlags.ShowFrametime : 0) | (ShowCpu ? HudFlags.ShowCpu : 0)
        | (ShowGpu ? HudFlags.ShowGpu : 0) | (ShowRam ? HudFlags.ShowRam : 0)
        | (ShowVram ? HudFlags.ShowVram : 0) | (ShowLows ? HudFlags.ShowLows : 0)
        | (ShowTemperatures ? HudFlags.ShowTemperatures : 0)
        | (ShowIntelClocks ? HudFlags.ShowIntelClocks : 0) | (ShowIntelPower ? HudFlags.ShowIntelPower : 0)
        | (ShowIntelBandwidth ? HudFlags.ShowIntelBandwidth : 0) | (ShowIntelThrottle ? HudFlags.ShowIntelThrottle : 0);

    public HudProfile Normalize() => this with
    {
        Id = Clean(Id, "profile", 48), Name = Clean(Name, "Профиль", 64),
        Layout = Enum.IsDefined(Layout) ? Layout : HudLayout.Panel,
        Corner = Enum.IsDefined(Corner) ? Corner : HudCorner.TopLeft,
        Scale = Clamp(Scale, .6f, 2.5f, 1), Opacity = Clamp(Opacity, .15f, 1, .88f),
        MarginX = Clamp(MarginX, 0, 240, 16), MarginY = Clamp(MarginY, 0, 240, 16),
        TextColor = NormalizeColor(TextColor, "#EAF1FF"),
        AccentColor = NormalizeColor(AccentColor, "#5EEAD4"),
        BackgroundColor = NormalizeColor(BackgroundColor, "#111827"),
    };

    public static bool IsColor(string? value) => value is { Length: 7 } && value[0] == '#'
        && uint.TryParse(value.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out _);

    // Протокол использует 0xAARRGGBB; прозрачность панели передаётся отдельно.
    public static uint PackColor(string value) => 0xFF000000u
        | uint.Parse(NormalizeColor(value, "#FFFFFF").AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture);

    private static string NormalizeColor(string? value, string fallback) => IsColor(value) ? value!.ToUpperInvariant() : fallback;
    private static float Clamp(float value, float min, float max, float fallback) => float.IsFinite(value) ? Math.Clamp(value, min, max) : fallback;
    private static string Clean(string? value, string fallback, int max) => string.IsNullOrWhiteSpace(value)
        ? fallback : value.Trim()[..Math.Min(value.Trim().Length, max)];
}

public sealed record AppSettings
{
    public const int CurrentVersion = 1;
    public int Version { get; init; } = CurrentVersion;
    public string ActiveProfileId { get; init; } = "compact";
    public List<HudProfile> Profiles { get; init; } = DefaultProfiles();

    public AppSettings Normalize()
    {
        var profiles = (Profiles ?? []).Where(p => p is not null).Take(16).Select(p => p.Normalize()).ToList();
        if (profiles.Count == 0) profiles = DefaultProfiles();
        var ids = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < profiles.Count; i++)
        {
            if (!ids.Add(profiles[i].Id))
            {
                var id = $"profile-{i}-{Guid.NewGuid():N}";
                profiles[i] = profiles[i] with { Id = id };
                ids.Add(id);
            }
        }
        return this with
        {
            Version = CurrentVersion, Profiles = profiles,
            ActiveProfileId = profiles.Any(p => p.Id == ActiveProfileId) ? ActiveProfileId : profiles[0].Id,
        };
    }

    public static List<HudProfile> DefaultProfiles() =>
    [
        new(),
        new() { Id = "detailed", Name = "Подробный", Layout = HudLayout.Panel, ShowLows = true, Scale = 1.1f },
        new() { Id = "minimal", Name = "Минимальный", ShowCpu = false, ShowGpu = false, ShowRam = false,
            ShowVram = false, ShowGraph = false, Opacity = .75f },
    ];
}
