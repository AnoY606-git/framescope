using System.Buffers.Binary;

namespace FrameScope.Core;

public enum GraphicsApi : uint { Unknown, Dx11, Dx12, Vulkan }
[Flags]
public enum FrameFlags : uint { HasFps = 1, HasLow1 = 2, HasLow01 = 4 }
public enum FrameReadStatus { Ready, Missing, Busy, Stale, Invalid }

public sealed record FrameSnapshot(
    ulong PresentCount, ulong TimestampQpc, double? Fps, double FrametimeMs,
    double? Low1, double? Low01, double HudCpuMicroseconds, GraphicsApi Api,
    uint Width, uint Height, uint ProcessId, IReadOnlyList<float> Graph);

public static class FrameSnapshotParser
{
    public static FrameReadStatus Parse(ReadOnlySpan<byte> data, ulong nowQpc, ulong frequency,
        out FrameSnapshot? snapshot, double maximumAgeSeconds = 2)
    {
        snapshot = null;
        if (data.Length < NativeProtocol.FrameSize || frequency == 0 || !double.IsFinite(maximumAgeSeconds) || maximumAgeSeconds <= 0)
            return FrameReadStatus.Invalid;
        var presentCount = U64(data, 0);
        var timestamp = U64(data, 8);
        if (presentCount == 0 || timestamp == 0) return FrameReadStatus.Missing;
        // Не принимаем будущие/просроченные кадры, даже если seqlock оказался согласованным.
        if (timestamp > nowQpc && (timestamp - nowQpc) / (double)frequency > .1) return FrameReadStatus.Invalid;
        if (nowQpc >= timestamp && (nowQpc - timestamp) / (double)frequency > maximumAgeSeconds) return FrameReadStatus.Stale;
        var count = U32(data, 64);
        var flags = (FrameFlags)U32(data, 60);
        var api = U32(data, 56);
        if (count > NativeProtocol.GraphCapacity || api > (uint)GraphicsApi.Vulkan
            || (flags & ~(FrameFlags.HasFps | FrameFlags.HasLow1 | FrameFlags.HasLow01)) != 0)
            return FrameReadStatus.Invalid;
        var frametime = F64(data, 24);
        var hudCpu = F64(data, 48);
        if (!Nonnegative(frametime) || !Nonnegative(hudCpu)) return FrameReadStatus.Invalid;
        var fps = ReadOptional(data, 16, flags.HasFlag(FrameFlags.HasFps));
        var low1 = ReadOptional(data, 32, flags.HasFlag(FrameFlags.HasLow1));
        var low01 = ReadOptional(data, 40, flags.HasFlag(FrameFlags.HasLow01));
        if (fps is double f && !Nonnegative(f) || low1 is double l && !Nonnegative(l) || low01 is double ll && !Nonnegative(ll))
            return FrameReadStatus.Invalid;
        var graph = new float[(int)count];
        for (var i = 0; i < graph.Length; i++)
        {
            var value = BinaryPrimitives.ReadSingleLittleEndian(data[(80 + i * 4)..]);
            if (!float.IsFinite(value) || value < 0) return FrameReadStatus.Invalid;
            graph[i] = value;
        }
        snapshot = new(presentCount, timestamp, fps, frametime, low1, low01, hudCpu, (GraphicsApi)api,
            U32(data, 68), U32(data, 72), U32(data, 76), Array.AsReadOnly(graph));
        return FrameReadStatus.Ready;
    }

    private static bool Nonnegative(double value) => double.IsFinite(value) && value >= 0;
    private static double? ReadOptional(ReadOnlySpan<byte> data, int offset, bool available) => available ? F64(data, offset) : null;
    private static uint U32(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(data[offset..]);
    private static ulong U64(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadUInt64LittleEndian(data[offset..]);
    private static double F64(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadDoubleLittleEndian(data[offset..]);
}

public interface IFrameTransport
{
    ulong ReadSequence();
    void CopyFrame(byte[] destination);
}

public static class SeqlockFrameReader
{
    public const int MaximumAttempts = 8;

    public static FrameReadStatus Read(IFrameTransport source, ulong nowQpc, ulong frequency, out FrameSnapshot? snapshot)
    {
        ArgumentNullException.ThrowIfNull(source);
        snapshot = null;
        var bytes = new byte[NativeProtocol.FrameSize];
        for (var attempt = 0; attempt < MaximumAttempts; attempt++)
        {
            var before = source.ReadSequence();
            if (before == 0) return FrameReadStatus.Missing;
            if ((before & 1) != 0) { Thread.SpinWait(4); continue; }
            Thread.MemoryBarrier();
            source.CopyFrame(bytes);
            Thread.MemoryBarrier();
            var after = source.ReadSequence();
            if (before == after && (after & 1) == 0)
                return FrameSnapshotParser.Parse(bytes, nowQpc, frequency, out snapshot);
        }
        return FrameReadStatus.Busy;
    }
}
