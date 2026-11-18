// Orden de ejecucion de los scripts de C++ (ScriptOrder.h).

#include "CramionCore/scripting/ScriptOrder.h"

#include <cctype>
#include <charconv>

namespace cramion::scripting {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

std::unordered_map<std::string, int> parseExecutionOrder(std::string_view text) {
    std::unordered_map<std::string, int> out;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t end = text.find_first_of(";,\n\r", i);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view item = trim(text.substr(i, end - i));
        i = end + 1;
        if (item.empty()) continue;
        const std::size_t sep = item.find_first_of("=:");
        if (sep == std::string_view::npos) continue;
        const std::string_view name = trim(item.substr(0, sep));
        std::string_view number = trim(item.substr(sep + 1));
        if (name.empty() || number.empty()) continue;
        if (number.front() == '+') number.remove_prefix(1);
        int value = 0;
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) continue;
        out[std::string(name)] = value;
    }
    return out;
}

}  // namespace cramion::scripting
