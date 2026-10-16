using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace FrameScope.WindowsSensors;

public sealed record WddmGpuReading(string AdapterLuid, double? TemperatureCelsius, ulong? MemoryFrequencyHz,
    ulong? DedicatedVideoMemoryBytes = null, uint? PciBus = null, uint? PciDevice = null, uint? PciFunction = null);

/// <summary>Read-only WDDM telemetry from the installed Windows display driver.</summary>
public static class WddmGpuSensor
{
    private const int AdapterPerformanceData = 62;
    private const int MaximumAdapters = 64;

    [SupportedOSPlatform("windows10.0.17134")]
    public static IReadOnlyList<WddmGpuReading> ReadAll()
    {
        var readings = new List<WddmGpuReading>();
        if (!Environment.Is64BitProcess || Marshal.SizeOf<AdapterInfo>() != 20 ||
            Marshal.SizeOf<AdapterPerformance>() != 64 || Marshal.SizeOf<QueryAdapter>() != 24)
        {
            return readings;
        }
        int stride = Marshal.SizeOf<AdapterInfo>();
        nint buffer = Marshal.AllocHGlobal(stride * MaximumAdapters);
        try
        {
            // Нулевые handles позволяют безопасно освободить только действительно открытые адаптеры.
            Marshal.Copy(new byte[stride * MaximumAdapters], 0, buffer, stride * MaximumAdapters);
            var adapters = new EnumAdapters { Count = MaximumAdapters, Adapters = buffer };
            int status = D3DKMTEnumAdapters2(ref adapters);
            int count = (int)Math.Min(adapters.Count, MaximumAdapters);
            for (int index = 0; index < count; index++)
            {
                AdapterInfo adapter = Marshal.PtrToStructure<AdapterInfo>(buffer + index * stride);
                if (adapter.Handle == 0)
                {
                    continue;
                }
                try
                {
                    if (status < 0)
                    {
                        continue;
                    }
                    AdapterPerformance performance = Query<AdapterPerformance>(adapter.Handle, AdapterPerformanceData, out int queryStatus);
                    SegmentSize segments = Query<SegmentSize>(adapter.Handle, 3, out int segmentStatus);
                    AdapterAddress address = Query<AdapterAddress>(adapter.Handle, 6, out int addressStatus);
                    // Ноль от драйвера не считается доказательством температуры 0 C.
                    double? temperature = queryStatus >= 0 ? DecodeTemperature(performance.Temperature) : null;
                    ulong? memoryClock = queryStatus < 0 || performance.MemoryFrequency == 0 ? null : performance.MemoryFrequency;
                    ulong? capacity = segmentStatus >= 0 && segments.DedicatedVideoMemory is > 0 and < (1UL << 50)
                        ? segments.DedicatedVideoMemory : null;
                    bool validAddress = addressStatus >= 0 && address.Bus <= 255 && address.Device <= 31 && address.Function <= 7;
                    string luid = $"{unchecked((uint)adapter.HighPart):x8}:{adapter.LowPart:x8}";
                    readings.Add(new WddmGpuReading(luid, temperature, memoryClock, capacity,
                        validAddress ? address.Bus : null, validAddress ? address.Device : null, validAddress ? address.Function : null));
                    // performance.Power — 0.1% по спецификации, не W; намеренно не экспортируется как мощность.
                }
                finally
                {
                    var close = new CloseAdapter { Handle = adapter.Handle };
                    _ = D3DKMTCloseAdapter(ref close);
                }
            }
        }
        catch (DllNotFoundException)
        {
            return readings;
        }
        catch (EntryPointNotFoundException)
        {
            return readings;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
        return readings;
    }

    public static double? DecodeTemperature(uint deciCelsius) =>
        deciCelsius is > 0 and <= 2000 ? deciCelsius / 10.0 : null;

    private static T Query<T>(uint handle, int type, out int status) where T : struct
    {
        int size = Marshal.SizeOf<T>();
        nint data = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(default(T), data, false);
            var query = new QueryAdapter
            {
                Handle = handle,
                Type = type,
                Data = data,
                Size = (uint)size,
            };
            status = D3DKMTQueryAdapterInfo(ref query);
            return status >= 0 ? Marshal.PtrToStructure<T>(data) : default;
        }
        finally
        {
            Marshal.FreeHGlobal(data);
        }
    }

    // Public Windows ABI, Pack=8 and native pointer size; no Intel SDK headers are included.
    [StructLayout(LayoutKind.Sequential)]
    private struct AdapterInfo
    {
        public uint Handle;
        public uint LowPart;
        public int HighPart;
        public uint Sources;
        public int PreciseRegions;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SegmentSize
    {
        public ulong DedicatedVideoMemory;
        public ulong DedicatedSystemMemory;
        public ulong SharedSystemMemory;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AdapterAddress
    {
        public uint Bus;
        public uint Device;
        public uint Function;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct EnumAdapters
    {
        public uint Count;
        public nint Adapters;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CloseAdapter
    {
        public uint Handle;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct QueryAdapter
    {
        public uint Handle;
        public int Type;
        public nint Data;
        public uint Size;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct AdapterPerformance
    {
        public uint PhysicalAdapterIndex;
        public ulong MemoryFrequency;
        public ulong MaxMemoryFrequency;
        public ulong MaxMemoryFrequencyOc;
        public ulong MemoryBandwidth;
        public ulong PcieBandwidth;
        public uint FanRpm;
        public uint Power;
        public uint Temperature;
        public byte PowerStateOverride;
    }

    [DllImport("gdi32.dll", ExactSpelling = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern int D3DKMTEnumAdapters2(ref EnumAdapters adapters);

    [DllImport("gdi32.dll", ExactSpelling = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern int D3DKMTQueryAdapterInfo(ref QueryAdapter query);

    [DllImport("gdi32.dll", ExactSpelling = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern int D3DKMTCloseAdapter(ref CloseAdapter adapter);
}
