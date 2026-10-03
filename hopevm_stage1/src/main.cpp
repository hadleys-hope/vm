#include "hbc/HbcReader.hpp"
#include "runtime/Value.hpp"
#include "vm/VM.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace hope;

namespace {

std::int64_t parseI64(const char* text, const char* name) {
    std::int64_t value{};
    const std::string input(text);
    const auto* begin = input.data();
    const auto* end = begin + input.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end) throw std::runtime_error(std::string("invalid ") + name + ": " + input);
    return value;
}

void printUsage() {
    std::cerr << "usage:\n"
              << "  hopevm <module.hbc>\n"
              << "  hopevm <module.hbc> --run <function>\n"
              << "  hopevm <module.hbc> --simulate <until-ms>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        printUsage();
        return 2;
    }
    if (argc == 4 && std::string(argv[2]) != "--run" && std::string(argv[2]) != "--simulate") {
        printUsage();
        return 2;
    }

    try {
        const auto module = hbc::Reader::readFile(std::filesystem::path(argv[1]));

        std::cout << "HBC v" << module.version << '\n';
        std::cout << "constants: " << module.constants.size() << '\n';
        std::cout << "functions: " << module.functions.size() << '\n';
        std::cout << "types: " << module.types.size() << '\n';
        std::cout << "structs: " << module.structs.size() << '\n';
        std::cout << "globals: " << module.globals.size() << '\n';
        std::cout << "events: " << module.events.size() << '\n';
        std::cout << "handlers: " << module.handlers.size() << '\n';

        vm::VM machine(module);
        const auto initStart = std::chrono::steady_clock::now();
        machine.initializeGlobals();
        const auto initEnd = std::chrono::steady_clock::now();

        std::cout << "\ninitialized globals:\n";
        for (std::size_t i = 0; i < module.globals.size(); ++i) {
            std::cout << "  " << hbc::stringConstant(module, module.globals[i].nameConstant)
                      << " = " << runtime::valueToString(machine.globals()[i]) << '\n';
        }
        const auto initMs = std::chrono::duration<double, std::milli>(initEnd - initStart).count();
        std::cout << "\nVM initialization: OK (" << initMs << " ms)\n";

        if (argc == 4 && std::string(argv[2]) == "--run") {
            const std::string function = argv[3];
            std::cout << "\nrunning: " << function << "()\n";
            const auto runStart = std::chrono::steady_clock::now();
            const auto result = machine.executeFunction(function);
            const auto runEnd = std::chrono::steady_clock::now();
            const auto runMs = std::chrono::duration<double, std::milli>(runEnd - runStart).count();
            std::cout << "result: " << runtime::valueToString(result) << '\n';
            std::cout << "execution time: " << runMs << " ms\n";
            std::cout << "queued events: " << machine.pendingEventCount() << '\n';
            const auto invoked = machine.dispatchEvents();
            std::cout << "event handlers invoked: " << invoked << '\n';
            std::cout << "VM execution: OK\n";
        }

        if (argc == 4 && std::string(argv[2]) == "--simulate") {
            const auto untilMs = parseI64(argv[3], "simulation time");
            std::cout << "\nsimulating until: " << untilMs << " ms\n";
            const auto runStart = std::chrono::steady_clock::now();
            const auto stats = machine.simulateUntil(untilMs);
            const auto runEnd = std::chrono::steady_clock::now();
            const auto runMs = std::chrono::duration<double, std::milli>(runEnd - runStart).count();
            std::cout << "virtual time: " << stats.virtualTimeMs << " ms\n";
            std::cout << "START handlers invoked: " << stats.startHandlersInvoked << '\n';
            std::cout << "timed handlers invoked: " << stats.timedHandlersInvoked << '\n';
            std::cout << "event handlers invoked: " << stats.eventHandlersInvoked << '\n';
            std::cout << "pending events: " << machine.pendingEventCount() << '\n';
            std::cout << "simulation wall time: " << runMs << " ms\n";
            std::cout << "final globals:\n";
            for (std::size_t i = 0; i < module.globals.size(); ++i) {
                std::cout << "  " << hbc::stringConstant(module, module.globals[i].nameConstant)
                          << " = " << runtime::valueToString(machine.globals()[i]) << '\n';
            }
            std::cout << "VM simulation: OK\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hopevm: " << error.what() << '\n';
        return 1;
    }
}
