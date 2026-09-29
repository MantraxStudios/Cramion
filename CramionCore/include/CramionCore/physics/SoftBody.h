#ifndef CRAMION_CORE_PHYSICS_SOFT_BODY_H
#define CRAMION_CORE_PHYSICS_SOFT_BODY_H

// Cuerpo blando (gelatina, pelota de goma, slime): una malla cerrada de
// particulas unidas por muelles, con presion dentro para conservar el
// volumen. La simula Jolt (soft body): cae, rebota, se aplasta, tiembla y
// vuelve a su forma; choca con los colliders y los rigidbodies y los empuja.
//
//   - Forma: esfera o cubo (redondeado al deformarse), de `size` metros; la
//     escala de la entidad la agranda.
//   - `stiffness` = lo firme que es la superficie (gelatina 0.3, goma 0.8);
//     `pressure` = cuanto se hincha por dentro (0 = se desinfla como un
//     trapo, 1 = aguanta su peso, 3+ = balon).
//   - La entidad va con el cuerpo (su centro), asi los scripts y las camaras
//     lo siguen.
//   - Se dibuja como las telas: un hueso por particula en el skinning por
//     GPU, con el .crmat del Mesh Renderer o con `color`.
//   - Dos cuerpos blandos no chocan entre si (Jolt), si con todo lo demas.
//
// En Lua: entity:resetSoftBody(), entity:addSoftBodyImpulse(Vec3) y los
// campos con getField/setField("SoftBody", ...).

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/physics/Cloth.h"  // ClothRuntime: el mismo estado compartido

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::asset {
struct ModelData;
}

namespace cramion::physics {

enum class SoftBodyShape : int { Sphere = 0, Cube = 1 };

using SoftBodyRuntime = ClothRuntime;

struct SoftBody {
    SoftBodyShape shape = SoftBodyShape::Sphere;
    core::Vec3 size{1.0f, 1.0f, 1.0f};  // metros (esfera: el diametro es size.x)
    int resolution = 8;                 // divisiones por lado de cada cara (mas = mas suave y mas caro)
    float mass = 1.0f;                  // kg
    float stiffness = 0.35f;            // 0 muy blando .. 1 casi rigido
    float pressure = 1.0f;              // 0 sin aire .. 5 muy hinchado
    float damping = 0.3f;               // frenado (mas = tiembla menos rato)
    float friction = 0.6f;
    float restitution = 0.0f;           // rebote extra al chocar
    float gravity_scale = 1.0f;
    float thickness = 0.01f;            // metros: separacion con lo que toca
    int iterations = 8;
    bool collide = true;
    bool follow_entity = true;          // la entidad va al centro del cuerpo
    // Aspecto (sin .crmat en el Mesh Renderer).
    core::Vec3 color{0.35f, 0.85f, 0.3f};
    float roughness = 0.15f;
    float metallic = 0.0f;

    ClothRuntimeRef runtime;  // no se guarda

    void reflect(ecs::PropertyVisitor& v);
    // Lo que obliga a rehacer el cuerpo (la malla o el aspecto).
    std::string layoutKey() const;
};

// La malla de la simulacion: particulas (en el espacio de la entidad) y
// triangulos cerrados hacia fuera; y para el render, cada vertice con su
// particula (las esquinas del cubo se reparten entre caras).
struct SoftBodyMesh {
    std::vector<core::Vec3> particles;
    std::vector<std::uint32_t> triangles;
    std::vector<core::Vec3> rest_normals;  // de cada particula (softBodyNormals en reposo)
};
SoftBodyMesh softBodyMesh(const SoftBody& body);
// Normal de cada particula: media de sus triangulos.
void softBodyNormals(const std::vector<std::uint32_t>& triangles, const std::vector<core::Vec3>& positions,
                     std::vector<core::Vec3>& normals);
asset::ModelData softBodyModel(const SoftBody& body);
// Matrices de los huesos (espacio de la entidad) para unas posiciones del mundo.
void softBodyBoneGlobals(const SoftBodyMesh& mesh, const core::Mat4& entity_world, const std::vector<core::Vec3>& world_positions,
                         std::vector<core::Mat4>& globals);

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_SOFT_BODY_H
