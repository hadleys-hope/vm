cmake_minimum_required(VERSION 3.20)
project(hopevm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)

# The VM itself, linked by the CLI and by the runtime that hosts many VMs.
add_library(libhopevm STATIC
    src/hbc/HbcReader.cpp
    src/runtime/Value.cpp
    src/host/HostFunctions.cpp
    src/vm/VM.cpp
    src/vm/Disasm.cpp
)
target_include_directories(libhopevm PUBLIC src)
target_compile_options(libhopevm PRIVATE -Wall -Wextra -Wpedantic)

add_library(hopemqtt STATIC src/mqtt/MqttClient.cpp)
target_include_directories(hopemqtt PUBLIC src)
target_link_libraries(hopemqtt PUBLIC Threads::Threads)

add_executable(hopevm src/main.cpp)
target_link_libraries(hopevm PRIVATE libhopevm)

add_executable(hope-runtime src/colony/main.cpp)
target_link_libraries(hope-runtime PRIVATE libhopevm hopemqtt)

add_executable(bus-bench src/colony/bus_bench.cpp)
target_link_libraries(bus-bench PRIVATE hopemqtt)

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE hopemqtt)
add_test(NAME unit_tests COMMAND unit_tests)
