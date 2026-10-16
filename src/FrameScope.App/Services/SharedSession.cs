using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using FrameScope.Core;

namespace FrameScope.App.Services;

internal sealed class SharedSession : IFrameTransport, IDisposable
{
    private readonly MemoryMappedFile _mapping;
    private readonly MemoryMappedViewAccessor _view;
    private readonly byte[] _control = new byte[ControlEncoder.Size];
    private readonly byte[] _intel = new byte[IntelControlEncoder.Size];
    private readonly object _gate = new();
    private ulong _sequence;
    private ulong _intelSequence;
    private bool _disposed;
    public string Id { get; } = Guid.NewGuid().ToString("N");

    public SharedSession()
    {
        // Local + случайный идентификатор + стандартный DACL текущего пользователя; без повышения прав.
        _mapping = MemoryMappedFile.CreateNew(NativeProtocol.MappingName(Id), NativeProtocol.MappingSize, MemoryMappedFileAccess.ReadWrite);
        try
        {
            _view = _mapping.CreateViewAccessor(0, NativeProtocol.MappingSize, MemoryMappedFileAccess.ReadWrite);
            _view.Write(0, NativeProtocol.Magic);
            _view.Write(4, NativeProtocol.Version);
            _view.Write(8, (uint)NativeProtocol.MappingSize);
            _view.Write(16, (ulong)Stopwatch.Frequency);
        }
        catch { _mapping.Dispose(); throw; }
    }

    public void WriteControl(HudProfile profile, MetricSample metrics)
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var control = metrics.ToControl(profile);
            ControlEncoder.Write(_control, control);
            _view.Write(NativeProtocol.ControlSequenceOffset, ++_sequence);
            Thread.MemoryBarrier();
            _view.WriteArray(NativeProtocol.ControlOffset, _control, 0, _control.Length);
            Thread.MemoryBarrier();
            _view.Write(NativeProtocol.ControlSequenceOffset, ++_sequence);
        }
    }

    public FrameReadStatus ReadFrame(out FrameSnapshot? snapshot)
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return SeqlockFrameReader.Read(this, (ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency, out snapshot);
        }
    }

    public void WriteIntel(IntelControl? control)
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var validated = IntelTelemetry.Validate(control, (ulong)Stopwatch.GetTimestamp(), (ulong)Stopwatch.Frequency);
            if (validated is { } data) IntelControlEncoder.Write(_intel, data);
            else Array.Clear(_intel); // Исчезнувший/устаревший поставщик очищает предыдущий блок, а не освежает его.
            _view.Write(NativeProtocol.IntelSequenceOffset, ++_intelSequence);
            Thread.MemoryBarrier();
            _view.WriteArray(NativeProtocol.IntelOffset, _intel, 0, _intel.Length);
            Thread.MemoryBarrier();
            _view.Write(NativeProtocol.IntelSequenceOffset, ++_intelSequence);
        }
    }

    public ulong ReadSequence() => _view.ReadUInt64(NativeProtocol.FrameSequenceOffset);
    public void CopyFrame(byte[] destination) => _view.ReadArray(NativeProtocol.FrameOffset, destination, 0, NativeProtocol.FrameSize);

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            _view.Dispose(); _mapping.Dispose();
        }
    }
}
