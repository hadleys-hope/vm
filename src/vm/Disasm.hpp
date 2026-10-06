#pragma once

#include "hbc/HbcFormat.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hope::vm {

// One decoded instruction: where it starts, its mnemonic, its operands in words a person can read
// (constants and names resolved), and how many bytes it takes.
struct Instruction {
    std::uint32_t ip{};
    std::string op;
    std::string args;
    std::uint32_t size{};
};

std::vector<Instruction> disassemble(const hbc::Module& module, const hbc::Function& function);
std::string constantText(const hbc::Module& module, std::uint16_t index);

} // namespace hope::vm
