namespace FrameScope.Core;

public readonly record struct ProcessorTimes(ulong Idle, ulong Kernel, ulong User);
public readonly record struct CpuUsage(double? Total, double? MaximumLogicalProcessor);

public sealed class CpuUsageCalculator
{
    private ProcessorTimes? _previousTotal;
    private ProcessorTimes[]? _previousProcessors;

    public CpuUsage Sample(ProcessorTimes total, IReadOnlyList<ProcessorTimes> processors)
    {
        var usage = _previousTotal is { } previous ? Calculate(previous, total) : null;
        double? maximum = null;
        if (_previousProcessors is { } old && old.Length == processors.Count)
        {
            for (var i = 0; i < old.Length; i++)
            {
                var value = Calculate(old[i], processors[i]);
                if (value is not null) maximum = Math.Max(maximum ?? 0, value.Value);
            }
        }
        _previousTotal = total;
        _previousProcessors = processors.ToArray();
        return new(usage, maximum);
    }

    public void Reset() { _previousTotal = null; _previousProcessors = null; }

    private static double? Calculate(ProcessorTimes previous, ProcessorTimes current)
    {
        if (current.Kernel < previous.Kernel || current.User < previous.User || current.Idle < previous.Idle) return null;
        var kernel = current.Kernel - previous.Kernel;
        var user = current.User - previous.User;
        var elapsed = (double)kernel + user;
        if (elapsed <= 0) return null;
        var idle = current.Idle - previous.Idle;
        if (idle > elapsed) return null;
        return Math.Clamp((elapsed - idle) * 100 / elapsed, 0, 100);
    }
}
