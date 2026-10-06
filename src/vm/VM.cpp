#include "vm/VM.hpp"

#include "hbc/HbcReader.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace hope::vm {
namespace {

constexpr std::uint8_t OP_NOP = 0x00;
constexpr std::uint8_t OP_PUSH_CONST = 0x01;
constexpr std::uint8_t OP_LOAD_LOCAL = 0x02;
constexpr std::uint8_t OP_STORE_LOCAL = 0x03;
constexpr std::uint8_t OP_LOAD_GLOBAL = 0x04;
constexpr std::uint8_t OP_STORE_GLOBAL = 0x05;
constexpr std::uint8_t OP_NEGATE = 0x10;
constexpr std::uint8_t OP_NOT = 0x11;
constexpr std::uint8_t OP_INT_TO_REAL = 0x12;
constexpr std::uint8_t OP_ADD = 0x20;
constexpr std::uint8_t OP_SUBTRACT = 0x21;
constexpr std::uint8_t OP_MULTIPLY = 0x22;
constexpr std::uint8_t OP_DIVIDE = 0x23;
constexpr std::uint8_t OP_MODULO = 0x24;
constexpr std::uint8_t OP_EQUAL = 0x30;
constexpr std::uint8_t OP_NOT_EQUAL = 0x31;
constexpr std::uint8_t OP_LESS = 0x32;
constexpr std::uint8_t OP_LESS_OR_EQUAL = 0x33;
constexpr std::uint8_t OP_GREATER = 0x34;
constexpr std::uint8_t OP_GREATER_OR_EQUAL = 0x35;
constexpr std::uint8_t OP_AND = 0x36;
constexpr std::uint8_t OP_OR = 0x37;
constexpr std::uint8_t OP_JUMP = 0x40;
constexpr std::uint8_t OP_JUMP_IF_FALSE = 0x41;
constexpr std::uint8_t OP_CALL = 0x50;
constexpr std::uint8_t OP_EMIT = 0x51;
constexpr std::uint8_t OP_RETURN = 0x60;
constexpr std::uint8_t OP_RETURN_VALUE = 0x61;
constexpr std::uint8_t OP_POP = 0x62;
constexpr std::uint8_t OP_NEW_STRUCT = 0x70;
constexpr std::uint8_t OP_LOAD_FIELD = 0x71;
constexpr std::uint8_t OP_STORE_FIELD = 0x72;
constexpr std::uint8_t OP_NEW_ARRAY = 0x73;
constexpr std::uint8_t OP_NEW_LIST = 0x74;
constexpr std::uint8_t OP_LOAD_INDEX = 0x75;
constexpr std::uint8_t OP_STORE_INDEX = 0x76;
constexpr std::uint8_t OP_LENGTH = 0x77;
constexpr std::uint8_t OP_LIST_APPEND = 0x78;
constexpr std::uint8_t OP_LIST_REMOVE = 0x79;
constexpr std::uint8_t OP_NEW_ARRAY_INIT = 0x7a;

std::uint16_t readU16(const std::vector<std::uint8_t>& code, std::size_t& ip) {
    if (ip + 2 > code.size()) throw std::runtime_error("truncated u16 operand");
    const auto value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(code[ip]) << 8) | code[ip + 1]);
    ip += 2;
    return value;
}

std::int32_t readI32(const std::vector<std::uint8_t>& code, std::size_t& ip) {
    if (ip + 4 > code.size()) throw std::runtime_error("truncated i32 operand");
    std::uint32_t raw = 0;
    for (int i = 0; i < 4; ++i) raw = (raw << 8) | code[ip + i];
    ip += 4;
    return static_cast<std::int32_t>(raw);
}

runtime::Value pop(std::vector<runtime::Value>& stack) {
    if (stack.empty()) throw std::runtime_error("operand stack underflow");
    auto value = std::move(stack.back());
    stack.pop_back();
    return value;
}

template <class T>
const T& expect(const runtime::Value& value, const char* name) {
    const auto* ptr = std::get_if<T>(&value);
    if (!ptr) throw std::runtime_error(std::string("expected ") + name);
    return *ptr;
}

bool truthy(const runtime::Value& value) {
    return expect<bool>(value, "bool");
}

runtime::Value numericBinary(std::uint8_t opcode, const runtime::Value& left, const runtime::Value& right) {
    if (const auto* li = std::get_if<std::int64_t>(&left)) {
        if (const auto* ri = std::get_if<std::int64_t>(&right)) {
            switch (opcode) {
                case OP_ADD: return *li + *ri;
                case OP_SUBTRACT: return *li - *ri;
                case OP_MULTIPLY: return *li * *ri;
                case OP_DIVIDE:
                    if (*ri == 0) throw std::runtime_error("integer division by zero");
                    return *li / *ri;
                case OP_MODULO:
                    if (*ri == 0) throw std::runtime_error("integer modulo by zero");
                    return *li % *ri;
                case OP_LESS: return *li < *ri;
                case OP_LESS_OR_EQUAL: return *li <= *ri;
                case OP_GREATER: return *li > *ri;
                case OP_GREATER_OR_EQUAL: return *li >= *ri;
                default: break;
            }
        }
    }
    const auto* ld = std::get_if<double>(&left);
    const auto* rd = std::get_if<double>(&right);
    if (ld && rd) {
        switch (opcode) {
            case OP_ADD: return *ld + *rd;
            case OP_SUBTRACT: return *ld - *rd;
            case OP_MULTIPLY: return *ld * *rd;
            case OP_DIVIDE:
                if (*rd == 0.0) throw std::runtime_error("real division by zero");
                return *ld / *rd;
            case OP_LESS: return *ld < *rd;
            case OP_LESS_OR_EQUAL: return *ld <= *rd;
            case OP_GREATER: return *ld > *rd;
            case OP_GREATER_OR_EQUAL: return *ld >= *rd;
            default: break;
        }
    }
    throw std::runtime_error("numeric operand type mismatch");
}

bool equalValues(const runtime::Value& a, const runtime::Value& b) {
    if (a.index() != b.index()) return false;
    return std::visit([&](const auto& av) -> bool {
        using T = std::decay_t<decltype(av)>;
        const auto& bv = std::get<T>(b);
        if constexpr (std::is_same_v<T, runtime::Unit>) return true;
        else if constexpr (std::is_same_v<T, runtime::TimeValue>) return av.milliseconds == bv.milliseconds;
        else return av == bv;
    }, a);
}

} // namespace

VM::VM(const hbc::Module& module, std::uint64_t randomSeed, host::HouseIo* io)
    : module_(module), hosts_(randomSeed, io) {
    globals_.resize(module_.globals.size());
    globalInitialized_.assign(module_.globals.size(), false);
    atHandlerFired_.assign(module_.handlers.size(), false);
    everyNextDueMs_.assign(module_.handlers.size(), -1);
    for (std::uint16_t i = 0; i < module_.functions.size(); ++i) {
        functionByName_.emplace(hbc::stringConstant(module_, module_.functions[i].nameConstant), i);
    }
    for (std::uint16_t i = 0; i < module_.globals.size(); ++i) {
        globalByName_.emplace(hbc::stringConstant(module_, module_.globals[i].nameConstant), i);
    }
    for (std::uint16_t i = 0; i < module_.events.size(); ++i) {
        eventByName_.emplace(hbc::stringConstant(module_, module_.events[i].nameConstant), i);
    }
    for (const auto& handler : module_.handlers) {
        if (handler.functionIndex >= module_.functions.size()) throw std::runtime_error("handler function index out of range");
        if (handler.kind == hbc::HandlerKind::Event && handler.eventIndex >= module_.events.size()) {
            throw std::runtime_error("handler event index out of range");
        }
        if (handler.kind == hbc::HandlerKind::Every && handler.milliseconds <= 0) {
            throw std::runtime_error("EVERY interval must be positive");
        }
        if (handler.kind == hbc::HandlerKind::At && handler.milliseconds < 0) {
            throw std::runtime_error("AT time cannot be negative");
        }
    }
}

std::uint16_t VM::functionIndex(const std::string& name) const {
    const auto it = functionByName_.find(name);
    if (it == functionByName_.end()) throw std::runtime_error("unknown function: " + name);
    return it->second;
}

std::uint16_t VM::structIndexByName(const std::string& name) const {
    for (std::uint16_t i = 0; i < module_.structs.size(); ++i) {
        if (hbc::stringConstant(module_, module_.structs[i].nameConstant) == name) return i;
    }
    throw std::runtime_error("unknown struct: " + name);
}

runtime::Value VM::constantValue(std::uint16_t index) const {
    if (index >= module_.constants.size()) throw std::runtime_error("constant index out of range");
    return std::visit([](const auto& c) -> runtime::Value {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, hbc::IntConstant>) return c.value;
        else if constexpr (std::is_same_v<T, hbc::RealConstant>) return c.value;
        else if constexpr (std::is_same_v<T, hbc::BoolConstant>) return c.value;
        else if constexpr (std::is_same_v<T, hbc::StringConstant>) return c.value;
        else return runtime::TimeValue{c.milliseconds};
    }, module_.constants[index]);
}

void VM::initializeGlobals() {
    for (std::uint16_t i = 0; i < module_.globals.size(); ++i) {
        globals_[i] = executeFunction(module_.globals[i].initializerFunction);
        globalInitialized_[i] = true;
    }
}

const runtime::Value& VM::global(const std::string& name) const {
    const auto it = globalByName_.find(name);
    if (it == globalByName_.end()) throw std::runtime_error("unknown global: " + name);
    if (!globalInitialized_[it->second]) throw std::runtime_error("global not initialized: " + name);
    return globals_[it->second];
}

runtime::Value VM::executeFunction(const std::string& name, const std::vector<runtime::Value>& args) {
    const auto it = functionByName_.find(name);
    if (it != functionByName_.end()) return executeFunction(it->second, args);
    if (hosts_.contains(name)) return hosts_.call(name, args);
    throw std::runtime_error("unknown function: " + name);
}

bool VM::post(const std::string& name, std::vector<runtime::Value> arguments) {
    if (!eventByName_.contains(name)) return false;
    emitEvent(name, std::move(arguments));
    return true;
}

void VM::emitEvent(const std::string& name, std::vector<runtime::Value> arguments) {
    const auto it = eventByName_.find(name);
    if (it == eventByName_.end()) throw std::runtime_error("unknown event: " + name);
    const auto& signature = module_.events[it->second].parameterTypes;
    if (arguments.size() != signature.size()) throw std::runtime_error("event argument count mismatch: " + name);
    events_.push_back(EventInstance{it->second, std::move(arguments)});
}

std::size_t VM::dispatchEvents() {
    std::size_t invoked = 0;
    while (!events_.empty()) {
        auto event = std::move(events_.front());
        events_.pop_front();
        for (const auto& handler : module_.handlers) {
            if (handler.kind != hbc::HandlerKind::Event || handler.eventIndex != event.eventIndex) continue;
            executeFunction(handler.functionIndex, event.arguments);
            ++invoked;
        }
    }
    return invoked;
}

SimulationStats VM::startLifecycle() {
    if (lifecycleStarted_) {
        return SimulationStats{virtualTimeMs_, 0, 0, 0};
    }

    lifecycleStarted_ = true;
    virtualTimeMs_ = 0;

    SimulationStats stats;
    stats.virtualTimeMs = virtualTimeMs_;

    for (const auto& handler : module_.handlers) {
        if (handler.kind != hbc::HandlerKind::Start) continue;
        executeFunction(handler.functionIndex);
        ++stats.startHandlersInvoked;
    }

    stats.eventHandlersInvoked += dispatchEvents();

    std::size_t unlimitedBudget = std::numeric_limits<std::size_t>::max();
    const auto atZero = runTimedHandlersAt(0, unlimitedBudget);
    stats.timedHandlersInvoked += atZero.timedHandlersInvoked;
    stats.eventHandlersInvoked += atZero.eventHandlersInvoked;

    for (std::size_t i = 0; i < module_.handlers.size(); ++i) {
        const auto& handler = module_.handlers[i];
        if (handler.kind == hbc::HandlerKind::Every) {
            if (handler.milliseconds <= 0) throw std::runtime_error("EVERY interval must be positive");
            everyNextDueMs_[i] = handler.milliseconds;
        }
    }

    stats.virtualTimeMs = virtualTimeMs_;
    return stats;
}

std::int64_t VM::nextTimedHandlerDueMs() const {
    std::int64_t next = std::numeric_limits<std::int64_t>::max();
    for (std::size_t i = 0; i < module_.handlers.size(); ++i) {
        const auto& handler = module_.handlers[i];
        if (handler.kind == hbc::HandlerKind::At) {
            if (!atHandlerFired_[i] && handler.milliseconds > virtualTimeMs_) {
                next = std::min(next, handler.milliseconds);
            }
        } else if (handler.kind == hbc::HandlerKind::Every) {
            const auto due = everyNextDueMs_[i];
            if (due >= 0 && due > virtualTimeMs_) next = std::min(next, due);
        }
    }
    return next;
}

SimulationStats VM::runTimedHandlersAt(std::int64_t dueMs, std::size_t& remainingBudget) {
    SimulationStats stats;
    virtualTimeMs_ = dueMs;
    stats.virtualTimeMs = virtualTimeMs_;

    for (std::size_t i = 0; i < module_.handlers.size(); ++i) {
        const auto& handler = module_.handlers[i];
        bool due = false;

        if (handler.kind == hbc::HandlerKind::At) {
            if (!atHandlerFired_[i] && handler.milliseconds == dueMs) {
                due = true;
                atHandlerFired_[i] = true;
            }
        } else if (handler.kind == hbc::HandlerKind::Every) {
            if (everyNextDueMs_[i] == dueMs) {
                due = true;
                if (handler.milliseconds <= 0) throw std::runtime_error("EVERY interval must be positive");
                if (dueMs > std::numeric_limits<std::int64_t>::max() - handler.milliseconds) {
                    everyNextDueMs_[i] = -1;
                } else {
                    everyNextDueMs_[i] = dueMs + handler.milliseconds;
                }
            }
        }

        if (!due) continue;
        if (remainingBudget == 0) throw std::runtime_error("scheduler handler invocation limit exceeded");
        --remainingBudget;
        executeFunction(handler.functionIndex);
        ++stats.timedHandlersInvoked;
    }

    stats.eventHandlersInvoked += dispatchEvents();
    return stats;
}

SimulationStats VM::simulateUntil(std::int64_t untilMs, std::size_t maxTimedHandlerInvocations) {
    if (untilMs < 0) throw std::runtime_error("simulation time cannot be negative");
    if (untilMs < virtualTimeMs_) throw std::runtime_error("cannot move virtual time backwards");

    SimulationStats total;
    if (!lifecycleStarted_) {
        const auto startStats = startLifecycle();
        total.startHandlersInvoked += startStats.startHandlersInvoked;
        total.timedHandlersInvoked += startStats.timedHandlersInvoked;
        total.eventHandlersInvoked += startStats.eventHandlersInvoked;
    }

    std::size_t remainingBudget = maxTimedHandlerInvocations;
    while (true) {
        const auto due = nextTimedHandlerDueMs();
        if (due == std::numeric_limits<std::int64_t>::max() || due > untilMs) break;
        const auto step = runTimedHandlersAt(due, remainingBudget);
        total.timedHandlersInvoked += step.timedHandlersInvoked;
        total.eventHandlersInvoked += step.eventHandlersInvoked;
    }

    virtualTimeMs_ = untilMs;
    total.virtualTimeMs = virtualTimeMs_;
    return total;
}

runtime::Value VM::executeFunction(std::uint16_t functionIndexValue, const std::vector<runtime::Value>& args) {
    if (functionIndexValue >= module_.functions.size()) throw std::runtime_error("function index out of range");
    const auto& function = module_.functions[functionIndexValue];
    if (args.size() != function.parameterCount) throw std::runtime_error("function argument count mismatch");

    std::vector<runtime::Value> locals(function.localCount);
    for (std::size_t i = 0; i < args.size(); ++i) locals[i] = args[i];
    std::vector<runtime::Value> stack;
    std::size_t ip = 0;

    // Every loop iteration goes through a backward jump and every recursion through a call, so charging only
    // those two stops any runaway handler while straight-line code runs unmetered (no cost per instruction).
    auto charge = [&]() {
        if (budget_ == 0) throw BudgetExceeded();
        --budget_;
        ++executed_;
    };
    auto jumpRelative = [&](std::int32_t displacement) {
        if (displacement < 0) charge();
        const auto target = static_cast<std::int64_t>(ip) + displacement;
        if (target < 0 || target >= static_cast<std::int64_t>(function.code.size())) throw std::runtime_error("jump target out of range");
        ip = static_cast<std::size_t>(target);
    };

    while (ip < function.code.size()) {
        if (trace_ && trace_->steps.size() < Trace::kMaxSteps)
            trace_->steps.push_back({functionIndexValue, static_cast<std::uint32_t>(ip)});
        const auto opcode = function.code[ip++];
        switch (opcode) {
            case OP_NOP: break;
            case OP_PUSH_CONST: stack.push_back(constantValue(readU16(function.code, ip))); break;
            case OP_LOAD_LOCAL: {
                const auto slot = readU16(function.code, ip);
                if (slot >= locals.size()) throw std::runtime_error("local slot out of range");
                stack.push_back(locals[slot]);
                break;
            }
            case OP_STORE_LOCAL: {
                const auto slot = readU16(function.code, ip);
                if (slot >= locals.size()) throw std::runtime_error("local slot out of range");
                locals[slot] = pop(stack);
                break;
            }
            case OP_LOAD_GLOBAL: {
                const auto name = hbc::stringConstant(module_, readU16(function.code, ip));
                const auto it = globalByName_.find(name);
                if (it == globalByName_.end() || !globalInitialized_[it->second]) throw std::runtime_error("global unavailable: " + name);
                stack.push_back(globals_[it->second]);
                break;
            }
            case OP_STORE_GLOBAL: {
                const auto name = hbc::stringConstant(module_, readU16(function.code, ip));
                const auto it = globalByName_.find(name);
                if (it == globalByName_.end()) throw std::runtime_error("unknown global: " + name);
                if (!module_.globals[it->second].mutableValue) throw std::runtime_error("cannot store immutable global: " + name);
                globals_[it->second] = pop(stack);
                globalInitialized_[it->second] = true;
                break;
            }
            case OP_NEGATE: {
                const auto value = pop(stack);
                if (const auto* i = std::get_if<std::int64_t>(&value)) stack.push_back(-*i);
                else if (const auto* d = std::get_if<double>(&value)) stack.push_back(-*d);
                else throw std::runtime_error("NEGATE expects numeric value");
                break;
            }
            case OP_NOT: stack.push_back(!truthy(pop(stack))); break;
            case OP_INT_TO_REAL: stack.push_back(static_cast<double>(expect<std::int64_t>(pop(stack), "int"))); break;
            case OP_ADD: case OP_SUBTRACT: case OP_MULTIPLY: case OP_DIVIDE: case OP_MODULO:
            case OP_LESS: case OP_LESS_OR_EQUAL: case OP_GREATER: case OP_GREATER_OR_EQUAL: {
                const auto right = pop(stack); const auto left = pop(stack);
                stack.push_back(numericBinary(opcode, left, right));
                break;
            }
            case OP_EQUAL: case OP_NOT_EQUAL: {
                const auto right = pop(stack); const auto left = pop(stack);
                const bool eq = equalValues(left, right);
                stack.push_back(opcode == OP_EQUAL ? eq : !eq);
                break;
            }
            case OP_AND: case OP_OR: {
                const auto right = truthy(pop(stack)); const auto left = truthy(pop(stack));
                stack.push_back(opcode == OP_AND ? (left && right) : (left || right));
                break;
            }
            case OP_JUMP: jumpRelative(readI32(function.code, ip)); break;
            case OP_JUMP_IF_FALSE: {
                const auto displacement = readI32(function.code, ip);
                if (!truthy(pop(stack))) jumpRelative(displacement);
                break;
            }
            case OP_CALL: {
                charge();
                const auto name = hbc::stringConstant(module_, readU16(function.code, ip));
                const auto argc = readU16(function.code, ip);
                std::vector<runtime::Value> callArgs(argc);
                for (std::size_t i = argc; i-- > 0;) callArgs[i] = pop(stack);
                if (trace_ && !functionByName_.contains(name)) {
                    // calls out of the program (sense, act, log, ...) are what a trace is for
                    TraceCall call{static_cast<std::uint32_t>(trace_->steps.size()), name, "", ""};
                    for (std::size_t i = 0; i < callArgs.size(); ++i)
                        call.args += (i ? ", " : "") + runtime::valueToString(callArgs[i]);
                    auto result = executeFunction(name, callArgs);
                    if (!std::holds_alternative<runtime::Unit>(result)) call.result = runtime::valueToString(result);
                    trace_->calls.push_back(std::move(call));
                    stack.push_back(std::move(result));
                    break;
                }
                stack.push_back(executeFunction(name, callArgs));
                break;
            }
            case OP_EMIT: {
                const auto name = hbc::stringConstant(module_, readU16(function.code, ip));
                const auto argc = readU16(function.code, ip);
                std::vector<runtime::Value> eventArgs(argc);
                for (std::size_t i = argc; i-- > 0;) eventArgs[i] = pop(stack);
                emitEvent(name, std::move(eventArgs));
                break;
            }
            case OP_RETURN: return runtime::Unit{};
            case OP_RETURN_VALUE: return pop(stack);
            case OP_POP: (void)pop(stack); break;
            case OP_NEW_STRUCT: {
                const auto structIndex = readU16(function.code, ip);
                if (structIndex >= module_.structs.size()) throw std::runtime_error("struct index out of range");
                const auto fieldCount = module_.structs[structIndex].fields.size();
                auto object = std::make_shared<runtime::StructObject>();
                object->structIndex = structIndex;
                object->fields.resize(fieldCount);
                for (std::size_t i = fieldCount; i-- > 0;) object->fields[i] = pop(stack);
                stack.push_back(object);
                break;
            }
            case OP_LOAD_FIELD: {
                const auto structIndex = readU16(function.code, ip);
                const auto fieldIndex = readU16(function.code, ip);
                (void)structIndex;
                auto object = expect<runtime::StructRef>(pop(stack), "struct");
                if (!object || fieldIndex >= object->fields.size()) throw std::runtime_error("field index out of range");
                stack.push_back(object->fields[fieldIndex]);
                break;
            }
            case OP_STORE_FIELD: {
                const auto structIndex = readU16(function.code, ip);
                const auto fieldIndex = readU16(function.code, ip);
                (void)structIndex;
                const auto value = pop(stack);
                auto object = expect<runtime::StructRef>(pop(stack), "struct");
                if (!object || fieldIndex >= object->fields.size()) throw std::runtime_error("field index out of range");
                object->fields[fieldIndex] = value;
                break;
            }
            case OP_NEW_ARRAY: {
                const auto typeIndex = readU16(function.code, ip);
                if (typeIndex >= module_.types.size()) throw std::runtime_error("array type index out of range");
                const auto* arrayType = std::get_if<hbc::ArrayType>(&module_.types[typeIndex]->value);
                if (!arrayType) throw std::runtime_error("NEW_ARRAY expected array type");
                auto array = std::make_shared<runtime::ArrayObject>();
                array->typeIndex = typeIndex;
                array->elements.resize(static_cast<std::size_t>(arrayType->size));
                for (std::size_t i = array->elements.size(); i-- > 0;) array->elements[i] = pop(stack);
                stack.push_back(array);
                break;
            }
            case OP_NEW_LIST: {
                const auto typeIndex = readU16(function.code, ip);
                const auto count = readU16(function.code, ip);
                auto list = std::make_shared<runtime::ListObject>();
                list->typeIndex = typeIndex;
                list->elements.resize(count);
                for (std::size_t i = count; i-- > 0;) list->elements[i] = pop(stack);
                stack.push_back(list);
                break;
            }
            case OP_LOAD_INDEX: {
                const auto index = expect<std::int64_t>(pop(stack), "int index");
                auto target = pop(stack);
                if (index < 0) throw std::runtime_error("negative index");
                if (const auto* array = std::get_if<runtime::ArrayRef>(&target)) {
                    if (!*array || static_cast<std::size_t>(index) >= (*array)->elements.size()) throw std::runtime_error("array index out of range");
                    stack.push_back((*array)->elements[static_cast<std::size_t>(index)]);
                } else if (const auto* list = std::get_if<runtime::ListRef>(&target)) {
                    if (!*list || static_cast<std::size_t>(index) >= (*list)->elements.size()) throw std::runtime_error("list index out of range");
                    stack.push_back((*list)->elements[static_cast<std::size_t>(index)]);
                } else throw std::runtime_error("LOAD_INDEX expects array/list");
                break;
            }
            case OP_STORE_INDEX: {
                const auto value = pop(stack);
                const auto index = expect<std::int64_t>(pop(stack), "int index");
                auto target = pop(stack);
                if (index < 0) throw std::runtime_error("negative index");
                if (auto* array = std::get_if<runtime::ArrayRef>(&target)) {
                    if (!*array || static_cast<std::size_t>(index) >= (*array)->elements.size()) throw std::runtime_error("array index out of range");
                    (*array)->elements[static_cast<std::size_t>(index)] = value;
                } else if (auto* list = std::get_if<runtime::ListRef>(&target)) {
                    if (!*list || static_cast<std::size_t>(index) >= (*list)->elements.size()) throw std::runtime_error("list index out of range");
                    (*list)->elements[static_cast<std::size_t>(index)] = value;
                } else throw std::runtime_error("STORE_INDEX expects array/list");
                break;
            }
            case OP_LENGTH: {
                const auto target = pop(stack);
                if (const auto* array = std::get_if<runtime::ArrayRef>(&target)) stack.push_back(static_cast<std::int64_t>((*array)->elements.size()));
                else if (const auto* list = std::get_if<runtime::ListRef>(&target)) stack.push_back(static_cast<std::int64_t>((*list)->elements.size()));
                else throw std::runtime_error("LENGTH expects array/list");
                break;
            }
            case OP_LIST_APPEND: {
                const auto value = pop(stack);
                auto list = expect<runtime::ListRef>(pop(stack), "list");
                if (!list) throw std::runtime_error("null list");
                list->elements.push_back(value);
                break;
            }
            case OP_LIST_REMOVE: {
                const auto index = expect<std::int64_t>(pop(stack), "int index");
                auto list = expect<runtime::ListRef>(pop(stack), "list");
                if (!list || index < 0 || static_cast<std::size_t>(index) >= list->elements.size()) throw std::runtime_error("list index out of range");
                auto value = list->elements[static_cast<std::size_t>(index)];
                list->elements.erase(list->elements.begin() + index);
                stack.push_back(value);
                break;
            }
            case OP_NEW_ARRAY_INIT: {
                const auto typeIndex = readU16(function.code, ip);
                const auto initializerName = hbc::stringConstant(module_, readU16(function.code, ip));
                if (typeIndex >= module_.types.size()) throw std::runtime_error("array type index out of range");
                const auto* arrayType = std::get_if<hbc::ArrayType>(&module_.types[typeIndex]->value);
                if (!arrayType) throw std::runtime_error("NEW_ARRAY_INIT expected array type");
                auto array = std::make_shared<runtime::ArrayObject>();
                array->typeIndex = typeIndex;
                array->elements.reserve(static_cast<std::size_t>(arrayType->size));
                for (std::int32_t i = 0; i < arrayType->size; ++i) array->elements.push_back(executeFunction(initializerName));
                stack.push_back(array);
                break;
            }
            default: throw std::runtime_error("unsupported opcode 0x" + std::to_string(opcode));
        }
    }
    throw std::runtime_error("function fell off end without return");
}

} // namespace hope::vm
// я ебал это писать