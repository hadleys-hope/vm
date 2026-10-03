#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace hope::hbc {

struct IntConstant { std::int64_t value; };
struct RealConstant { double value; };
struct BoolConstant { bool value; };
struct StringConstant { std::string value; };
struct TimeConstant { std::int64_t milliseconds; };
using Constant = std::variant<IntConstant, RealConstant, BoolConstant, StringConstant, TimeConstant>;

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct IntType {};
struct RealType {};
struct BoolType {};
struct StringType {};
struct TimeType {};
struct StructType { std::string name; };
struct ArrayType { TypePtr element; std::int32_t size; };
struct ListType { TypePtr element; };

struct Type {
    std::variant<IntType, RealType, BoolType, StringType, TimeType, StructType, ArrayType, ListType> value;
};

struct Function {
    std::uint16_t nameConstant{};
    std::uint16_t parameterCount{};
    std::uint16_t localCount{};
    std::vector<std::uint8_t> code;
};

struct Field {
    std::uint16_t nameConstant{};
    std::uint16_t typeIndex{};
};

struct StructDef {
    std::uint16_t nameConstant{};
    std::vector<Field> fields;
};

struct GlobalDef {
    std::uint16_t nameConstant{};
    std::uint16_t typeIndex{};
    std::uint16_t initializerFunction{};
    bool mutableValue{};
};

struct EventDef {
    std::uint16_t nameConstant{};
    std::vector<std::uint16_t> parameterTypes;
};

enum class HandlerKind : std::uint8_t {
    Event = 1,
    Start = 2,
    Every = 3,
    At = 4,
};

struct HandlerDef {
    HandlerKind kind{};
    std::uint16_t functionIndex{};
    std::uint16_t eventIndex{};
    std::int64_t milliseconds{};
};

struct Module {
    std::uint16_t version{};
    std::vector<Constant> constants;
    std::vector<Function> functions;
    std::vector<TypePtr> types;
    std::vector<StructDef> structs;
    std::vector<GlobalDef> globals;
    std::vector<EventDef> events;
    std::vector<HandlerDef> handlers;
    bool metadataPresent{};
};

} // namespace hope::hbc
