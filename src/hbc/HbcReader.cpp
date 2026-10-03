#include "hbc/HbcReader.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace hope::hbc {
namespace {

class BinaryReader {
public:
    explicit BinaryReader(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    std::size_t remaining() const { return bytes_.size() - pos_; }

    std::uint8_t u8() {
        require(1);
        return bytes_[pos_++];
    }

    std::uint16_t u16() {
        require(2);
        const auto value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes_[pos_]) << 8) |
            static_cast<std::uint16_t>(bytes_[pos_ + 1]));
        pos_ += 2;
        return value;
    }

    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | bytes_[pos_ + i];
        pos_ += 4;
        return value;
    }

    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8) | bytes_[pos_ + i];
        pos_ += 8;
        return value;
    }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    double f64() {
        const auto bits = u64();
        return std::bit_cast<double>(bits);
    }

    std::vector<std::uint8_t> bytes(std::size_t count) {
        require(count);
        std::vector<std::uint8_t> out(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                      bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + count));
        pos_ += count;
        return out;
    }

    std::string utf8(std::size_t count) {
        const auto raw = bytes(count);
        return std::string(raw.begin(), raw.end());
    }

private:
    void require(std::size_t count) const {
        if (count > remaining()) throw FormatError("Truncated HBC file");
    }

    std::vector<std::uint8_t> bytes_;
    std::size_t pos_{};
};

Constant readConstant(BinaryReader& in) {
    switch (in.u8()) {
        case 1: return IntConstant{in.i64()};
        case 2: return RealConstant{in.f64()};
        case 3: {
            const auto value = in.u8();
            if (value > 1) throw FormatError("Invalid boolean constant");
            return BoolConstant{value == 1};
        }
        case 4: return StringConstant{in.utf8(in.u16())};
        case 5: return TimeConstant{in.i64()};
        default: throw FormatError("Unknown constant tag");
    }
}

TypePtr readType(BinaryReader& in, int depth = 0) {
    if (depth >= 64) throw FormatError("Type nesting exceeds 64 levels");
    auto type = std::make_shared<Type>();
    switch (in.u8()) {
        case 1: type->value = IntType{}; break;
        case 2: type->value = RealType{}; break;
        case 3: type->value = BoolType{}; break;
        case 4: type->value = StringType{}; break;
        case 5: type->value = TimeType{}; break;
        case 6: type->value = StructType{in.utf8(in.u16())}; break;
        case 7: {
            const auto size = in.i32();
            if (size < 0) throw FormatError("Negative array size");
            type->value = ArrayType{readType(in, depth + 1), size};
            break;
        }
        case 8: type->value = ListType{readType(in, depth + 1)}; break;
        default: throw FormatError("Unknown type tag");
    }
    return type;
}

std::vector<std::uint8_t> loadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw FormatError("Cannot open HBC file: " + path.string());
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) throw FormatError("Cannot determine HBC file size");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) throw FormatError("Cannot read HBC file");
    return bytes;
}

} // namespace

Module Reader::readFile(const std::filesystem::path& path) {
    BinaryReader in(loadBytes(path));
    const auto magic = in.bytes(4);
    if (magic != std::vector<std::uint8_t>{'H','B','C',0}) throw FormatError("Not an HBC file");

    Module module;
    module.version = in.u16();
    if (module.version != 1) throw FormatError("Unsupported HBC version: " + std::to_string(module.version));

    const auto constantCount = in.u16();
    module.constants.reserve(constantCount);
    for (std::uint16_t i = 0; i < constantCount; ++i) module.constants.push_back(readConstant(in));

    const auto functionCount = in.u16();
    module.functions.reserve(functionCount);
    for (std::uint16_t i = 0; i < functionCount; ++i) {
        Function function;
        function.nameConstant = in.u16();
        function.parameterCount = in.u16();
        function.localCount = in.u16();
        const auto codeSize = in.u32();
        if (codeSize > in.remaining()) throw FormatError("Invalid function bytecode size");
        function.code = in.bytes(codeSize);
        module.functions.push_back(std::move(function));
    }

    if (in.remaining() == 0) return module;

    const auto marker = in.bytes(4);
    if (marker != std::vector<std::uint8_t>{'M','E','T','A'}) throw FormatError("Unknown HBC metadata section");
    module.metadataPresent = true;

    const auto typeCount = in.u16();
    module.types.reserve(typeCount);
    for (std::uint16_t i = 0; i < typeCount; ++i) module.types.push_back(readType(in));

    const auto structCount = in.u16();
    module.structs.reserve(structCount);
    for (std::uint16_t i = 0; i < structCount; ++i) {
        StructDef def;
        def.nameConstant = in.u16();
        const auto fieldCount = in.u16();
        def.fields.reserve(fieldCount);
        for (std::uint16_t j = 0; j < fieldCount; ++j) def.fields.push_back(Field{in.u16(), in.u16()});
        module.structs.push_back(std::move(def));
    }

    const auto globalCount = in.u16();
    module.globals.reserve(globalCount);
    for (std::uint16_t i = 0; i < globalCount; ++i) {
        const auto name = in.u16();
        const auto type = in.u16();
        const auto initializer = in.u16();
        const auto mutableFlag = in.u8();
        if (mutableFlag > 1) throw FormatError("Invalid global mutability");
        module.globals.push_back(GlobalDef{name, type, initializer, mutableFlag == 1});
    }

    const auto eventCount = in.u16();
    module.events.reserve(eventCount);
    for (std::uint16_t i = 0; i < eventCount; ++i) {
        EventDef event;
        event.nameConstant = in.u16();
        const auto paramCount = in.u16();
        event.parameterTypes.reserve(paramCount);
        for (std::uint16_t j = 0; j < paramCount; ++j) event.parameterTypes.push_back(in.u16());
        module.events.push_back(std::move(event));
    }

    const auto handlerCount = in.u16();
    module.handlers.reserve(handlerCount);
    for (std::uint16_t i = 0; i < handlerCount; ++i) {
        HandlerDef handler;
        const auto tag = in.u8();
        if (tag < 1 || tag > 4) throw FormatError("Unknown handler kind");
        handler.kind = static_cast<HandlerKind>(tag);
        handler.functionIndex = in.u16();
        if (handler.kind == HandlerKind::Event) handler.eventIndex = in.u16();
        if (handler.kind == HandlerKind::Every || handler.kind == HandlerKind::At) handler.milliseconds = in.i64();
        module.handlers.push_back(handler);
    }

    if (in.remaining() != 0) throw FormatError("Trailing bytes in HBC file");
    return module;
}

std::string stringConstant(const Module& module, std::uint16_t index) {
    if (index >= module.constants.size()) throw FormatError("String constant index out of range");
    const auto* value = std::get_if<StringConstant>(&module.constants[index]);
    if (!value) throw FormatError("Expected string constant");
    return value->value;
}

std::string constantToString(const Constant& constant) {
    return std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, IntConstant>) return "int " + std::to_string(value.value);
        else if constexpr (std::is_same_v<T, RealConstant>) {
            std::ostringstream out; out << "real " << value.value; return out.str();
        } else if constexpr (std::is_same_v<T, BoolConstant>) return std::string("bool ") + (value.value ? "true" : "false");
        else if constexpr (std::is_same_v<T, StringConstant>) return "string \"" + value.value + "\"";
        else return "time " + std::to_string(value.milliseconds) + "ms";
    }, constant);
}

std::string typeToString(const TypePtr& type) {
    return std::visit([&](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, IntType>) return "int";
        else if constexpr (std::is_same_v<T, RealType>) return "real";
        else if constexpr (std::is_same_v<T, BoolType>) return "bool";
        else if constexpr (std::is_same_v<T, StringType>) return "string";
        else if constexpr (std::is_same_v<T, TimeType>) return "time";
        else if constexpr (std::is_same_v<T, StructType>) return value.name;
        else if constexpr (std::is_same_v<T, ArrayType>) return typeToString(value.element) + "[" + std::to_string(value.size) + "]";
        else return "list<" + typeToString(value.element) + ">";
    }, type->value);
}

} // namespace hope::hbc
