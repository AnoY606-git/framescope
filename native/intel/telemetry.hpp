// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace framescope::intel {

struct Availability {
    std::string status = "unavailable";
    std::string reason;
    std::optional<std::uint32_t> count;
};

struct Reading {
    std::string key;
    std::string scope;
    std::string instance;
    std::string unit;
    std::string api;
    std::optional<double> value;
    Availability availability;
    std::optional<std::uint32_t> subdeviceId;
    std::optional<double> windowMs;
    bool derived = false;
    std::string note;
    std::vector<std::string> flags;
};

struct CounterSample {
    std::uint64_t value = 0;
    std::uint64_t timestamp = 0;
};

struct CounterResult {
    std::optional<double> value;
    Availability availability;
    std::uint64_t timestampDelta = 0;
};

class CounterTracker {
  public:
    CounterResult update(CounterSample current, double multiplier,
                         std::optional<double> maximum = std::nullopt) {
        if (current.timestamp == 0) {
            previous_.reset();
            return {{}, {"unavailable", "invalid_counter_timestamp", {}}, 0};
        }
        if (!previous_) {
            previous_ = current;
            return {{}, {"warming_up", "two_samples_required", {}}, 0};
        }
        const auto previous = *previous_;
        // Не угадываем разрядность счётчика: reset и rollover требуют нового baseline.
        if (current.timestamp < previous.timestamp || current.value < previous.value) {
            previous_ = current;
            return {{}, {"warming_up", "counter_reset_or_rollover", {}}, 0};
        }
        if (current.timestamp == previous.timestamp) {
            return {{}, {"stale", "counter_timestamp_not_advanced", {}}, 0};
        }
        previous_ = current;
        const auto elapsed = current.timestamp - previous.timestamp;
        const double result = static_cast<double>(current.value - previous.value) * multiplier /
                              static_cast<double>(elapsed);
        if (!std::isfinite(result) || result < 0 || (maximum && result > *maximum)) {
            return {{}, {"invalid_data", "counter_ratio_out_of_range", {}}, elapsed};
        }
        return {result, {"available", {}, {}}, elapsed};
    }

    void reset() { previous_.reset(); }

  private:
    std::optional<CounterSample> previous_;
};

class ByteCounterTracker {
  public:
    CounterResult update(CounterSample current, bool requireObservedCounter) {
        // Legacy Windows implementation может вернуть SUCCESS и нулевое поле
        // при ошибке отдельного KMD response. До первого ненулевого counter
        // такое поле нельзя выдавать за доказанный idle 0 B/s.
        if (requireObservedCounter && current.value == 0 && !observedNonzero_) {
            counter_.reset();
            return {{}, {"unavailable", "zero_counter_without_validity_flag", {}}, 0};
        }
        if (requireObservedCounter && !observedNonzero_) {
            observedNonzero_ = true;
            counter_.reset();
        }
        if (requireObservedCounter && current.value == 0) {
            reset();
            return {{}, {"warming_up", "counter_reset_or_rollover", {}}, 0};
        }
        return counter_.update(current, 1e6);
    }
    void reset() {
        counter_.reset();
        observedNonzero_ = false;
    }

  private:
    CounterTracker counter_;
    bool observedNonzero_ = false;
};

struct PciAddress {
    std::uint32_t domain = 0;
    std::uint32_t bus = 0;
    std::uint32_t device = 0;
    std::uint32_t function = 0;
};

struct DeviceSnapshot {
    std::string id;
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::string driverVersion;
    std::optional<PciAddress> pci;
    std::map<std::string, Availability> capabilities;
    std::vector<Reading> readings;
};

struct Snapshot {
    std::string timestamp;
    std::uint64_t monotonicMs = 0;
    std::uint64_t sequence = 0;
    std::uint32_t intervalMs = 1000;
    Availability backend;
    std::string mode;
    std::string loaderVersion;
    bool loaderLoadedFromSystem32 = false;
    std::map<std::string, bool> exports;
    std::map<std::string, Availability> initialization;
    std::vector<DeviceSnapshot> devices;
};

// Минимальный writer: locale-independent JSON, без внешней runtime-зависимости.
class JsonWriter {
  public:
    JsonWriter() { output_.imbue(std::locale::classic()); }
    void object() { beforeValue(); output_ << '{'; first_.push_back(true); }
    void array() { beforeValue(); output_ << '['; first_.push_back(true); }
    void endObject() { output_ << '}'; first_.pop_back(); }
    void endArray() { output_ << ']'; first_.pop_back(); }
    void key(std::string_view text) {
        comma();
        quoted(text);
        output_ << ':';
        afterKey_ = true;
    }
    void string(std::string_view text) { beforeValue(); quoted(text); }
    void boolean(bool value) { beforeValue(); output_ << (value ? "true" : "false"); }
    void integer(std::uint64_t value) { beforeValue(); output_ << value; }
    void number(double value) {
        beforeValue();
        if (std::isfinite(value)) output_ << std::setprecision(15) << value;
        else output_ << "null";
    }
    void null() { beforeValue(); output_ << "null"; }
    std::string str() const { return output_.str(); }

  private:
    void comma() {
        if (!first_.empty()) {
            if (!first_.back()) output_ << ',';
            first_.back() = false;
        }
    }
    void beforeValue() {
        if (afterKey_) afterKey_ = false;
        else comma();
    }
    void quoted(std::string_view text) {
        constexpr char hex[] = "0123456789abcdef";
        output_ << '"';
        for (unsigned char ch : text) {
            switch (ch) {
            case '"': output_ << "\\\""; break;
            case '\\': output_ << "\\\\"; break;
            case '\b': output_ << "\\b"; break;
            case '\f': output_ << "\\f"; break;
            case '\n': output_ << "\\n"; break;
            case '\r': output_ << "\\r"; break;
            case '\t': output_ << "\\t"; break;
            default:
                if (ch < 0x20) output_ << "\\u00" << hex[ch >> 4] << hex[ch & 15];
                else output_ << static_cast<char>(ch);
            }
        }
        output_ << '"';
    }
    std::ostringstream output_;
    std::vector<bool> first_;
    bool afterKey_ = false;
};

inline void writeAvailability(JsonWriter& json, const Availability& availability) {
    json.key("status"); json.string(availability.status);
    json.key("reason");
    if (availability.reason.empty()) json.null();
    else json.string(availability.reason);
    json.key("count");
    if (availability.count) json.integer(*availability.count);
    else json.null();
}

inline std::string serialize(const Snapshot& snapshot) {
    JsonWriter json;
    json.object();
    json.key("schemaVersion"); json.integer(1);
    json.key("source"); json.string("level-zero-sysman");
    json.key("timestamp"); json.string(snapshot.timestamp);
    json.key("monotonicMs"); json.integer(snapshot.monotonicMs);
    json.key("sequence"); json.integer(snapshot.sequence);
    json.key("intervalMs"); json.integer(snapshot.intervalMs);
    json.key("backend"); json.object();
    writeAvailability(json, snapshot.backend);
    json.key("mode"); json.string(snapshot.mode);
    json.key("headersTag"); json.string("v1.34.0");
    json.key("headersCommit"); json.string("934714b278f850d164627eeaaa043eee02785a9e");
    json.key("loader"); json.object();
    json.key("name"); json.string("ze_loader.dll");
    json.key("fileVersion");
    if (snapshot.loaderVersion.empty()) json.null();
    else json.string(snapshot.loaderVersion);
    json.key("loadedFromSystem32"); json.boolean(snapshot.loaderLoadedFromSystem32);
    json.endObject();
    json.key("exports"); json.object();
    for (const auto& [name, found] : snapshot.exports) {
        json.key(name); json.boolean(found);
    }
    json.endObject();
    json.key("initialization"); json.object();
    for (const auto& [name, availability] : snapshot.initialization) {
        json.key(name); json.object(); writeAvailability(json, availability); json.endObject();
    }
    json.endObject();
    json.endObject();
    json.key("devices"); json.array();
    for (const auto& device : snapshot.devices) {
        json.object();
        json.key("id"); json.string(device.id);
        json.key("name"); json.string(device.name);
        json.key("vendorId"); json.integer(device.vendorId);
        json.key("deviceId"); json.integer(device.deviceId);
        json.key("driverVersion");
        if (device.driverVersion.empty()) json.null();
        else json.string(device.driverVersion);
        json.key("pci");
        if (device.pci) {
            json.object();
            json.key("domain"); json.integer(device.pci->domain);
            json.key("bus"); json.integer(device.pci->bus);
            json.key("device"); json.integer(device.pci->device);
            json.key("function"); json.integer(device.pci->function);
            json.endObject();
        } else json.null();
        json.key("capabilities"); json.object();
        for (const auto& [name, availability] : device.capabilities) {
            json.key(name); json.object(); writeAvailability(json, availability); json.endObject();
        }
        json.endObject();
        json.key("readings"); json.array();
        for (const auto& reading : device.readings) {
            json.object();
            json.key("key"); json.string(reading.key);
            json.key("scope"); json.string(reading.scope);
            json.key("instance"); json.string(reading.instance);
            json.key("unit"); json.string(reading.unit);
            json.key("api"); json.string(reading.api);
            json.key("value");
            if (reading.value && reading.availability.status == "available") json.number(*reading.value);
            else json.null();
            writeAvailability(json, reading.availability);
            json.key("subdeviceId");
            if (reading.subdeviceId) json.integer(*reading.subdeviceId);
            else json.null();
            json.key("derived"); json.boolean(reading.derived);
            json.key("windowMs");
            if (reading.windowMs) json.number(*reading.windowMs);
            else json.null();
            json.key("note");
            if (reading.note.empty()) json.null();
            else json.string(reading.note);
            json.key("flags"); json.array();
            for (const auto& flag : reading.flags) json.string(flag);
            json.endArray();
            json.endObject();
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
    json.endObject();
    return json.str();
}

} // namespace framescope::intel
