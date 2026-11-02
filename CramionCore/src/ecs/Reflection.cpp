#include "CramionCore/ecs/Reflection.h"

#include "CramionCore/ecs/World.h"

namespace cramion::ecs {

ComponentRegistry& ComponentRegistry::instance() {
    // Se registran la primera vez que se pide: no dependemos del orden de
    // inicializacion de estaticos entre archivos.
    static ComponentRegistry registry = [] {
        ComponentRegistry r;
        registerBuiltinComponents(r);
        return r;
    }();
    return registry;
}

const ComponentType* ComponentRegistry::find(std::string_view name) const {
    const int index = indexOf(name);
    return index >= 0 ? &types_[static_cast<std::size_t>(index)] : nullptr;
}

int ComponentRegistry::indexOf(std::string_view name) const {
    const auto it = index_.find(name);
    return it != index_.end() ? static_cast<int>(it->second) : -1;
}

}  // namespace cramion::ecs
