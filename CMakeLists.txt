cmake_minimum_required(VERSION 3.20)
project(hopevm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(hopevm
    src/main.cpp
    src/hbc/HbcReader.cpp
    src/runtime/Value.cpp
    src/host/HostFunctions.cpp
    src/vm/VM.cpp
)

target_include_directories(hopevm PRIVATE src)
target_compile_options(hopevm PRIVATE -Wall -Wextra -Wpedantic)
