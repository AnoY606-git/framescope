using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace FrameScope.App.Controls;

public sealed class FrametimeGraph : FrameworkElement
{
    public static readonly DependencyProperty SamplesProperty = DependencyProperty.Register(nameof(Samples), typeof(IReadOnlyList<float>), typeof(FrametimeGraph),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public IReadOnlyList<float>? Samples { get => (IReadOnlyList<float>?)GetValue(SamplesProperty); set => SetValue(SamplesProperty, value); }
    private static readonly Brush Accent = FrozenBrush(0x5E, 0xEA, 0xD4);
    private static readonly Brush Muted = FrozenBrush(0x85, 0x96, 0xB0);
    private static readonly Pen GridPen = new(FrozenBrush(0x25, 0x33, 0x49), 1);

    protected override void OnRender(DrawingContext drawing)
    {
        base.OnRender(drawing);
        var width = ActualWidth; var height = ActualHeight;
        if (width < 70 || height < 35) return;
        var samples = Samples;
        var maximum = Math.Max(33.334, samples is { Count: > 0 } ? samples.Max() * 1.1 : 0);
        var chartWidth = width - 60; var chartHeight = height - 16;
        for (var row = 0; row < 3; row++)
        {
            var y = 8 + chartHeight * row / 2;
            drawing.DrawLine(GridPen, new(0, y), new(chartWidth, y));
            Text(drawing, (maximum * (2 - row) / 2).ToString("0.0", CultureInfo.CurrentCulture) + " ms", chartWidth + 8, Math.Max(0, y - 8));
        }
        if (samples is not { Count: > 0 }) { Text(drawing, "Нет свежих нативных кадров — график не синтезируется", 12, height / 2 - 8); return; }
        var path = new StreamGeometry();
        using (var context = path.Open())
        {
            for (var i = 0; i < samples.Count; i++)
            {
                var point = new Point(i * chartWidth / Math.Max(1, samples.Count - 1), 8 + chartHeight * (1 - Math.Clamp(samples[i] / maximum, 0, 1)));
                if (i == 0) context.BeginFigure(point, false, false); else context.LineTo(point, true, false);
            }
        }
        path.Freeze(); drawing.DrawGeometry(null, new Pen(Accent, 2), path);
    }

    private void Text(DrawingContext drawing, string text, double x, double y) => drawing.DrawText(
        new FormattedText(text, CultureInfo.CurrentCulture, FlowDirection.LeftToRight, new Typeface("Segoe UI"), 11, Muted,
            VisualTreeHelper.GetDpi(this).PixelsPerDip), new(x, y));
    private static Brush FrozenBrush(byte r, byte g, byte b) { var brush = new SolidColorBrush(Color.FromRgb(r, g, b)); brush.Freeze(); return brush; }
}
