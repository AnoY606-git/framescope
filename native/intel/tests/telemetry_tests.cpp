// SPDX-License-Identifier: GPL-3.0-only
#include "../telemetry.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace framescope::intel;

void check(bool condition, const char* name) {
    if (!condition) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}

int main() {
    CounterTracker power;
    check(power.update({1'000'000, 1'000'000}, 1.0).availability.status == "warming_up", "power warmup");
    auto result = power.update({31'000'000, 2'000'000}, 1.0);
    check(result.value && *result.value == 30.0, "microjoule/microsecond produces watts");
    check(power.update({31'000'000, 2'000'000}, 1.0).availability.status == "stale", "stale timestamp");
    check(power.update({1, 3'000'000}, 1.0).availability.status == "warming_up", "counter reset");
    check(!power.update({1, 0}, 1.0).value, "zero timestamp is not a fake zero");
    power.reset();
    check(!power.update({1, 1}, 1.0).value, "explicit reset rewarms");

    CounterTracker bandwidth;
    bandwidth.update({100, 1'000'000}, 1e6);
    result = bandwidth.update({600, 1'500'000}, 1e6);
    check(result.value && *result.value == 1000.0, "bandwidth bytes per second, no extra x32");
    ByteCounterTracker legacyBytes;
    check(!legacyBytes.update({0, 100}, true).value, "legacy zero counter is unverified");
    check(!legacyBytes.update({0, 200}, true).value, "legacy absent counter not fake zero bandwidth");
    check(legacyBytes.update({100, 300}, true).availability.status == "warming_up", "legacy first nonzero baseline");
    result = legacyBytes.update({100, 400}, true);
    check(result.value && *result.value == 0, "verified unchanged byte counter is genuine zero");
    check(!legacyBytes.update({0, 500}, true).value, "legacy byte counter reset");
    CounterTracker utilization;
    utilization.update({1, 100}, 100.0, 100.0);
    result = utilization.update({51, 200}, 100.0, 100.0);
    check(result.value && *result.value == 50.0, "engine percent");
    check(!utilization.update({1000, 300}, 100.0, 100.0).value, "invalid percent not clamped");
    CounterTracker zero;
    zero.update({42, 1}, 1.0);
    result = zero.update({42, 2}, 1.0);
    check(result.value && *result.value == 0, "a measured zero remains available");

    JsonWriter json;
    json.object(); json.key("quote\"\\\n"); json.array(); json.null(); json.boolean(false);
    json.number(std::numeric_limits<double>::quiet_NaN()); json.string("\x01");
    json.endArray(); json.endObject();
    check(json.str() == "{\"quote\\\"\\\\\\n\":[null,false,null,\"\\u0001\"]}", "JSON escaping and nonfinite guard");
    Snapshot snapshot;
    DeviceSnapshot device;
    Reading unavailable;
    unavailable.key = "temperature";
    unavailable.value = 57; // Только unit fixture: недоступное значение не должно утечь в JSON.
    unavailable.availability = {"unsupported", "test_fixture", {}};
    device.readings.push_back(unavailable);
    snapshot.devices.push_back(device);
    const auto output = serialize(snapshot);
    check(output.find("\"value\":null") != std::string::npos, "unsupported serializes null");
    check(output.find("\"value\":57") == std::string::npos, "unsupported never serializes cached temperature");
    std::cout << "PASS: counters, units, reset, stale, unknown, JSON\n";
}
