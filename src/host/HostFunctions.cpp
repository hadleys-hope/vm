#include "host/HostFunctions.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace hope::host {
namespace {

template <class T>
const T& expect(const runtime::Value& value, const char* name) {
    const auto* ptr = std::get_if<T>(&value);
    if (!ptr) throw std::runtime_error(std::string("host function expected ") + name);
    return *ptr;
}

void expectArity(const std::string& name, const std::vector<runtime::Value>& args, std::size_t expected) {
    if (args.size() != expected) {
        throw std::runtime_error(name + " expects " + std::to_string(expected) +
                                 " arguments, got " + std::to_string(args.size()));
    }
}

} // namespace

Registry::Registry(std::uint64_t seed, HouseIo* io) : rng_(seed), io_(io) {
    installBuiltins();
    if (io_) installHouseIo();
}

bool Registry::contains(const std::string& name) const {
    return functions_.contains(name);
}

runtime::Value Registry::call(const std::string& name, const std::vector<runtime::Value>& args) {
    const auto it = functions_.find(name);
    if (it == functions_.end()) throw std::runtime_error("unknown host function: " + name);
    return it->second(args);
}

void Registry::installBuiltins() {
    functions_["random_real"] = [this](const auto& args) -> runtime::Value {
        expectArity("random_real", args, 2);
        const auto low = expect<double>(args[0], "real");
        const auto high = expect<double>(args[1], "real");
        if (low > high) throw std::runtime_error("random_real: min > max");
        std::uniform_real_distribution<double> distribution(low, high);
        return distribution(rng_);
    };

    functions_["random_int"] = [this](const auto& args) -> runtime::Value {
        expectArity("random_int", args, 2);
        const auto low = expect<std::int64_t>(args[0], "int");
        const auto high = expect<std::int64_t>(args[1], "int");
        if (low > high) throw std::runtime_error("random_int: min > max");
        std::uniform_int_distribution<std::int64_t> distribution(low, high);
        return distribution(rng_);
    };

    functions_["clamp"] = [](const auto& args) -> runtime::Value {
        expectArity("clamp", args, 3);
        const auto value = expect<double>(args[0], "real");
        const auto low = expect<double>(args[1], "real");
        const auto high = expect<double>(args[2], "real");
        if (low > high) throw std::runtime_error("clamp: min > max");
        return std::clamp(value, low, high);
    };

    functions_["sqrt"] = [](const auto& args) -> runtime::Value {
        expectArity("sqrt", args, 1);
        const auto value = expect<double>(args[0], "real");
        if (value < 0.0) throw std::runtime_error("sqrt: negative argument");
        return std::sqrt(value);
    };

    functions_["sin"] = [](const auto& args) -> runtime::Value {
        expectArity("sin", args, 1);
        return std::sin(expect<double>(args[0], "real"));
    };

    functions_["cos"] = [](const auto& args) -> runtime::Value {
        expectArity("cos", args, 1);
        return std::cos(expect<double>(args[0], "real"));
    };

    functions_["log"] = [this](const auto& args) -> runtime::Value {
        expectArity("log", args, 1);
        const auto& line = expect<std::string>(args[0], "string");
        if (io_) {  // 5000 houses must not write to one stdout: the runtime drains and publishes these
            if (io_->logs.size() >= HouseIo::kMaxLogs) io_->logs.pop_front();
            io_->logs.push_back(line.substr(0, 512));
        } else {
            std::cout << "[hope] " << line << '\n';
        }
        return runtime::Unit{};
    };

    functions_["metric"] = [](const auto& args) -> runtime::Value {
        expectArity("metric", args, 2);
        const auto& name = expect<std::string>(args[0], "string");
        std::cout << "[metric] " << name << '=' << runtime::valueToString(args[1]) << '\n';
        return runtime::Unit{};
    };
}

void Registry::installHouseIo() {
    // A sensor the world has not sent (yet) reads as 0.0: a program must not crash because a field is missing.
    functions_["sense"] = [this](const auto& args) -> runtime::Value {
        expectArity("sense", args, 1);
        const auto it = io_->sensors.find(expect<std::string>(args[0], "string"));
        return it == io_->sensors.end() ? 0.0 : it->second;
    };

    functions_["act"] = [this](const auto& args) -> runtime::Value {
        expectArity("act", args, 2);
        io_->actuators[expect<std::string>(args[0], "string")] = expect<double>(args[1], "real");
        return runtime::Unit{};
    };

    functions_["act_text"] = [this](const auto& args) -> runtime::Value {
        expectArity("act_text", args, 2);
        io_->texts[expect<std::string>(args[0], "string")] = expect<std::string>(args[1], "string").substr(0, 64);
        return runtime::Unit{};
    };

    functions_["house_id"] = [this](const auto& args) -> runtime::Value {
        expectArity("house_id", args, 0);
        return io_->houseId;
    };
}

} // namespace hope::host
