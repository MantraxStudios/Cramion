#include "CramionCore/net/NetworkObject.h"

// La definicion de ComponentRegistry::registerComponent<T>.
#include "CramionCore/ecs/World.h"

#include <array>

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
    static constexpr std::array<const char*, 3> kModes = {"Suavizado", "Interpolación con búfer", "Extrapolación"};
    v.enumeration({"interpolation", "Seguimiento",
                   "Cómo siguen los demás lo que manda el dueño. Interpolación: va un poco por detrás y pasa por todas "
                   "las posiciones (exacto). Extrapolación: adelanta con la velocidad (menos retraso)"},
                  interpolation, kModes);
    v.field({"interpolation_delay", "Retraso del búfer", "Segundos por detrás del dueño (unos 2 envíos)"},
            interpolation_delay, ecs::FloatRange{0.02f, 1.0f, 0.005f, "%.3f s"});
    v.field({"relevance", "Relevancia", "El servidor solo lo manda a los jugadores a menos de estos metros (0 = a todos)"},
            relevance, ecs::FloatRange{0.0f, 10000.0f, 1.0f, "%.0f m"});
    v.field({"max_speed", "Velocidad máxima", "El servidor rechaza (y corrige) movimientos más rápidos: anti-trampas (0 = no)"},
            max_speed, ecs::FloatRange{0.0f, 1000.0f, 0.5f, "%.1f m/s"});
}

void registerNetworkComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("NetworkObject") == nullptr) {
        registry.registerComponent<NetworkObject>("NetworkObject", "Objeto de red", "Scripting");
    }
}

}  // namespace cramion::net
