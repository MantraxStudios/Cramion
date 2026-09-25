#ifndef CRAMION_CORE_ECS_COMPONENTS_H
#define CRAMION_CORE_ECS_COMPONENTS_H

// Componentes del motor. Son datos planos (EnTT los guarda en pools de
// memoria contigua, uno por tipo); la logica esta en los sistemas (World,
// RenderSync). Cada uno describe sus propiedades en reflect() para el
// Inspector y los .crscene.
//
// Para anadir un componente nuevo:
//   1. un struct aqui (o en tu propio archivo) con void reflect(PropertyVisitor&)
//   2. registrarlo: ComponentRegistry::instance().registerComponent<MiComp>(
//          "MiComp", "Mi componente", "Categoria");
//      (los del motor se registran en registerBuiltinComponents, Components.cpp)
// Y listo: aparece en "Add Component", en el Inspector y se guarda en la escena.

#include "CramionCore/ecs/AnimatorController.h"
#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/vk/PostProcessSettings.h>

#include <entt/entity/entity.hpp>

#include <string>
#include <vector>

namespace cramion::ecs {

// --- Internos (no aparecen en el Inspector como componentes) ----------------

// Identidad persistente: la usan las referencias entre entidades y los .crscene.
struct IdComponent {
    Uuid uuid{};
};

struct NameComponent {
    std::string name;
};

// Estado de la entidad (como el GameObject de Unity).
struct EntityInfo {
    bool active = true;  // activeSelf: activa en la jerarquia solo si sus padres tambien
    std::string tag;
    int layer = 0;
};

// Jerarquia: padre e hijos EN ORDEN (el de la ventana Jerarquia).
struct Hierarchy {
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

// --- Transform ----------------------------------------------------------------

// Posicion/rotacion/escala locales (respecto al padre). La rotacion vive en
// un cuaternion; `euler` son los grados que muestra el Inspector (se guardan
// para que 0/360/-180 no salten al editar). Las matrices se cachean: solo se
// recalculan cuando algo de la cadena de padres cambio (marca `dirty`).
struct Transform {
    core::Vec3 position{};
    core::Quat rotation{};
    core::Vec3 scale{1.0f, 1.0f, 1.0f};
    core::Vec3 euler{};  // grados, orden YXZ

    // Cache (no se serializa). La mantiene World.
    mutable core::Mat4 local_matrix = core::Mat4::identity();
    mutable core::Mat4 world_matrix = core::Mat4::identity();
    mutable bool dirty = true;
    std::uint64_t version = 0;  // sube con cada cambio (para detectar movimiento)

    void reflect(PropertyVisitor& v);
    static void onChanged(World& world, entt::entity entity);
};

// --- Renderizado ------------------------------------------------------------

// Dibuja una pieza de un modelo importado (o una primitiva integrada).
// Como el "Cast Shadows" de Unity: Off, On o Shadows Only.
enum class ShadowCasting : int { Off = 0, On = 1, ShadowsOnly = 2 };

struct MeshRenderer {
    assets::AssetRef model{{}, assets::AssetType::Model};
    int part = 0;  // indice en ModelAsset::parts
    bool visible = true;
    ShadowCasting cast_shadows = ShadowCasting::On;
    // Materiales (.crmat) que sustituyen a los del modelo, por hueco (el
    // indice es el del material en el modelo). Vacio o invalido = el suyo.
    std::vector<assets::AssetRef> materials;

    void reflect(PropertyVisitor& v);
};

// Reproduce las animaciones de un modelo con esqueleto (va en la entidad del
// MeshRenderer animado).
// Con un Animator Controller (.cranimator) asignado, su maquina de estados
// elige el clip, la velocidad y el bucle; sin el, mandan clip/clip_name.
struct Animator {
    assets::AssetRef controller{{}, assets::AssetType::AnimatorController};
    int clip = 0;           // indice de la animacion (-1 = pose de reposo)
    std::string clip_name;  // si no esta vacio, manda sobre `clip` al cargar
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;
    float time = 0.0f;      // segundos (lo avanza RenderSync)

    // Estado de la maquina (no se guarda en la escena).
    AnimatorRuntime runtime;

    // Parametros del controlador (el juego o el editor los cambian).
    void setFloat(const std::string& name, float value) { runtime.values[name] = value; }
    void setInt(const std::string& name, int value) { runtime.values[name] = static_cast<float>(value); }
    void setBool(const std::string& name, bool value) { runtime.values[name] = value ? 1.0f : 0.0f; }
    void setTrigger(const std::string& name) { runtime.values[name] = 1.0f; }

    void reflect(PropertyVisitor& v);
};

enum class LightType : int { Directional = 0, Point = 1, Spot = 2 };

// Luz. La direccional fija el sol (su eje forward es la direccion de los
// rayos); las puntuales y los focos, sus posiciones y conos.
struct Light {
    LightType type = LightType::Point;
    core::Vec3 color{1.0f, 0.85f, 0.6f};
    float intensity = 12.0f;
    float range = 18.0f;          // metros (puntual y foco)
    float inner_angle = 14.0f;    // grados (foco)
    float outer_angle = 24.0f;
    bool cast_shadows = true;

    void reflect(PropertyVisitor& v);
};

// Camara de juego. La vista del editor tiene la suya.
struct Camera {
    float fov = 70.0f;  // grados, vertical
    float near_plane = 0.1f;
    float far_plane = 2000.0f;
    bool is_main = true;

    void reflect(PropertyVisitor& v);
};

// Cielo y hora: cielo HDR (asset) o fisico, nubes y ciclo de dia.
struct Sky {
    assets::AssetRef environment{{}, assets::AssetType::Environment};
    bool use_hdr = true;
    bool clouds = true;
    float time_of_day = 10.0f;  // horas (sin luz direccional ni HDR)
    bool day_cycle = false;

    void reflect(PropertyVisitor& v);
};

// Lluvia, charcos y zona inundada.
struct Weather {
    bool rain = true;
    float wetness = 0.5f;
    float puddles = 0.5f;
    bool flood = false;
    core::Vec2 flood_center{};
    core::Vec2 flood_radii{3.0f, 2.0f};

    void reflect(PropertyVisitor& v);
};

enum class DecalType : int { Stamp = 0, Puddle = 1, Wet = 2 };

// Decal (como el Decal Actor de Unreal): proyecta sobre todo lo que queda
// dentro de su caja (el cubo unidad escalado por el Transform) a lo largo de
// su eje Y local (hacia abajo). Tres tipos:
//   Estampa  una imagen o un color (grafitis, suciedad, marcas, logos)
//   Charco   agua acumulada local; se suma a los charcos de la lluvia global
//   Humedad  mancha mojada sin agua encima
struct Decal {
    DecalType type = DecalType::Stamp;
    std::string texture;  // imagen dentro de Assets/ (ruta relativa); vacia = solo color/forma
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    float amount = 1.0f;          // charco: nivel del agua; humedad: cuanto moja
    bool follow_rain = false;     // charco/humedad: solo con la lluvia global (escala con sus charcos)
    float edge_softness = 0.15f;  // 0..0.5 del tamano
    float max_angle = 60.0f;      // grados entre la superficie y el eje de proyeccion
    float roughness = 0.5f;       // estampa
    float roughness_amount = 0.0f;
    float metallic = 0.0f;

    void reflect(PropertyVisitor& v);
};

// Post-proceso y efectos de pantalla (como el Volume global de Unity). Si
// hay varios activos, manda el de mayor prioridad.
struct PostProcessing {
    int priority = 0;
    gfx::PostProcessSettings settings{};

    void reflect(PropertyVisitor& v);
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_COMPONENTS_H
