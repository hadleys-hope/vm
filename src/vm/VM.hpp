#pragma once

#include "hbc/HbcFormat.hpp"
#include "host/HostFunctions.hpp"
#include "runtime/Value.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hope::vm {

struct EventInstance {
    std::uint16_t eventIndex{};
    std::vector<runtime::Value> arguments;
};

// A handler ran longer than its instruction budget (an endless loop, a runaway computation).
struct BudgetExceeded : std::runtime_error {
    BudgetExceeded() : std::runtime_error("instruction budget exceeded") {}
};

// A recorded run, for watching a program work: every instruction executed (function, ip) and every call
// out of the program (host functions, events) with its arguments, its result and the step it happened at.
struct TraceStep {
    std::uint16_t fn{};
    std::uint32_t ip{};
};
struct TraceCall {
    std::uint32_t step{};
    std::string name;
    std::string args;
    std::string result;
};
struct Trace {
    std::vector<TraceStep> steps;
    std::vector<TraceCall> calls;
    static constexpr std::size_t kMaxSteps = 20000;
};

struct SimulationStats {
    std::int64_t virtualTimeMs{};
    std::size_t startHandlersInvoked{};
    std::size_t timedHandlersInvoked{};
    std::size_t eventHandlersInvoked{};
};

class VM {
public:
    explicit VM(const hbc::Module& module, std::uint64_t randomSeed = 123, host::HouseIo* io = nullptr);

    // Queue an event from outside (the runtime posts "Sensors" when a house's readings arrive).
    // Returns false if the program declares no such event.
    bool post(const std::string& name, std::vector<runtime::Value> arguments = {});
    bool hasEvent(const std::string& name) const { return eventByName_.contains(name); }

    // Loop iterations (backward jumps) and calls the VM may still make; past it the running handler is stopped
    // with BudgetExceeded. Straight-line code is not metered, it cannot run away.
    void setStepBudget(std::uint64_t steps) { budget_ = steps; }
    // Record the next runs into trace (nullptr stops recording). Costs one pointer test per instruction.
    void setTrace(Trace* trace) { trace_ = trace; }
    std::uint64_t stepsExecuted() const { return executed_; }

    void initializeGlobals();
    runtime::Value executeFunction(std::uint16_t functionIndex, const std::vector<runtime::Value>& args = {});
    runtime::Value executeFunction(const std::string& name, const std::vector<runtime::Value>& args = {});

    std::size_t pendingEventCount() const { return events_.size(); }
    std::size_t dispatchEvents();

    SimulationStats startLifecycle();

    SimulationStats simulateUntil(
        std::int64_t untilMs,
        std::size_t maxTimedHandlerInvocations = 1'000'000
    );

    std::int64_t virtualTimeMs() const { return virtualTimeMs_; }
    bool lifecycleStarted() const { return lifecycleStarted_; }

    const std::vector<runtime::Value>& globals() const { return globals_; }
    const runtime::Value& global(const std::string& name) const;

private:
    const hbc::Module& module_;
    host::Registry hosts_;
    std::vector<runtime::Value> globals_;
    std::vector<bool> globalInitialized_;
    std::unordered_map<std::string, std::uint16_t> functionByName_;
    std::unordered_map<std::string, std::uint16_t> globalByName_;
    std::unordered_map<std::string, std::uint16_t> eventByName_;
    std::deque<EventInstance> events_;

    bool lifecycleStarted_{};
    std::uint64_t budget_ = std::numeric_limits<std::uint64_t>::max();
    Trace* trace_ = nullptr;
    std::uint64_t executed_{};
    std::int64_t virtualTimeMs_{};
    std::vector<bool> atHandlerFired_;
    std::vector<std::int64_t> everyNextDueMs_;

    runtime::Value constantValue(std::uint16_t index) const;
    std::uint16_t functionIndex(const std::string& name) const;
    std::uint16_t structIndexByName(const std::string& name) const;
    void emitEvent(const std::string& name, std::vector<runtime::Value> arguments);

    std::int64_t nextTimedHandlerDueMs() const;
    SimulationStats runTimedHandlersAt(std::int64_t dueMs, std::size_t& remainingBudget);
};

} // namespace hope::vm
