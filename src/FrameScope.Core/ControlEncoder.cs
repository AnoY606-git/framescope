using System.Buffers.Binary;

namespace FrameScope.Core;

public static class ControlEncoder
{
    public const int Size = 128;

    public static void Write(Span<byte> destination, in HudControl control)
    {
        if (destination.Length < Size) throw new ArgumentException("Control buffer must contain 128 bytes.", nameof(destination));
        BinaryPrimitives.WriteUInt32LittleEndian(destination, (uint)control.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[4..], control.Corner);
        Single(destination, 8, control.Scale); Single(destination, 12, control.Opacity);
        Single(destination, 16, control.MarginX); Single(destination, 20, control.MarginY);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[24..], control.TextColor);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[28..], control.AccentColor);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[32..], control.BackgroundColor);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[36..], (uint)control.AvailableMask);
        Double(destination, 40, control.CpuUsage); Double(destination, 48, control.CpuMax);
        Double(destination, 56, control.GpuUsage); Double(destination, 64, control.RamUsedGiB);
        Double(destination, 72, control.RamTotalGiB); Double(destination, 80, control.VramUsedGiB);
        Double(destination, 88, control.VramTotalGiB); Double(destination, 96, control.GpuTemperature);
        Double(destination, 104, control.CpuTemperature); Double(destination, 112, control.GpuPowerWatts);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[120..], control.UpdatedQpc);
    }

    private static void Single(Span<byte> target, int offset, float value) => BinaryPrimitives.WriteSingleLittleEndian(target[offset..], value);
    private static void Double(Span<byte> target, int offset, double value) => BinaryPrimitives.WriteDoubleLittleEndian(target[offset..], value);
}
