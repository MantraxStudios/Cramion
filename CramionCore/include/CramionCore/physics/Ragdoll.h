#ifndef CRAMION_CORE_PHYSICS_RAGDOLL_H
#define CRAMION_CORE_PHYSICS_RAGDOLL_H

// Lo que comparten la animacion (RenderSync) y la fisica (PhysicsSystem)
// sobre el ragdoll de una entidad (componente ecs::Ragdoll):
//
//   RenderSync, cada frame: que huesos lleva (resueltos en el esqueleto) y
//   donde los pone la animacion en el mundo (y donde estaban el frame
//   anterior: la velocidad con la que cae).
//   PhysicsSystem: al activarlo crea una capsula por hueso y sus
//   articulaciones; mientras simula escribe la pose de cada hueso.
//   RenderSync pone esa pose en el esqueleto (y al apagarlo vuelve a la
//   animacion mezclando durante `blend_out`).
//
// Todo en el espacio del mundo.

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cramion::ecs {

struct RagdollRuntime {
    struct Bone {
        std::string name;
        int node = -1;
        int parent = -1;             // indice en `bones` (-1 = la raiz)
        float radius = 0.05f;        // metros
        float mass = 1.0f;           // kg
        float swing = 40.0f;         // grados
        float twist = 20.0f;         // grados
        // La animacion: el hueso en el mundo (con su escala) y donde acaba la
        // capsula (su hijo).
        core::Mat4 world = core::Mat4::identity();
        core::Mat4 previous = core::Mat4::identity();
        core::Vec3 tip{};
        // La fisica: la pose del hueso (origen y giro, sin escala).
        core::Vec3 sim_position{};
        core::Quat sim_rotation{};
    };
    std::vector<Bone> bones;
    std::uint64_t signature = 0;  // configuracion con la que se hicieron `bones`
    bool pose_ready = false;      // RenderSync ya escribio una pose
    bool has_previous = false;
    float pose_dt = 1.0f / 60.0f; // segundos entre `previous` y `world`

    bool simulating = false;      // hay cuerpos en la fisica
    bool sim_ready = false;       // `sim_*` validos
    float blend_out = 0.0f;       // segundos que faltan para volver a la animacion
    float blend_out_total = 0.0f;

    // Empujones pedidos (Lua: entity:addRagdollForce): la fisica los aplica.
    struct Push {
        int bone = -1;            // -1 = el mas cercano a `point` o la raiz
        core::Vec3 impulse{};
        core::Vec3 point{};
        bool at_point = false;
    };
    std::vector<Push> pushes;
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_PHYSICS_RAGDOLL_H
