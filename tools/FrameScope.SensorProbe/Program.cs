using System.Text.Json;
using FrameScope.WindowsSensors;

if (WddmGpuSensor.DecodeTemperature(570) != 57.0 ||
    WddmGpuSensor.DecodeTemperature(0) is not null ||
    WddmGpuSensor.DecodeTemperature(uint.MaxValue) is not null)
{
    throw new InvalidOperationException("WDDM units/availability contract failed.");
}
if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 17134))
{
    throw new PlatformNotSupportedException("WDDM temperature telemetry requires Windows 10 1803 or newer.");
}
Console.WriteLine(JsonSerializer.Serialize(WddmGpuSensor.ReadAll(), new JsonSerializerOptions { WriteIndented = true }));
