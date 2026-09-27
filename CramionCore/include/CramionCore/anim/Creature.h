#ifndef CRAMION_CORE_ANIM_CREATURE_H
#define CRAMION_CORE_ANIM_CREATURE_H

// Esqueletos de cualquier criatura (perros, caballos, dinosaurios, aranas,
// pajaros... y tambien humanos): encuentra solo sus patas, cabeza, cuello,
// cola y cuerpo para configurar el IK, los pies en el suelo y el ragdoll.
//
// Primero por los nombres de los huesos (foot, paw, hoof, head, neck, tail,
// pata, cabeza, cola... con lado L/R y delante/detras) y, si no, por la
// forma del esqueleto en reposo: un pie es el primer hueso de una rama que
// baja cerca del suelo; la rama empieza donde el cuerpo se divide (cadera u
// hombros). La cabeza es la punta mas alta que no es pata ni cola y el
// "delante" va del cuerpo a la cabeza.

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <string>
#include <vector>

namespace cramion::creature {

struct Leg {
    int foot = -1;     // el ultimo hueso (tobillo, pezuna, zarpa)
    int bones = 2;     // huesos por encima del pie que dobla el IK (2 = muslo y espinilla; 3 = patas de perro/caballo)
    bool front = false;
    bool left = false;
};

struct Rig {
    bool valid = false;
    core::Vec3 up{0.0f, 1.0f, 0.0f};  // espacio del modelo
    core::Vec3 forward{0.0f, 0.0f, 1.0f};
    core::Vec3 right{1.0f, 0.0f, 0.0f};
    float height = 1.0f;               // alto en reposo (unidades del modelo)
    int body = -1;                     // donde se unen las patas (cadera / columna)
    std::vector<Leg> legs;
    int head = -1;
    std::vector<int> neck;             // del de abajo a la cabeza (sin ella)
    std::vector<int> tail;             // de la base a la punta
    std::vector<int> spine;            // del cuerpo al cuello
};

// `rest`: globales de reposo (humanoid::restGlobals).
Rig detect(const std::vector<asset::Node>& nodes, const std::vector<core::Mat4>& rest);

// --- Ragdoll -------------------------------------------------------------------

struct RagdollBone {
    int node = -1;
    int parent = -1;       // indice en la lista (-1 = la raiz)
    int tip = -1;          // nodo donde acaba la capsula (el hijo de la lista que sigue al hueso)
    float length = 0.0f;   // modelo
    float radius = 0.0f;   // modelo
    float mass = 1.0f;     // fraccion de la masa total (suman 1)
    float swing = 40.0f;   // grados
    float twist = 20.0f;   // grados (a cada lado)
};

// Huesos del ragdoll de un esqueleto: los grandes del humanoide o, en otras
// criaturas, cuerpo, columna, cuello, cabeza, patas y cola (sin huesos
// diminutos ni ayudantes). Padres antes que hijos. Como mucho `max_bones`.
std::vector<RagdollBone> ragdollBones(const std::vector<asset::Node>& nodes, const std::vector<core::Mat4>& rest,
                                      int max_bones = 24);

// El largo de un hueso en reposo: hasta su primer hijo (o `fallback`).
float boneLength(const std::vector<asset::Node>& nodes, const std::vector<core::Mat4>& rest, int node, float fallback);

// Nombre en minusculas sin prefijos de programa ("mixamorig:", "Bip01 ").
std::string cleanName(const std::string& name);

}  // namespace cramion::creature

#endif  // CRAMION_CORE_ANIM_CREATURE_H
