#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace hope::host {

// What one house program sees of the world and what it asks the world to do.
// The runtime fills `sensors` from the bus before it runs a handler and reads `actuators`/`texts` after it.
struct HouseIo {
    std::int64_t houseId{};
    std::unordered_map<std::string, double> sensors;    // sense("t_in"); booleans are 0.0 / 1.0
    std::unordered_map<std::string, double> actuators;  // act("heater_on", 1.0)
    std::unordered_map<std::string, std::string> texts; // act_text("reason", "eco night")
    std::deque<std::string> logs;                       // log("..."), drained by the runtime
    static constexpr std::size_t kMaxLogs = 32;
};

} // namespace hope::host
