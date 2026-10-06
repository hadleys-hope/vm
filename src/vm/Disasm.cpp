#include "vm/Disasm.hpp"

#include "hbc/HbcReader.hpp"

#include <cstdio>
#include <cstring>

namespace hope::vm {
namespace {

// operand layout of every opcode, as VM.cpp reads it: N = none, U = one u16, UU = two u16, J = i32 jump
enum Kind { N, U, UU, J };
struct Op { std::uint8_t code; const char* name; Kind kind; };
constexpr Op OPS[] = {
    {0x00, "NOP", N}, {0x01, "PUSH_CONST", U}, {0x02, "LOAD_LOCAL", U}, {0x03, "STORE_LOCAL", U},
    {0x04, "LOAD_GLOBAL", U}, {0x05, "STORE_GLOBAL", U}, {0x10, "NEGATE", N}, {0x11, "NOT", N},
    {0x12, "INT_TO_REAL", N}, {0x20, "ADD", N}, {0x21, "SUBTRACT", N}, {0x22, "MULTIPLY", N},
    {0x23, "DIVIDE", N}, {0x24, "MODULO", N}, {0x30, "EQUAL", N}, {0x31, "NOT_EQUAL", N}, {0x32, "LESS", N},
    {0x33, "LESS_OR_EQUAL", N}, {0x34, "GREATER", N}, {0x35, "GREATER_OR_EQUAL", N}, {0x36, "AND", N},
    {0x37, "OR", N}, {0x40, "JUMP", J}, {0x41, "JUMP_IF_FALSE", J}, {0x50, "CALL", UU}, {0x51, "EMIT", UU},
    {0x60, "RETURN", N}, {0x61, "RETURN_VALUE", N}, {0x62, "POP", N}, {0x70, "NEW_STRUCT", U},
    {0x71, "LOAD_FIELD", UU}, {0x72, "STORE_FIELD", UU}, {0x73, "NEW_ARRAY", U}, {0x74, "NEW_LIST", UU},
    {0x75, "LOAD_INDEX", N}, {0x76, "STORE_INDEX", N}, {0x77, "LENGTH", N}, {0x78, "LIST_APPEND", N},
    {0x79, "LIST_REMOVE", N}, {0x7a, "NEW_ARRAY_INIT", UU},
};

std::uint16_t u16(const std::vector<std::uint8_t>& code, std::size_t at) {
    return static_cast<std::uint16_t>(code.at(at) << 8 | code.at(at + 1));
}

std::int32_t i32(const std::vector<std::uint8_t>& code, std::size_t at) {
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v = v << 8 | code.at(at + k);
    std::int32_t out;
    std::memcpy(&out, &v, 4);
    return out;
}

} // namespace

std::string constantText(const hbc::Module& module, std::uint16_t index) {
    if (index >= module.constants.size()) return "#" + std::to_string(index);
    const auto& c = module.constants[index];
    char buf[64];
    if (auto* p = std::get_if<hbc::IntConstant>(&c)) return std::to_string(p->value);
    if (auto* p = std::get_if<hbc::RealConstant>(&c)) { std::snprintf(buf, sizeof buf, "%g", p->value); return buf; }
    if (auto* p = std::get_if<hbc::BoolConstant>(&c)) return p->value ? "true" : "false";
    if (auto* p = std::get_if<hbc::StringConstant>(&c)) return "\"" + p->value + "\"";
    if (auto* p = std::get_if<hbc::TimeConstant>(&c)) return std::to_string(p->milliseconds) + " ms";
    return "#" + std::to_string(index);
}

std::vector<Instruction> disassemble(const hbc::Module& module, const hbc::Function& function) {
    std::vector<Instruction> out;
    const auto& code = function.code;
    std::size_t ip = 0;
    while (ip < code.size()) {
        const std::uint8_t byte = code[ip];
        const Op* op = nullptr;
        for (const auto& o : OPS) if (o.code == byte) op = &o;
        Instruction ins;
        ins.ip = static_cast<std::uint32_t>(ip);
        if (!op) {
            ins.op = "DB";
            ins.args = std::to_string(byte);
            ins.size = 1;
        } else {
            ins.op = op->name;
            const std::size_t a = ip + 1;
            switch (op->kind) {
                case N: ins.size = 1; break;
                case U: {
                    const auto v = u16(code, a);
                    ins.size = 3;
                    if (byte == 0x01) ins.args = constantText(module, v);
                    else if (byte == 0x04 || byte == 0x05) ins.args = v < module.globals.size()
                        ? hbc::stringConstant(module, module.globals[v].nameConstant) : "#" + std::to_string(v);
                    else ins.args = std::to_string(v);
                    break;
                }
                case UU: {
                    const auto x = u16(code, a), y = u16(code, a + 2);
                    ins.size = 5;
                    if (byte == 0x50 || byte == 0x51) ins.args = hbc::stringConstant(module, x) + " (" + std::to_string(y) + " args)";
                    else ins.args = std::to_string(x) + ", " + std::to_string(y);
                    break;
                }
                case J: {
                    const auto d = i32(code, a);
                    ins.size = 5;
                    ins.args = "-> " + std::to_string(static_cast<std::int64_t>(ip) + 5 + d);
                    break;
                }
            }
        }
        out.push_back(ins);
        ip += ins.size;
    }
    return out;
}

} // namespace hope::vm
