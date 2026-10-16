using System.Buffers.Binary;

namespace FrameScope.Core;

public static class IntelTelemetry
{
    public const double MaximumAgeSeconds = 5;
    public static IntelControl? Validate(IntelControl? value, ulong nowQpc, ulong frequency)
    {
        if (value is not { } data || frequency == 0 || data.Magic != NativeProtocol.IntelMagic || data.Version != 1
            || data.UpdatedQpc == 0 || data.UpdatedQpc > nowQpc
            || (nowQpc - data.UpdatedQpc) / (double)frequency > MaximumAgeSeconds) return null;
        const IntelAvailability known = IntelAvailability.GpuClock | IntelAvailability.MemoryClock | IntelAvailability.GpuPower
            | IntelAvailability.BoardPower | IntelAvailability.Fan | IntelAvailability.Voltage | IntelAvailability.MemoryRead
            | IntelAvailability.MemoryWrite | IntelAvailability.MemoryTemperature | IntelAvailability.Throttle;
        data.AvailableMask &= known;
        Sanitize(ref data, IntelAvailability.GpuClock, ref data.GpuFrequencyMhz);
        Sanitize(ref data, IntelAvailability.MemoryClock, ref data.MemoryFrequencyMhz);
        Sanitize(ref data, IntelAvailability.GpuPower, ref data.GpuPowerWatts);
        Sanitize(ref data, IntelAvailability.BoardPower, ref data.BoardPowerWatts);
        Sanitize(ref data, IntelAvailability.Fan, ref data.FanRpm);
        Sanitize(ref data, IntelAvailability.Voltage, ref data.VoltageVolts);
        Sanitize(ref data, IntelAvailability.MemoryRead, ref data.MemoryReadGbps);
        Sanitize(ref data, IntelAvailability.MemoryWrite, ref data.MemoryWriteGbps);
        Sanitize(ref data, IntelAvailability.MemoryTemperature, ref data.MemoryTemperature);
        if (!data.AvailableMask.HasFlag(IntelAvailability.Throttle)) data.ThrottleFlags = 0;
        data.Reserved1 = data.Reserved2 = data.Reserved3 = 0;
        return data;
    }

    private static void Sanitize(ref IntelControl data, IntelAvailability availability, ref double value)
    {
        if (!double.IsFinite(value) || value < 0) data.AvailableMask &= ~availability;
        if (!data.AvailableMask.HasFlag(availability)) value = 0;
    }
}

public static class IntelControlEncoder
{
    public const int Size = 128;
    public static void Write(Span<byte> destination, in IntelControl control)
    {
        if (destination.Length < Size) throw new ArgumentException("Intel buffer must contain 128 bytes.", nameof(destination));
        BinaryPrimitives.WriteUInt32LittleEndian(destination, control.Magic);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[4..], control.Version);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[8..], (ulong)control.AvailableMask);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[16..], control.UpdatedQpc);
        Double(destination, 24, control.GpuFrequencyMhz); Double(destination, 32, control.MemoryFrequencyMhz);
        Double(destination, 40, control.GpuPowerWatts); Double(destination, 48, control.BoardPowerWatts);
        Double(destination, 56, control.FanRpm); Double(destination, 64, control.VoltageVolts);
        Double(destination, 72, control.MemoryReadGbps); Double(destination, 80, control.MemoryWriteGbps);
        Double(destination, 88, control.MemoryTemperature);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[96..], control.ThrottleFlags);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[104..], control.Reserved1);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[112..], control.Reserved2);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[120..], control.Reserved3);
    }
    private static void Double(Span<byte> target, int offset, double value) => BinaryPrimitives.WriteDoubleLittleEndian(target[offset..], value);
}
