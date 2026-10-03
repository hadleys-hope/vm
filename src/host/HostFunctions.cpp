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

Registry::Registry(std::uint64_t seed) : rng_(seed) {
    installBuiltins();
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

    functions_["log"] = [](const auto& args) -> runtime::Value {
        expectArity("log", args, 1);
        std::cout << "[hope] " << expect<std::string>(args[0], "string") << '\n';
        return runtime::Unit{};
    };

    functions_["metric"] = [](const auto& args) -> runtime::Value {
        expectArity("metric", args, 2);
        const auto& name = expect<std::string>(args[0], "string");
        std::cout << "[metric] " << name << '=' << runtime::valueToString(args[1]) << '\n';
        return runtime::Unit{};
    };
}

} // namespace hope::host
