#include "runtime/Value.hpp"

#include <sstream>
#include <type_traits>

namespace hope::runtime {

std::string valueToString(const Value& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, Unit>) return "unit";
        else if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(item);
        else if constexpr (std::is_same_v<T, double>) {
            std::ostringstream out;
            out << item;
            return out.str();
        } else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return '"' + item + '"';
        else if constexpr (std::is_same_v<T, TimeValue>) return std::to_string(item.milliseconds) + "ms";
        else if constexpr (std::is_same_v<T, StructRef>) {
            if (!item) return "struct(null)";
            return "struct#" + std::to_string(item->structIndex) + " fields=" + std::to_string(item->fields.size());
        } else if constexpr (std::is_same_v<T, ArrayRef>) {
            if (!item) return "array(null)";
            return "array[len=" + std::to_string(item->elements.size()) + "]";
        } else {
            if (!item) return "list(null)";
            return "list[len=" + std::to_string(item->elements.size()) + "]";
        }
    }, value);
}

} // namespace hope::runtime
