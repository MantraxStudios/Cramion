#ifndef CRAMION_CORE_NET_NETWORK_OBJECT_H
#define CRAMION_CORE_NET_NETWORK_OBJECT_H

// Objeto de red: una entidad creada con Network.spawn existe en todos los
// jugadores con el mismo id de red. Su dueno (el jugador que la controla, o el
// servidor) manda su posicion y giro; los demas la siguen suavizando. Se puede
// poner en un prefab para ajustar la frecuencia y el suavizado; si no lo
// lleva, Network.spawn lo anade con los valores por defecto.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <vector>

namespace cramion::net {

struct NetworkObject {
    bool sync_transform = true;  // mandar la posicion y el giro del dueno
    float send_rate = 20.0f;     // veces por segundo (solo si se movio)
    float smoothing = 15.0f;     // lo rapido que los demas alcanzan la posicion recibida
    // Con Rigidbody dinamico: los demas tambien lo simulan (prediccion) y lo
    // que llega del dueno lo va corrigiendo, asi cualquiera lo empuja sin
    // esperar a la red (balones, cajas). Apagado: en los demas es cinematico
    // y solo sigue la posicion recibida (personajes).
    bool local_physics = false;
    // Como siguen los demas lo que manda el dueno: 0 suavizado (persigue la
    // ultima posicion), 1 interpolacion con bufer (va `interpolation_delay`
    // por detras y pasa por todas las posiciones recibidas: movimiento
    // exacto aunque lleguen a saltos), 2 extrapolacion (adelanta con la
    // velocidad: menos retraso, se pasa en los giros bruscos).
    int interpolation = 1;
    float interpolation_delay = 0.1f;  // s (unos 2 envios a 20 Hz)
    float relevance = 0.0f;            // m: el servidor solo lo manda a quien este cerca (0 = a todos)
    float max_speed = 0.0f;            // m/s: el servidor rechaza movimientos mas rapidos (0 = no mira)

    // En Play (no se guarda).
    struct Snapshot {
        double time = 0.0;  // hora local equivalente
        core::Vec3 position{};
        core::Quat rotation{};
    };
    std::vector<Snapshot> snapshots;  // interpolacion con bufer
    double clock_offset = 0.0;        // hora local - hora del dueno (estimada)
    bool has_offset = false;
    std::uint32_t net_id = 0;  // 0 = no creado por Network.spawn
    std::uint32_t owner = 0;
    bool has_target = false;
    bool predicted = false;          // copia de otro simulada aqui (local_physics)
    core::Vec3 target_position{};
    core::Quat target_rotation{};
    core::Vec3 target_velocity{};    // estimada entre las dos ultimas posiciones recibidas
    float target_age = 0.0f;         // segundos desde la ultima posicion recibida
    float send_timer = 0.0f;
    float since_sent = 0.0f;
    core::Vec3 last_position{};
    core::Quat last_rotation{};

    void reflect(ecs::PropertyVisitor& v);
};

void registerNetworkComponents();

}  // namespace cramion::net

#endif  // CRAMION_CORE_NET_NETWORK_OBJECT_H
