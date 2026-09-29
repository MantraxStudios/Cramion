#ifndef CRAMION_CORE_PHYSICS_CLOTH_H
#define CRAMION_CORE_PHYSICS_CLOTH_H

// Tela (como el Cloth de Unity o Chaos Cloth de Unreal): una rejilla de
// particulas unidas por muelles que simula Jolt (soft body). Cuelga, se
// arruga, ondea con el viento y choca con los colliders y los rigidbodies
// (y los empuja).
//
//   - La rejilla esta en el plano XY local de la entidad (mirando a +Z), de
//     `width` x `height` metros; la escala de la entidad la agranda. Para un
//     mantel, gira la entidad 90 grados en X.
//   - `pin` fija particulas (el borde de arriba de una cortina, el mastil de
//     una bandera): van pegadas a la entidad y la siguen al moverla.
//   - Se dibuja con el Mesh Renderer de la entidad (material .crmat,
//     sombras); sin material, con `color`. Cada particula es un "hueso" del
//     skinning por GPU, asi que sombras, motion blur y el culling funcionan
//     como con un personaje.
//   - Fuera de Play se ve estirada (la pose de reposo).
//
// En Lua: entity:resetCloth(), entity:addClothImpulse(Vec3) y los campos
// con getField/setField("Cloth", "wind", Vec3(...)).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::asset {
struct ModelData;
}

namespace cramion::physics {

enum class ClothPin : int {
    None = 0,         // cae entera
    TopEdge = 1,      // cortina
    TopCorners = 2,   // sabana tendida
    LeftEdge = 3,     // bandera en su mastil
    AllCorners = 4,   // hamaca, trampolin
    Center = 5,       // mantel sobre un poste
};

// Lo que la simulacion comparte con el render y con Lua (no se guarda).
struct ClothRuntime {
    std::vector<core::Vec3> positions;  // una por particula, en el mundo (interpolada)
    bool simulated = false;             // hay posiciones de la fisica
    std::uint64_t version = 0;          // sube con cada frame simulado
    // Pedidos desde Lua (la fisica los recoge en el siguiente paso).
    bool reset_requested = false;
    core::Vec3 pending_velocity{};      // cambio de velocidad para todas las particulas libres
};
struct ClothRuntimeRef {
    std::shared_ptr<ClothRuntime> ptr;
    ClothRuntimeRef() = default;
    // Duplicar la entidad no comparte la simulacion.
    ClothRuntimeRef(const ClothRuntimeRef&) {}
    ClothRuntimeRef& operator=(const ClothRuntimeRef&) { return *this; }
    ClothRuntimeRef(ClothRuntimeRef&&) noexcept = default;
    ClothRuntimeRef& operator=(ClothRuntimeRef&&) noexcept = default;
};

struct Cloth {
    float width = 2.0f;   // metros (X local)
    float height = 2.0f;  // metros (Y local)
    int segments_x = 20;  // cuadros por lado (particulas = (x+1) * (y+1))
    int segments_y = 20;
    ClothPin pin = ClothPin::TopEdge;
    float mass = 1.0f;          // kg de toda la tela
    float stiffness = 0.9f;     // 0 elastica .. 1 no se estira
    float bending = 0.3f;       // 0 se arruga libre .. 1 carton
    float damping = 0.2f;       // frenado del aire
    float friction = 0.5f;
    float thickness = 0.02f;    // metros: separacion con lo que toca
    float gravity_scale = 1.0f;
    int iterations = 6;         // mas = mas rigida y precisa (y mas cara)
    core::Vec3 wind{};          // m/s en el mundo
    float turbulence = 0.4f;    // 0 viento constante .. 1 rachas fuertes
    float air_drag = 1.0f;      // cuanto la empuja el viento
    bool collide = true;        // choca con los colliders (si no, atraviesa todo)
    // Aspecto (sin .crmat en el Mesh Renderer).
    core::Vec3 color{0.75f, 0.12f, 0.12f};
    float roughness = 0.85f;
    bool double_sided = true;   // se ve por las dos caras

    ClothRuntimeRef runtime;    // no se guarda

    void reflect(ecs::PropertyVisitor& v);

    int particlesX() const;
    int particlesY() const;
    int particleCount() const;
    // Lo que obliga a rehacer la tela (la rejilla o el aspecto).
    std::string layoutKey() const;
};

// Posicion de reposo de cada particula (en el espacio local de la entidad).
std::vector<core::Vec3> clothRestPositions(const Cloth& cloth);
// true si la particula esta fijada a la entidad.
bool clothPinned(const Cloth& cloth, int x, int y);
// Triangulos de la rejilla (antihorarios vistos desde +Z).
std::vector<std::uint32_t> clothTriangles(const Cloth& cloth);
// Normal de cada particula a partir de sus vecinas (posiciones del mundo o
// locales, las que se den).
void clothNormals(const Cloth& cloth, const std::vector<core::Vec3>& positions, std::vector<core::Vec3>& normals);

// Modelo para el render: la rejilla (y su cara de atras si es de doble cara)
// con un hueso por particula. La pose de reposo es la de clothRestPositions.
asset::ModelData clothModel(const Cloth& cloth);
// Matriz de cada hueso (espacio del modelo = el de la entidad) para unas
// posiciones del mundo: la particula en su sitio y girada con la tela.
void clothBoneGlobals(const Cloth& cloth, const core::Mat4& entity_world, const std::vector<core::Vec3>& world_positions,
                      std::vector<core::Mat4>& globals);

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_CLOTH_H
