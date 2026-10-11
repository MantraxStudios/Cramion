// Autocompletado del editor de codigo (CodeCompletion.h).
#include "CodeCompletion.h"

#include <CramionCore/scripting/NativeApi.h>
#include <CramionCore/scripting/Scripting.h>

#include <cctype>

namespace cramion::editor {

namespace {

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

std::vector<CodeCompletion> filterCompletions(const std::vector<CodeCompletion>& raw, const std::string& prefix) {
    const std::string low = lower(prefix);
    std::vector<CodeCompletion> first, second;
    for (const CodeCompletion& c : raw) {
        const std::size_t at = lower(c.label).find(low);
        if (!low.empty() && at == std::string::npos) continue;
        (at == 0 ? first : second).push_back(c);
    }
    first.insert(first.end(), second.begin(), second.end());
    if (first.size() > 200) first.resize(200);
    return first;
}

bool apiDoc(const std::string& callee, std::string& args, std::string& description, bool& function) {
    namespace api = scripting::api;
    const api::NativeApi& registry = scripting::ScriptSystem::apiRegistry();
    const api::Entry* entry = registry.find(callee);
    // Los de un tipo se registran con ':' ("Entity:position"), tambien las propiedades.
    if (entry == nullptr) {
        if (const std::size_t dot = callee.find('.'); dot != std::string::npos) {
            entry = registry.find(callee.substr(0, dot) + ":" + callee.substr(dot + 1));
        }
    }
    if (entry == nullptr) return false;
    args = entry->doc.args;
    description = entry->doc.description;
    function = entry->kind != api::Entry::Kind::Property;
    return true;
}

}  // namespace cramion::editor
