#pragma once

#include "hbc/HbcFormat.hpp"
#include <filesystem>
#include <stdexcept>

namespace hope::hbc {

class FormatError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    static Module readFile(const std::filesystem::path& path);
};

std::string constantToString(const Constant& constant);
std::string typeToString(const TypePtr& type);
std::string stringConstant(const Module& module, std::uint16_t index);

} // namespace hope::hbc
