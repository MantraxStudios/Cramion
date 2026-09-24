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
    for (const ComponentType& type : types_) {
        if (type.name == name) {
            return &type;
        }
    }
    return nullptr;
}

}  // namespace cramion::ecs
