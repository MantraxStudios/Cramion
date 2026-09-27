#ifndef CRAMION_CORE_ANIM_HUMANOID_H
#define CRAMION_CORE_ANIM_HUMANOID_H

// Humanoides (como el Avatar de Unity):
//
//   - detectHumanoid: encuentra en un esqueleto los huesos de un humano
//     (cadera, columna, cabeza, brazos, piernas) por sus nombres, sean de
//     Mixamo (mixamorig:LeftUpLeg), Unreal (thigh_l), Blender (thigh.L),
//     3ds Max Biped (Bip01 L Thigh) o nombres sueltos (UpperArm_Left...).
//   - retargetClip: pasa una animacion de un humanoide a otro con otros
//     nombres, proporciones, ejes y pose de reposo (T o A). Cada hueso copia
//     el GIRO que hace el de origen respecto a su reposo, medido en el
//     "espacio del personaje" (arriba, derecha, delante sacados del
//     esqueleto), no en los ejes de cada hueso: por eso da igual como los
//     orientara cada programa. La cadera se desplaza escalada por la altura.

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <array>
#include <string>
#include <vector>

namespace cramion::humanoid {

enum class Bone : int {
    Hips = 0,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
    Count
};
inline constexpr int kBoneCount = static_cast<int>(Bone::Count);
const char* boneName(Bone bone);

// Nodo del esqueleto (indice en ModelData::nodes) de cada hueso humano; -1 si
// no tiene (hombros, pecho alto, dedos del pie son opcionales).
struct Map {
    std::array<int, kBoneCount> nodes{};
    bool valid = false;  // tiene todos los obligatorios: es humanoide
    int operator[](Bone b) const { return nodes[static_cast<std::size_t>(b)]; }
    int& operator[](Bone b) { return nodes[static_cast<std::size_t>(b)]; }
};

Map detect(const std::vector<asset::Node>& nodes);
inline bool isHumanoid(const std::vector<asset::Node>& nodes) { return detect(nodes).valid; }

// Transformaciones de reposo en el espacio del modelo (padres antes que
// hijos, como ModelData::nodes).
std::vector<core::Mat4> restGlobals(const std::vector<asset::Node>& nodes);

// Ejes del personaje en reposo (espacio del modelo, unitarios): su derecha,
// arriba y delante (hacia donde mira).
void characterAxes(const Map& map, const std::vector<core::Mat4>& rest, core::Vec3& right, core::Vec3& up,
                   core::Vec3& forward);

// `clip` anima `source` (sus canales apuntan a nodos de `source`). Devuelve
// en `out` el clip para `target` (canales a nodos de `target`), muestreado a
// `fps`. false (con el motivo) si alguno no es humanoide.
bool retargetClip(const std::vector<asset::Node>& source, const asset::AnimationClip& clip,
                  const std::vector<asset::Node>& target, asset::AnimationClip& out, std::string* error = nullptr,
                  float fps = 30.0f);

}  // namespace cramion::humanoid

#endif  // CRAMION_CORE_ANIM_HUMANOID_H
