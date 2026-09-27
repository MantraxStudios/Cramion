#include "CramionCore/net/NetworkObject.h"

// La definicion de ComponentRegistry::registerComponent<T>.
#include "CramionCore/ecs/World.h"

namespace cramion::net {

void NetworkObject::reflect(ecs::PropertyVisitor& v) {
    v.field({"sync_transform", "Sincronizar posición", "El dueño manda su posición y giro a los demás"}, sync_transform);
    v.field({"send_rate", "Envíos por segundo", "Cuántas veces por segundo se manda (solo si se movió)"}, send_rate,
            ecs::FloatRange{1.0f, 60.0f, 0.5f, "%.0f"});
    v.field({"smoothing", "Suavizado", "Lo rápido que los demás alcanzan la posición recibida (más = más pegado, menos = más suave)"},
            smoothing, ecs::FloatRange{1.0f, 60.0f, 0.5f, "%.0f"});
    v.field({"local_physics", "Física local en los demás",
             "Con Rigidbody dinámico: los demás jugadores también lo simulan y lo que manda el dueño lo corrige, así "
             "cualquiera lo puede empujar (balones, cajas). Apagado: en los demás solo sigue la posición recibida "
             "(personajes)"},
            local_physics);
}

void registerNetworkComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("NetworkObject") == nullptr) {
        registry.registerComponent<NetworkObject>("NetworkObject", "Objeto de red", "Scripting");
    }
}

}  // namespace cramion::net
