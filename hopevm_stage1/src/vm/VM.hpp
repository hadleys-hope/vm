#pragma once

#include "hbc/HbcFormat.hpp"
#include "host/HostFunctions.hpp"
#include "runtime/Value.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace hope::vm {

struct EventInstance {
    std::uint16_t eventIndex{};
    std::vector<runtime::Value> arguments;
};

struct SimulationStats {
    std::int64_t virtualTimeMs{};
    std::size_t startHandlersInvoked{};
    std::size_t timedHandlersInvoked{};
    std::size_t eventHandlersInvoked{};
};

class VM {
public:
    explicit VM(const hbc::Module& module, std::uint64_t randomSeed = 123);

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
