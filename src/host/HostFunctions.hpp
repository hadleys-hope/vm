#pragma once

#include "host/HouseIo.hpp"
#include "runtime/Value.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hope::host {

using HostFunction = std::function<runtime::Value(const std::vector<runtime::Value>&)>;

class Registry {
public:
    // io: the house this program controls; without it sense/act/house_id are not available
    explicit Registry(std::uint64_t seed = 123, HouseIo* io = nullptr);

    bool contains(const std::string& name) const;
    runtime::Value call(const std::string& name, const std::vector<runtime::Value>& args);

private:
    std::mt19937_64 rng_;
    HouseIo* io_;
    std::unordered_map<std::string, HostFunction> functions_;

    void installBuiltins();
    void installHouseIo();
};

} // namespace hope::host
