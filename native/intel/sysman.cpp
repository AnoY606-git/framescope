// SPDX-License-Identifier: GPL-3.0-only
#include "sysman.hpp"

#include <windows.h>
#include <winver.h>
#include <ze_api.h>
#include <zes_api.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace framescope::intel {
namespace {

static_assert(sizeof(void*) == 8, "FrameScope Intel Sysman requires Windows x64");

// Только read-only API: в таблице намеренно нет setter/reset/overclock функций.
#define FRAMESCOPE_SYSMAN_FUNCTIONS(X) \
    X(zesInit) X(zesDriverGet) X(zesDeviceGet) X(zesDeviceGetProperties) \
    X(zesDevicePciGetProperties) X(zesDeviceEnumTemperatureSensors) \
    X(zesTemperatureGetProperties) X(zesTemperatureGetState) \
    X(zesDeviceEnumFrequencyDomains) X(zesFrequencyGetProperties) X(zesFrequencyGetState) \
    X(zesDeviceEnumPowerDomains) X(zesDeviceGetCardPowerDomain) \
    X(zesPowerGetProperties) X(zesPowerGetEnergyCounter) \
    X(zesDeviceEnumMemoryModules) X(zesMemoryGetProperties) X(zesMemoryGetState) \
    X(zesMemoryGetBandwidth) X(zesDeviceEnumEngineGroups) X(zesEngineGetProperties) \
    X(zesEngineGetActivity) \
    X(zeInit) X(zeDriverGet) X(zeDeviceGet) X(zeDeviceGetProperties)

struct Api {
#define DECLARE_FUNCTION(name) decltype(&::name) name = nullptr;
    FRAMESCOPE_SYSMAN_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION
};

std::string resultName(ze_result_t result) {
    switch (result) {
    case ZE_RESULT_SUCCESS: return "ZE_RESULT_SUCCESS";
    case ZE_RESULT_ERROR_UNINITIALIZED: return "ZE_RESULT_ERROR_UNINITIALIZED";
    case ZE_RESULT_ERROR_UNSUPPORTED_FEATURE: return "ZE_RESULT_ERROR_UNSUPPORTED_FEATURE";
    case ZE_RESULT_ERROR_UNSUPPORTED_VERSION: return "ZE_RESULT_ERROR_UNSUPPORTED_VERSION";
    case ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION: return "ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION";
    case ZE_RESULT_ERROR_DEPENDENCY_UNAVAILABLE: return "ZE_RESULT_ERROR_DEPENDENCY_UNAVAILABLE";
    case ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS: return "ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS";
    case ZE_RESULT_ERROR_NOT_AVAILABLE: return "ZE_RESULT_ERROR_NOT_AVAILABLE";
    case ZE_RESULT_ERROR_DEVICE_LOST: return "ZE_RESULT_ERROR_DEVICE_LOST";
    case ZE_RESULT_ERROR_DEVICE_REQUIRES_RESET: return "ZE_RESULT_ERROR_DEVICE_REQUIRES_RESET";
    case ZE_RESULT_ERROR_DEVICE_IN_LOW_POWER_STATE: return "ZE_RESULT_ERROR_DEVICE_IN_LOW_POWER_STATE";
    case ZE_RESULT_ERROR_INVALID_SIZE: return "ZE_RESULT_ERROR_INVALID_SIZE";
    case ZE_RESULT_ERROR_INVALID_ARGUMENT: return "ZE_RESULT_ERROR_INVALID_ARGUMENT";
    case ZE_RESULT_ERROR_INVALID_NULL_HANDLE: return "ZE_RESULT_ERROR_INVALID_NULL_HANDLE";
    case ZE_RESULT_ERROR_INVALID_NULL_POINTER: return "ZE_RESULT_ERROR_INVALID_NULL_POINTER";
    default: return "ZE_RESULT_ERROR";
    }
}

Availability outcome(ze_result_t result) {
    if (result == ZE_RESULT_SUCCESS) return {"available", {}, {}};
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(result));
    std::string status = "unavailable";
    if (result == ZE_RESULT_ERROR_UNSUPPORTED_FEATURE ||
        result == ZE_RESULT_ERROR_UNSUPPORTED_VERSION ||
        result == ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION) status = "unsupported";
    if (result == ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS) status = "access_denied";
    return {status, resultName(result) + ":" + code, {}};
}

Availability missing(std::string_view function) {
    return {"unsupported", "missing_export:" + std::string(function), {}};
}

std::string win32Reason(std::string_view operation) {
    return std::string(operation) + ":win32:" + std::to_string(GetLastError());
}

std::string fileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0 || size > 1024 * 1024) return {};
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return {};
    VS_FIXEDFILEINFO* version = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&version), &length) ||
        length < sizeof(VS_FIXEDFILEINFO) || version->dwSignature != 0xfeef04bd) return {};
    return std::to_string(HIWORD(version->dwFileVersionMS)) + "." +
           std::to_string(LOWORD(version->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(version->dwFileVersionLS)) + "." +
           std::to_string(LOWORD(version->dwFileVersionLS));
}

template <std::size_t N> std::string text(const char (&value)[N]) {
    const auto end = std::find(value, value + N, '\0');
    const auto length = static_cast<int>(end - value);
    if (length == 0) return {};
    // Не пропускаем некорректный UTF-8 из драйвера в JSON.
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, length, nullptr, 0) == 0)
        return "invalid_utf8_driver_string";
    return {value, end};
}

template <class Handle> struct Enumeration {
    Availability availability;
    std::vector<Handle> handles;
};

template <class Handle, class Function>
Enumeration<Handle> enumerate(std::string_view name, bool present, Function function) {
    if (!present) return {missing(name), {}};
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::uint32_t count = 0;
        auto result = function(&count, nullptr);
        if (result != ZE_RESULT_SUCCESS) return {outcome(result), {}};
        if (count == 0) return {{"unsupported", "no_domains_exposed", 0}, {}};
        if (count > 256) return {{"invalid_data", "enumeration_count_exceeds_limit", {}}, {}};
        std::vector<Handle> handles(count);
        const auto capacity = count;
        result = function(&count, handles.data());
        if (result == ZE_RESULT_ERROR_INVALID_SIZE || count > capacity) continue;
        if (result != ZE_RESULT_SUCCESS) return {outcome(result), {}};
        handles.resize(count);
        if (std::any_of(handles.begin(), handles.end(), [](auto handle) { return !handle; }))
            return {{"invalid_data", "null_handle_in_enumeration", {}}, {}};
        return {{"available", {}, count}, std::move(handles)};
    }
    return {{"unavailable", "enumeration_changed_during_query", {}}, {}};
}

template <class Handle, class Properties> struct Domain {
    Handle handle{};
    Properties properties{};
    Availability propertyStatus;
};

struct Power : Domain<zes_pwr_handle_t, zes_power_properties_t> {
    zes_power_domain_t domain = ZES_POWER_DOMAIN_UNKNOWN;
    CounterTracker counter;
};
struct Memory : Domain<zes_mem_handle_t, zes_mem_properties_t> {
    ByteCounterTracker readCounter;
    ByteCounterTracker writeCounter;
};
struct Engine : Domain<zes_engine_handle_t, zes_engine_properties_t> {
    CounterTracker counter;
};
using Temperature = Domain<zes_temp_handle_t, zes_temp_properties_t>;
using Frequency = Domain<zes_freq_handle_t, zes_freq_properties_t>;

struct Device {
    zes_device_handle_t handle{};
    DeviceSnapshot identity;
    std::vector<Temperature> temperatures;
    std::vector<Frequency> frequencies;
    std::vector<Power> powers;
    std::vector<Memory> memories;
    std::vector<Engine> engines;
};

std::string temperatureScope(zes_temp_sensors_t type) {
    switch (type) {
    case ZES_TEMP_SENSORS_GLOBAL: return "global_max";
    case ZES_TEMP_SENSORS_GPU: return "gpu_max";
    case ZES_TEMP_SENSORS_MEMORY: return "memory_max";
    case ZES_TEMP_SENSORS_GLOBAL_MIN: return "global_min";
    case ZES_TEMP_SENSORS_GPU_MIN: return "gpu_min";
    case ZES_TEMP_SENSORS_MEMORY_MIN: return "memory_min";
    case ZES_TEMP_SENSORS_GPU_BOARD: return "board_max";
    case ZES_TEMP_SENSORS_GPU_BOARD_MIN: return "board_min";
    case ZES_TEMP_SENSORS_VOLTAGE_REGULATOR: return "voltage_regulator_max";
    case ZES_TEMP_SENSORS_COMPOSITE: return "composite_normalized";
    case ZES_TEMP_SENSORS_GPU_BOARD_SINGLE: return "board_single";
    case ZES_TEMP_SENSORS_VOLTAGE_REGULATOR_SINGLE: return "voltage_regulator_single";
    default: return "unknown";
    }
}

std::string frequencyScope(zes_freq_domain_t type) {
    switch (type) {
    case ZES_FREQ_DOMAIN_GPU: return "gpu";
    case ZES_FREQ_DOMAIN_MEMORY: return "memory";
    case ZES_FREQ_DOMAIN_MEDIA: return "media";
    default: return "unknown";
    }
}

std::string powerScope(zes_power_domain_t domain) {
    switch (domain) {
    case ZES_POWER_DOMAIN_CARD: return "card";
    case ZES_POWER_DOMAIN_PACKAGE: return "package";
    case ZES_POWER_DOMAIN_STACK: return "stack";
    case ZES_POWER_DOMAIN_MEMORY: return "memory";
    case ZES_POWER_DOMAIN_GPU: return "gpu";
    default: return "unknown";
    }
}

std::string engineScope(zes_engine_group_t type) {
    switch (type) {
    case ZES_ENGINE_GROUP_ALL: return "all";
    case ZES_ENGINE_GROUP_COMPUTE_ALL: return "compute_all";
    case ZES_ENGINE_GROUP_RENDER_ALL: return "render_all";
    case ZES_ENGINE_GROUP_MEDIA_ALL: return "media_all";
    case ZES_ENGINE_GROUP_COPY_ALL: return "copy_all";
    case ZES_ENGINE_GROUP_3D_RENDER_COMPUTE_ALL: return "render_compute_all_legacy";
    default: return "single_engine_not_aggregated";
    }
}

Reading reading(std::string key, std::string scope, std::string instance, std::string unit,
                std::string api, Availability availability = {"available", {}, {}}) {
    Reading value;
    value.key = std::move(key); value.scope = std::move(scope);
    value.instance = std::move(instance); value.unit = std::move(unit);
    value.api = std::move(api); value.availability = std::move(availability);
    value.derived = value.key == "power.average" || value.key == "memory.used_bytes" ||
                    value.key == "memory.read_bandwidth" || value.key == "memory.write_bandwidth" ||
                    value.key == "engine.utilization";
    return value;
}

void measured(Reading& reading, double value, bool negativeIsUnknown = true) {
    if (reading.availability.status != "available") return;
    if (!std::isfinite(value)) {
        reading.availability = {"invalid_data", "driver_returned_nonfinite_value", {}};
    } else if (negativeIsUnknown && value < 0) {
        reading.availability = {"unsupported", "driver_reported_unknown_value", {}};
    } else reading.value = value;
}

template <class Properties> void locate(Reading& reading, const Properties& properties) {
    if (properties.onSubdevice) reading.subdeviceId = properties.subdeviceId;
}

void applyCounter(Reading& reading, const CounterResult& result, bool microsecondTimestamp) {
    reading.derived = true;
    reading.availability = result.availability;
    reading.value = result.value;
    if (microsecondTimestamp && result.timestampDelta != 0)
        reading.windowMs = static_cast<double>(result.timestampDelta) / 1000.0;
}

Availability absentScope(const DeviceSnapshot& snapshot, const char* enumeration) {
    const auto it = snapshot.capabilities.find(enumeration);
    if (it != snapshot.capabilities.end() && it->second.status != "available") return it->second;
    return {"unsupported", "sensor_scope_not_exposed", {}};
}

void ensureReading(DeviceSnapshot& device, const char* key, const char* scope, const char* unit,
                   const char* api, const char* enumeration) {
    if (std::none_of(device.readings.begin(), device.readings.end(), [&](const auto& value) {
            return value.key == key && value.scope == scope;
        })) device.readings.push_back(reading(key, scope, "", unit, api, absentScope(device, enumeration)));
}

} // namespace

struct SysmanBackend::Impl {
    Api api;
    HMODULE module = nullptr;
    Snapshot metadata;
    std::vector<Device> devices;

    explicit Impl(bool legacy) { initialize(legacy); }
    ~Impl() { if (module) FreeLibrary(module); }

    void initialize(bool legacy) {
        metadata.mode = legacy ? "legacy" : "standalone";
        // Меняется только окружение дочернего CLI, не пользователя/системы/родителя.
        for (auto variable : {L"ZE_ENABLE_NULL_DRIVER", L"ZE_ENABLE_ALT_DRIVERS",
                              L"ZE_ENABLE_VALIDATION_LAYER", L"ZE_ENABLE_TRACING_LAYER",
                              L"ZEL_ENABLE_LOADER_LOGGING", L"ZEL_ENABLE_LOADER_DEBUG_TRACE",
                              L"ZE_AFFINITY_MASK", L"ZE_FLAT_DEVICE_HIERARCHY"}) {
            if (!SetEnvironmentVariableW(variable, nullptr) && GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
                metadata.backend = {"unavailable", win32Reason("sanitize_process_environment"), {}};
                return;
            }
        }
        if (!SetEnvironmentVariableW(L"ZES_ENABLE_SYSMAN", legacy ? L"1" : nullptr) &&
            GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
            metadata.backend = {"unavailable", win32Reason("set_process_sysman_mode"), {}};
            return;
        }
        // Не разрешаем loader-зависимостям искать DLL в каталоге EXE или CWD.
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            metadata.backend = {"unavailable", win32Reason("SetDefaultDllDirectories"), {}};
            return;
        }
        wchar_t systemDirectory[32768]{};
        const auto length = GetSystemDirectoryW(systemDirectory, 32768);
        if (length == 0 || length >= 32768) {
            metadata.backend = {"unavailable", win32Reason("GetSystemDirectoryW"), {}};
            return;
        }
        const std::wstring path = std::wstring(systemDirectory, length) + L"\\ze_loader.dll";
        metadata.loaderVersion = fileVersion(path);
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            metadata.backend = {"unavailable", win32Reason("LoadLibraryExW(ze_loader.dll)"), {}};
            return;
        }
        wchar_t loadedPath[32768]{};
        const auto loadedLength = GetModuleFileNameW(module, loadedPath, 32768);
        metadata.loaderLoadedFromSystem32 = loadedLength != 0 && loadedLength < 32768 &&
                                            _wcsicmp(path.c_str(), loadedPath) == 0;
        if (!metadata.loaderLoadedFromSystem32) {
            metadata.backend = {"unavailable", "loader_path_not_system32", {}};
            return;
        }
#define LOAD_FUNCTION(name) \
        api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(module, #name)); \
        metadata.exports[#name] = api.name != nullptr;
        FRAMESCOPE_SYSMAN_FUNCTIONS(LOAD_FUNCTION)
#undef LOAD_FUNCTION
        // Эти symbols проверяются только на наличие, callable pointers не сохраняются.
        for (const char* name : {"zesDeviceEnumFans", "zesFanGetProperties", "zesFanGetState"})
            metadata.exports[name] = GetProcAddress(module, name) != nullptr;
        // Режимы не смешиваются: для legacy retry после zesInit нужен новый процесс.
        if (!legacy && (!api.zesInit || !api.zesDriverGet || !api.zesDeviceGet)) {
            metadata.backend = {"unsupported", "standalone_exports_missing_retry_with_legacy", {}};
            return;
        }
        if (legacy && (!api.zeInit || !api.zeDriverGet || !api.zeDeviceGet)) {
            metadata.backend = {"unsupported", "legacy_exports_missing", {}};
            return;
        }
        const auto result = legacy ? api.zeInit(ZE_INIT_FLAG_GPU_ONLY) : api.zesInit(ZES_INIT_FLAG_GPU_ONLY);
        metadata.initialization[legacy ? "zeInit" : "zesInit"] = outcome(result);
        if (result != ZE_RESULT_SUCCESS) {
            metadata.backend = outcome(result);
            return;
        }
        auto driverGet = legacy ? api.zeDriverGet : api.zesDriverGet;
        auto deviceGet = legacy ? api.zeDeviceGet : api.zesDeviceGet;
        const char* driverApi = legacy ? "zeDriverGet" : "zesDriverGet";
        const char* deviceApi = legacy ? "zeDeviceGet" : "zesDeviceGet";
        auto drivers = enumerate<zes_driver_handle_t>(driverApi, true,
            [&](auto count, auto handles) { return driverGet(count, handles); });
        metadata.initialization[driverApi] = drivers.availability;
        if (drivers.availability.status != "available") {
            metadata.backend = drivers.availability;
            return;
        }
        std::set<zes_device_handle_t> seen;
        std::uint32_t driverIndex = 0;
        for (auto driver : drivers.handles) {
            auto list = enumerate<zes_device_handle_t>(deviceApi, true,
                [&](auto count, auto handles) { return deviceGet(driver, count, handles); });
            metadata.initialization[std::string(deviceApi) + ":" + std::to_string(driverIndex++)] =
                list.availability;
            for (auto handle : list.handles) {
                if (!seen.insert(handle).second) continue;
                Device device;
                device.handle = handle;
                if (identify(device, legacy)) {
                    discover(device);
                    devices.push_back(std::move(device));
                }
            }
        }
        metadata.backend = devices.empty() ? Availability{"unavailable", "no_identified_intel_gpu", 0} :
            Availability{"available", {}, static_cast<std::uint32_t>(devices.size())};
    }

    bool identify(Device& device, bool legacy) {
        zes_device_properties_t properties{};
        properties.stype = ZES_STRUCTURE_TYPE_DEVICE_PROPERTIES;
        properties.core.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
        auto result = api.zesDeviceGetProperties ? api.zesDeviceGetProperties(device.handle, &properties) :
            ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
        device.identity.capabilities["zesDeviceGetProperties"] = api.zesDeviceGetProperties ?
            outcome(result) : missing("zesDeviceGetProperties");
        if (result != ZE_RESULT_SUCCESS && legacy && api.zeDeviceGetProperties) {
            properties.core = {};
            properties.core.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
            result = api.zeDeviceGetProperties(device.handle, &properties.core);
            device.identity.capabilities["zeDeviceGetProperties"] = outcome(result);
        }
        if (result != ZE_RESULT_SUCCESS || properties.core.vendorId != 0x8086 ||
            properties.core.type != ZE_DEVICE_TYPE_GPU) {
            metadata.initialization["device_identity:" + std::to_string(metadata.initialization.size())] =
                result == ZE_RESULT_SUCCESS ? Availability{"unsupported", "not_an_intel_gpu", {}} : outcome(result);
            return false;
        }
        device.identity.name = text(properties.core.name);
        device.identity.vendorId = properties.core.vendorId;
        device.identity.deviceId = properties.core.deviceId;
        device.identity.driverVersion = text(properties.driverVersion);
        // UUID, serialNumber и boardNumber намеренно не сериализуются.
        zes_pci_properties_t pci{};
        pci.stype = ZES_STRUCTURE_TYPE_PCI_PROPERTIES;
        result = api.zesDevicePciGetProperties ? api.zesDevicePciGetProperties(device.handle, &pci) :
            ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
        device.identity.capabilities["zesDevicePciGetProperties"] = api.zesDevicePciGetProperties ?
            outcome(result) : missing("zesDevicePciGetProperties");
        if (result == ZE_RESULT_SUCCESS && pci.address.bus <= 255 && pci.address.device <= 31 &&
            pci.address.function <= 7) {
            device.identity.pci = PciAddress{pci.address.domain, pci.address.bus, pci.address.device, pci.address.function};
            char address[64]{};
            std::snprintf(address, sizeof(address), "pci:%04x:%02x:%02x.%x", pci.address.domain,
                          pci.address.bus, pci.address.device, pci.address.function);
            device.identity.id = address;
        } else device.identity.id = "session-device-" + std::to_string(devices.size());
        return true;
    }

    template <class DomainType, class Handle, class Function, class PropertyFunction>
    void discoverDomains(Device& device, const char* enumeration, Function function,
                         const char* getter, PropertyFunction propertyFunction,
                         zes_structure_type_t type, std::vector<DomainType>& output) {
        auto list = enumerate<Handle>(enumeration, function != nullptr,
            [&](auto count, auto handles) { return function(device.handle, count, handles); });
        device.identity.capabilities[enumeration] = list.availability;
        std::size_t index = 0;
        for (auto handle : list.handles) {
            DomainType domain;
            domain.handle = handle;
            domain.properties.stype = type;
            domain.propertyStatus = propertyFunction ? outcome(propertyFunction(handle, &domain.properties)) :
                missing(getter);
            // pNext никогда не хранит указатель на уже уничтоженную stack-переменную.
            domain.properties.pNext = nullptr;
            device.identity.capabilities[std::string(getter) + ":" + std::to_string(index++)] = domain.propertyStatus;
            output.push_back(std::move(domain));
        }
    }

    void discover(Device& device) {
        discoverDomains<Temperature, zes_temp_handle_t>(device, "zesDeviceEnumTemperatureSensors",
            api.zesDeviceEnumTemperatureSensors, "zesTemperatureGetProperties", api.zesTemperatureGetProperties,
            ZES_STRUCTURE_TYPE_TEMP_PROPERTIES, device.temperatures);
        discoverDomains<Frequency, zes_freq_handle_t>(device, "zesDeviceEnumFrequencyDomains",
            api.zesDeviceEnumFrequencyDomains, "zesFrequencyGetProperties", api.zesFrequencyGetProperties,
            ZES_STRUCTURE_TYPE_FREQ_PROPERTIES, device.frequencies);
        discoverDomains<Memory, zes_mem_handle_t>(device, "zesDeviceEnumMemoryModules",
            api.zesDeviceEnumMemoryModules, "zesMemoryGetProperties", api.zesMemoryGetProperties,
            ZES_STRUCTURE_TYPE_MEM_PROPERTIES, device.memories);
        discoverDomains<Engine, zes_engine_handle_t>(device, "zesDeviceEnumEngineGroups",
            api.zesDeviceEnumEngineGroups, "zesEngineGetProperties", api.zesEngineGetProperties,
            ZES_STRUCTURE_TYPE_ENGINE_PROPERTIES, device.engines);
        // В upstream Windows getProperties пишет CurrentNumOfControlPoints;
        // enum может вызвать этот getter при создании handles. Не вызываем даже enum.
        device.identity.capabilities["zesDeviceEnumFans"] =
            {"unsupported", "read_only_policy_windows_sysman_fan_getters_can_write", {}};
        device.identity.capabilities["zesFanGetProperties"] =
            {"unsupported", "read_only_policy_windows_sysman_fan_getters_can_write", {}};
        device.identity.capabilities["zesFanGetState"] =
            {"unsupported", "read_only_policy_windows_sysman_fan_getters_can_write", {}};

        auto powerList = enumerate<zes_pwr_handle_t>("zesDeviceEnumPowerDomains", api.zesDeviceEnumPowerDomains != nullptr,
            [&](auto count, auto handles) { return api.zesDeviceEnumPowerDomains(device.handle, count, handles); });
        device.identity.capabilities["zesDeviceEnumPowerDomains"] = powerList.availability;
        zes_pwr_handle_t card = nullptr;
        const auto cardResult = api.zesDeviceGetCardPowerDomain ?
            api.zesDeviceGetCardPowerDomain(device.handle, &card) : ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
        device.identity.capabilities["zesDeviceGetCardPowerDomain"] = api.zesDeviceGetCardPowerDomain ?
            outcome(cardResult) : missing("zesDeviceGetCardPowerDomain");
        if (cardResult != ZE_RESULT_SUCCESS) card = nullptr;
        if (card && std::find(powerList.handles.begin(), powerList.handles.end(), card) == powerList.handles.end())
            powerList.handles.push_back(card);
        std::size_t index = 0;
        for (auto handle : powerList.handles) {
            Power power;
            power.handle = handle;
            power.properties.stype = ZES_STRUCTURE_TYPE_POWER_PROPERTIES;
            zes_power_ext_properties_t extension{};
            extension.stype = ZES_STRUCTURE_TYPE_POWER_EXT_PROPERTIES;
            extension.domain = ZES_POWER_DOMAIN_UNKNOWN;
            power.properties.pNext = &extension;
            auto result = api.zesPowerGetProperties ? api.zesPowerGetProperties(handle, &power.properties) :
                ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
            if (result == ZE_RESULT_SUCCESS) power.domain = extension.domain;
            else if (api.zesPowerGetProperties) {
                power.properties = {};
                power.properties.stype = ZES_STRUCTURE_TYPE_POWER_PROPERTIES;
                result = api.zesPowerGetProperties(handle, &power.properties);
            }
            power.properties.pNext = nullptr;
            power.propertyStatus = api.zesPowerGetProperties ? outcome(result) : missing("zesPowerGetProperties");
            device.identity.capabilities["zesPowerGetProperties:" + std::to_string(index++)] = power.propertyStatus;
            if (card && handle == card) power.domain = ZES_POWER_DOMAIN_CARD;
            device.powers.push_back(std::move(power));
        }
    }

    void temperatures(Device& device, DeviceSnapshot& snapshot) {
        std::size_t index = 0;
        for (auto& domain : device.temperatures) {
            const auto scope = domain.propertyStatus.status == "available" ? temperatureScope(domain.properties.type) : "unknown";
            auto value = reading("temperature", scope, "temperature:" + std::to_string(index++), "C", "zesTemperatureGetState", domain.propertyStatus);
            if (domain.propertyStatus.status == "available") {
                double temperature = std::numeric_limits<double>::quiet_NaN();
                value.availability = api.zesTemperatureGetState ? outcome(api.zesTemperatureGetState(domain.handle, &temperature)) : missing(value.api);
                measured(value, temperature, false);
                locate(value, domain.properties);
            }
            snapshot.capabilities[value.api + ":" + value.instance] = value.availability;
            snapshot.readings.push_back(std::move(value));
        }
        ensureReading(snapshot, "temperature", "gpu_max", "C", "zesTemperatureGetState", "zesDeviceEnumTemperatureSensors");
        ensureReading(snapshot, "temperature", "memory_max", "C", "zesTemperatureGetState", "zesDeviceEnumTemperatureSensors");
    }

    void frequencies(Device& device, DeviceSnapshot& snapshot) {
        std::size_t index = 0;
        for (auto& domain : device.frequencies) {
            const auto instance = "frequency:" + std::to_string(index++);
            const auto scope = domain.propertyStatus.status == "available" ? frequencyScope(domain.properties.type) : "unknown";
            zes_freq_state_t state{};
            state.stype = ZES_STRUCTURE_TYPE_FREQ_STATE;
            state.actual = state.request = state.currentVoltage = -1;
            auto status = domain.propertyStatus;
            if (status.status == "available") status = api.zesFrequencyGetState ? outcome(api.zesFrequencyGetState(domain.handle, &state)) : missing("zesFrequencyGetState");
            snapshot.capabilities["zesFrequencyGetState:" + instance] = status;
            for (const auto& entry : {std::pair{"frequency.actual", state.actual},
                                     std::pair{"frequency.requested", state.request},
                                     std::pair{"voltage", state.currentVoltage}}) {
                auto value = reading(entry.first, scope, instance, entry.first == std::string_view("voltage") ? "V" : "MHz", "zesFrequencyGetState", status);
                locate(value, domain.properties);
                measured(value, entry.second);
                if (scope == "memory" && value.unit == "MHz")
                    value.note = "driver_reported_memory_frequency_not_verified_as_physical_clock";
                snapshot.readings.push_back(std::move(value));
            }
            auto throttle = reading("frequency.throttle_reasons", scope, instance, "bitmask", "zesFrequencyGetState", status);
            locate(throttle, domain.properties);
            measured(throttle, state.throttleReasons);
            const std::pair<std::uint32_t, const char*> flags[] = {
                {ZES_FREQ_THROTTLE_REASON_FLAG_AVE_PWR_CAP, "average_power_cap"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_BURST_PWR_CAP, "burst_power_cap"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_CURRENT_LIMIT, "current_limit"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_THERMAL_LIMIT, "thermal_limit"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_PSU_ALERT, "psu_alert"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_SW_RANGE, "software_range"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_HW_RANGE, "hardware_range"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_VOLTAGE, "voltage"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_THERMAL, "thermal"},
                {ZES_FREQ_THROTTLE_REASON_FLAG_POWER, "power"}};
            if (throttle.value) for (const auto& [mask, name] : flags)
                if (state.throttleReasons & mask) throttle.flags.emplace_back(name);
            snapshot.readings.push_back(std::move(throttle));
        }
        for (auto scope : {"gpu", "memory"}) {
            ensureReading(snapshot, "frequency.actual", scope, "MHz", "zesFrequencyGetState", "zesDeviceEnumFrequencyDomains");
            ensureReading(snapshot, "frequency.requested", scope, "MHz", "zesFrequencyGetState", "zesDeviceEnumFrequencyDomains");
            ensureReading(snapshot, "voltage", scope, "V", "zesFrequencyGetState", "zesDeviceEnumFrequencyDomains");
            ensureReading(snapshot, "frequency.throttle_reasons", scope, "bitmask", "zesFrequencyGetState", "zesDeviceEnumFrequencyDomains");
        }
    }

    void powers(Device& device, DeviceSnapshot& snapshot) {
        std::size_t index = 0;
        for (auto& domain : device.powers) {
            auto value = reading("power.average", powerScope(domain.domain), "power:" + std::to_string(index++), "W", "zesPowerGetEnergyCounter");
            zes_power_energy_counter_t energy{};
            value.availability = api.zesPowerGetEnergyCounter ? outcome(api.zesPowerGetEnergyCounter(domain.handle, &energy)) : missing(value.api);
            snapshot.capabilities[value.api + ":" + value.instance] = value.availability;
            if (domain.propertyStatus.status == "available") locate(value, domain.properties);
            if (value.availability.status == "available") {
                // microjoule / microsecond == watt; дополнительный множитель 10^6 не нужен.
                applyCounter(value, domain.counter.update({energy.energy, energy.timestamp}, 1.0), true);
            } else { value.derived = true; domain.counter.reset(); }
            snapshot.readings.push_back(std::move(value));
        }
        ensureReading(snapshot, "power.average", "card", "W", "zesPowerGetEnergyCounter", "zesDeviceEnumPowerDomains");
        ensureReading(snapshot, "power.average", "gpu", "W", "zesPowerGetEnergyCounter", "zesDeviceEnumPowerDomains");
    }

    void memories(Device& device, DeviceSnapshot& snapshot) {
        std::size_t index = 0;
        for (auto& domain : device.memories) {
            const auto instance = "memory:" + std::to_string(index++);
            const auto scope = domain.propertyStatus.status != "available" ? "unknown" :
                (domain.properties.location == ZES_MEM_LOC_DEVICE ? "device_allocatable" :
                 domain.properties.location == ZES_MEM_LOC_SYSTEM ? "system_allocatable" : "unknown");
            const auto physicalScope = domain.propertyStatus.status != "available" ? "unknown" :
                (domain.properties.location == ZES_MEM_LOC_DEVICE ? "device_physical" :
                 domain.properties.location == ZES_MEM_LOC_SYSTEM ? "system_physical" : "unknown");
            auto physical = reading("memory.physical_bytes", physicalScope, instance, "B", "zesMemoryGetProperties", domain.propertyStatus);
            locate(physical, domain.properties);
            if (domain.propertyStatus.status == "available" && domain.properties.physicalSize == 0)
                physical.availability = {"unsupported", "physical_size_unknown", {}};
            measured(physical, static_cast<double>(domain.properties.physicalSize));
            snapshot.readings.push_back(std::move(physical));
            zes_mem_state_t state{};
            state.stype = ZES_STRUCTURE_TYPE_MEM_STATE;
            auto status = api.zesMemoryGetState ? outcome(api.zesMemoryGetState(domain.handle, &state)) : missing("zesMemoryGetState");
            snapshot.capabilities["zesMemoryGetState:" + instance] = status;
            if (status.status == "available" && (state.size == 0 || state.free > state.size))
                status = {"invalid_data", "invalid_allocatable_memory_size", {}};
            for (auto key : {"memory.total_bytes", "memory.free_bytes", "memory.used_bytes"}) {
                auto value = reading(key, scope, instance, "B", "zesMemoryGetState", status);
                locate(value, domain.properties);
                value.note = "driver_allocatable_size_deprecated_not_physical_capacity";
                value.derived = key == std::string_view("memory.used_bytes");
                if (status.status == "available") {
                    auto bytes = key == std::string_view("memory.total_bytes") ? state.size :
                        key == std::string_view("memory.free_bytes") ? state.free : state.size - state.free;
                    measured(value, static_cast<double>(bytes));
                }
                snapshot.readings.push_back(std::move(value));
            }
            zes_mem_bandwidth_t bandwidth{};
            status = api.zesMemoryGetBandwidth ? outcome(api.zesMemoryGetBandwidth(domain.handle, &bandwidth)) : missing("zesMemoryGetBandwidth");
            snapshot.capabilities["zesMemoryGetBandwidth:" + instance] = status;
            auto read = reading("memory.read_bandwidth", scope, instance, "B/s", "zesMemoryGetBandwidth", status);
            auto write = reading("memory.write_bandwidth", scope, instance, "B/s", "zesMemoryGetBandwidth", status);
            locate(read, domain.properties); locate(write, domain.properties);
            if (status.status == "available") {
                // API отдаёт bytes, не сырые PMT-транзакции. Повторно умножать на 32 нельзя.
                const bool legacyValidityGuard = metadata.mode == "legacy";
                applyCounter(read, domain.readCounter.update({bandwidth.readCounter, bandwidth.timestamp}, legacyValidityGuard), true);
                applyCounter(write, domain.writeCounter.update({bandwidth.writeCounter, bandwidth.timestamp}, legacyValidityGuard), true);
                if (legacyValidityGuard) {
                    read.note = write.note = "legacy_api_has_no_per_counter_validity_flag";
                }
            } else {
                read.derived = write.derived = true;
                domain.readCounter.reset(); domain.writeCounter.reset();
            }
            snapshot.readings.push_back(std::move(read)); snapshot.readings.push_back(std::move(write));
        }
        for (auto key : {"memory.total_bytes", "memory.free_bytes", "memory.used_bytes"})
            if (device.memories.empty()) ensureReading(snapshot, key, "device_allocatable", "B", "zesMemoryGetState", "zesDeviceEnumMemoryModules");
        if (device.memories.empty()) {
            ensureReading(snapshot, "memory.physical_bytes", "device_physical", "B", "zesMemoryGetProperties", "zesDeviceEnumMemoryModules");
            ensureReading(snapshot, "memory.read_bandwidth", "device_allocatable", "B/s", "zesMemoryGetBandwidth", "zesDeviceEnumMemoryModules");
            ensureReading(snapshot, "memory.write_bandwidth", "device_allocatable", "B/s", "zesMemoryGetBandwidth", "zesDeviceEnumMemoryModules");
        }
    }

    void engines(Device& device, DeviceSnapshot& snapshot) {
        std::size_t index = 0;
        for (auto& domain : device.engines) {
            const auto instance = "engine:" + std::to_string(index++);
            if (domain.propertyStatus.status != "available") continue;
            const auto scope = engineScope(domain.properties.type);
            if (scope == "single_engine_not_aggregated") continue;
            auto value = reading("engine.utilization", scope, instance, "%", "zesEngineGetActivity");
            locate(value, domain.properties);
            zes_engine_stats_t state{};
            value.availability = api.zesEngineGetActivity ? outcome(api.zesEngineGetActivity(domain.handle, &state)) : missing(value.api);
            snapshot.capabilities[value.api + ":" + instance] = value.availability;
            if (value.availability.status == "available") {
                // Нельзя суммировать проценты engine. Берём только готовую aggregate group.
                applyCounter(value, domain.counter.update({state.activeTime, state.timestamp}, 100.0, 100.0), false);
                value.note = "ratio_of_implementation_specific_engine_time_counters";
            } else { value.derived = true; domain.counter.reset(); }
            snapshot.readings.push_back(std::move(value));
        }
        ensureReading(snapshot, "engine.utilization", "all", "%", "zesEngineGetActivity", "zesDeviceEnumEngineGroups");
    }

    void fans(DeviceSnapshot& snapshot) {
        // Без audited read-only Windows implementation fan RPM остаётся N/A.
        ensureReading(snapshot, "fan.speed", "fan", "RPM", "zesFanGetState", "zesDeviceEnumFans");
    }

    Snapshot sample() {
        auto snapshot = metadata;
        for (auto& device : devices) {
            auto output = device.identity;
            temperatures(device, output); frequencies(device, output); powers(device, output);
            memories(device, output); engines(device, output); fans(output);
            snapshot.devices.push_back(std::move(output));
        }
        return snapshot;
    }
};

SysmanBackend::SysmanBackend(bool legacy) : impl_(std::make_unique<Impl>(legacy)) {}
SysmanBackend::~SysmanBackend() = default;
Snapshot SysmanBackend::sample() { return impl_->sample(); }

} // namespace framescope::intel
