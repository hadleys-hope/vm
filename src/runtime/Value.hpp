#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace hope::runtime {

struct Unit {};
struct TimeValue { std::int64_t milliseconds{}; };

struct StructObject;
struct ArrayObject;
struct ListObject;

using StructRef = std::shared_ptr<StructObject>;
using ArrayRef = std::shared_ptr<ArrayObject>;
using ListRef = std::shared_ptr<ListObject>;

using Value = std::variant<
    Unit,
    std::int64_t,
    double,
    bool,
    std::string,
    TimeValue,
    StructRef,
    ArrayRef,
    ListRef
>;

struct StructObject {
    std::uint16_t structIndex{};
    std::vector<Value> fields;
};

struct ArrayObject {
    std::uint16_t typeIndex{};
    std::vector<Value> elements;
};

struct ListObject {
    std::uint16_t typeIndex{};
    std::vector<Value> elements;
};

std::string valueToString(const Value& value);

} // namespace hope::runtime
