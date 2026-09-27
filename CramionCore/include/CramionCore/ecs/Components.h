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
#include "CramionCore/ecs/RuntimeMesh.h"

#include <CramionFX/vk/PostProcessSettings.h>

#include <entt/entity/entity.hpp>

#include <memory>
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
    // "Static" de Unity: el objeto no se mueve, no se oculta ni cambia de
    // malla en el juego. Al exportar, las mallas estaticas se combinan en un
    // lote por escena (static batching, ver StaticBatching.h).
    bool is_static = false;
    // Su MeshRenderer ya lo dibuja el lote estatico de la escena: RenderSync
    // no lo dibuja aparte (la fisica y los scripts lo siguen viendo). Solo lo
    // pone la exportacion, en la copia de la escena que va al juego.
    bool static_batched = false;
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
    // Malla creada por codigo (RuntimeMesh.h; como MeshFilter.mesh de Unity):
    // si hay, se dibuja en lugar de `model`. No se guarda en la escena.
    std::shared_ptr<Mesh> mesh;

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

// Cinematica inversa (IK) sobre la pose animada de un modelo con esqueleto
// (va en la entidad del MeshRenderer, junto al Animator). Los objetivos son
// otras entidades de la escena: moverlas (a mano, por script, con fisica)
// mueve la mano, el pie o la mirada.
struct IKLimb {
    Uuid target;          // a donde llega la mano o el pie (vacio = no se usa)
    Uuid hint;            // hacia donde apunta el codo o la rodilla (opcional)
    float weight = 1.0f;  // 0 = la animacion, 1 = el objetivo
    bool match_rotation = false;  // la mano/el pie copia el giro del objetivo
    // Objetivo como un punto del mundo en vez de una entidad (Lua:
    // entity:setIKTarget("left_hand", Vec3(...))).
    bool use_position = false;
    core::Vec3 position{};
};
// Cadena de cualquier esqueleto (animales, colas, brazos roboticos,
// tentaculos): `bone` es el ultimo y `length` cuantos huesos por encima se
// doblan (2 = muslo y espinilla; 3 = las patas de un perro o un caballo; mas
// = cuellos y colas). Con `ground` el objetivo es el suelo bajo ese pie: pies
// de animales que se apoyan en escaleras y pendientes.
struct IKChain {
    std::string bone;
    int length = 2;
    Uuid target;
    Uuid hint;
    float weight = 1.0f;
    bool ground = false;
    bool match_rotation = false;
    bool use_position = false;  // objetivo = `position` (mundo)
    core::Vec3 position{};
};
struct InverseKinematics {
    bool enabled = true;
    // Humanoide (los huesos se encuentran solos).
    IKLimb left_hand;
    IKLimb right_hand;
    IKLimb left_foot;
    IKLimb right_foot;
    Uuid look_at;               // la cabeza (y un poco el cuello) mira aqui
    float look_weight = 1.0f;
    float look_max_angle = 70.0f;
    // Cualquier esqueleto: el hueso que mira (vacio = la cabeza del humanoide
    // o la que se detecta en un animal) y cuantos huesos hacia arriba
    // reparten el giro (cuellos largos: jirafa, dinosaurio, serpiente).
    std::string look_bone;
    int look_chain = 2;
    bool look_use_position = false;
    core::Vec3 look_position{};
    // Pies en el suelo: cada pie se apoya donde hay suelo (escaleras,
    // pendientes, rocas) y la cadera baja si hace falta para llegar.
    bool foot_grounding = false;
    float grounding_weight = 1.0f;
    float max_step = 0.5f;      // cuanto puede subir o bajar un pie (m)
    bool align_feet = true;     // el pie sigue la inclinacion del suelo
    // Animales: el cuerpo se inclina con el suelo bajo sus patas (cuesta
    // arriba, de lado) ademas de bajar la cadera. Usa las cadenas con suelo.
    bool align_body = true;
    float body_align_weight = 1.0f;
    std::vector<IKChain> chains;

    void reflect(PropertyVisitor& v);
};

// Animacion procedural (va con el MeshRenderer de un modelo con esqueleto,
// con o sin Animator): huesos con muelle (pelo, colas, capas), patas que dan
// pasos solas (arañas, robots) y capas de respirar, inclinarse y ruido.
struct SpringBoneChain {
    std::string bone;          // la raiz: se simulan ella y todos sus hijos
    float stiffness = 40.0f;   // vuelta a la pose (mas = mas rigido)
    float damping = 0.2f;      // mas = rebota menos
    float gravity = 4.0f;      // m/s²
    float radius = 0.03f;      // grosor para chocar (m)
};
struct ProceduralLeg {
    std::string bone;  // el pie (se usan su padre y su abuelo como en el IK)
    int group = -1;    // se turnan por grupos; -1 = alterna solo (0, 1, 0, 1...)
};
struct ProceduralNoise {
    std::string bone;
    float amplitude = 4.0f;   // grados
    float frequency = 0.4f;   // Hz
};
struct ProceduralAnimation {
    bool enabled = true;
    // Huesos con muelle.
    std::vector<SpringBoneChain> springs;
    bool body_colliders = true;  // chocan con la cabeza, el pecho y la cadera (humanoides)
    // Patas.
    std::vector<ProceduralLeg> legs;
    float step_distance = 0.35f;
    float step_height = 0.12f;
    float step_duration = 0.22f;
    float step_overshoot = 0.5f;
    bool adjust_body = true;
    float body_weight = 1.0f;
    // Capas (humanoides).
    bool breathing = false;
    float breath_rate = 14.0f;    // respiraciones por minuto
    float breath_amount = 2.5f;   // grados
    bool lean = false;
    float lean_amount = 10.0f;    // grados como mucho
    std::vector<ProceduralNoise> noise;

    void reflect(PropertyVisitor& v);
};

// --- Esqueleto: ver y mover los huesos -------------------------------------
// Va en la entidad del modelo (o en su raiz). En la Escena dibuja los huesos;
// en el Inspector se ven todos y cada uno se puede girar, mover o escalar
// encima de la animacion (como el "Transform (Modify) Bone" de Unreal).
struct BoneOverride {
    std::string bone;
    core::Vec3 rotation{};                  // grados, en los ejes del hueso (se suma a la animacion)
    core::Vec3 position{};                  // desplazamiento en los ejes del padre (unidades del modelo)
    core::Vec3 scale{1.0f, 1.0f, 1.0f};
    float weight = 1.0f;
};
struct Skeleton {
    bool show_bones = true;    // dibujar los huesos en la Escena (y en Play)
    bool show_names = false;
    float bone_size = 1.0f;    // grosor del dibujo
    std::string selected;      // hueso resaltado (el Inspector)
    std::vector<BoneOverride> bones;

    void reflect(PropertyVisitor& v);
};

// Engancha una entidad a un hueso del modelo de un antepasado (o hermano):
// Seguir = la entidad va con el hueso (una espada en la mano, un sombrero, un
// collider en la cabeza); Mover el hueso = el hueso sigue a la entidad
// (posar a mano con el gizmo, objetivos de animacion).
enum class SocketMode : int { Follow = 0, Drive = 1 };
struct BoneSocket {
    std::string bone;
    SocketMode mode = SocketMode::Follow;
    core::Vec3 position{};  // desplazamiento en los ejes del hueso (metros)
    core::Vec3 rotation{};  // grados
    bool drive_position = false;  // Mover el hueso: tambien su posicion (si no, solo el giro)
    float weight = 1.0f;

    void reflect(PropertyVisitor& v);
};

// --- Phys Bones (como los de VRChat): pelo, colas, orejas, faldas, capas ---
struct PhysBoneChain {
    std::string bone;           // la raiz: se simulan sus hijos
    std::string ignore;         // huesos que no (separados por comas)
    float pull = 0.2f;          // vuelve a la pose animada
    float spring = 0.2f;        // rebote (conserva la velocidad)
    float stiffness = 0.2f;     // rigidez directa
    float gravity = 0.0f;       // 0..1 de la gravedad
    float gravity_falloff = 0.0f;  // menos peso si ya cuelga
    float immobile = 0.0f;      // 1 = se mueve rigido con el personaje
    float max_angle = 0.0f;     // grados (0 = libre)
    float radius = 0.02f;       // metros, para chocar
    float radius_tip = -1.0f;   // en la punta (< 0 = igual)
    float end_length = 0.0f;    // punta extra (fraccion del ultimo hueso)
    bool collide = true;
};
struct PhysBones {
    bool enabled = true;
    std::vector<PhysBoneChain> chains;
    // Colliders que usan (entidades con PhysBoneCollider). Vacio = todos los
    // de la escena.
    std::vector<Uuid> colliders;

    void reflect(PropertyVisitor& v);
};

enum class PhysBoneColliderShape : int { Sphere = 0, Capsule = 1, Plane = 2 };
// Forma con la que chocan los Phys Bones (la cabeza, el cuerpo, el suelo).
// Ponla en una entidad con un Bone Socket para que siga a un hueso.
struct PhysBoneCollider {
    PhysBoneColliderShape shape = PhysBoneColliderShape::Sphere;
    float radius = 0.1f;
    float height = 0.3f;     // capsula: alto total en el eje Y de la entidad
    core::Vec3 offset{};     // centro (ejes de la entidad)
    bool inside = false;     // mantener dentro en vez de fuera

    void reflect(PropertyVisitor& v);
};

// --- Ragdoll ---------------------------------------------------------------
// Muneco de trapo con la fisica (Jolt): una capsula por hueso unidas por
// articulaciones con limites. Apagado sigue a la animacion; al activarlo
// (Lua: entity.ragdoll = true, al morir) cae con la velocidad que llevaba.
// Sin huesos en la lista se eligen solos (humanoides y animales).
struct RagdollRuntime;  // physics/Ragdoll.h
struct RagdollRuntimeRef {
    std::shared_ptr<RagdollRuntime> ptr;
    RagdollRuntimeRef() = default;
    // Copiar la entidad (duplicar, prefabs) no comparte la simulacion.
    RagdollRuntimeRef(const RagdollRuntimeRef&) {}
    RagdollRuntimeRef& operator=(const RagdollRuntimeRef&) { return *this; }
    RagdollRuntimeRef(RagdollRuntimeRef&&) noexcept = default;
    RagdollRuntimeRef& operator=(RagdollRuntimeRef&&) noexcept = default;
};
struct RagdollBoneSetting {
    std::string bone;
    float radius = 0.0f;   // metros (0 = automatico)
    float length = 0.0f;   // metros (0 = automatico)
    float mass = 0.0f;     // kg (0 = reparto automatico de la masa total)
    float swing = 40.0f;   // grados que se puede doblar
    float twist = 20.0f;   // grados que puede girar sobre si mismo
};
struct Ragdoll {
    bool active = false;           // simulando (true = cae)
    float mass = 70.0f;            // kg en total
    float blend = 1.0f;            // cuanto manda la fisica activo (0..1)
    float blend_out = 0.35f;       // segundos para volver a la animacion al apagarlo
    float friction = 0.7f;
    float damping = 0.05f;         // frenado del aire
    float joint_friction = 1.0f;   // rigidez de las articulaciones
    bool follow_entity = true;     // la entidad va con el cuerpo (camaras que la siguen)
    bool inherit_velocity = true;  // cae con la velocidad de la animacion
    std::vector<RagdollBoneSetting> bones;
    RagdollRuntimeRef runtime;     // no se guarda

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
    // Target Texture (como Unity): lo que ve esta camara va a esa Render
    // Texture (.crrt) en vez de a la pantalla.
    assets::AssetRef target_texture{{}, assets::AssetType::RenderTexture};

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

// Profiler (como el "stat fps / stat unit" de Unreal): muestra en una esquina
// de la pantalla del juego los FPS, el uso de CPU y de GPU y la memoria, con
// una grafica del tiempo de cada frame. Lo dibuja el juego exportado y la
// vista Juego del editor (ProfilerOverlay); basta uno en la escena.
enum class ProfilerCorner : int { TopRight = 0, TopLeft = 1, BottomRight = 2, BottomLeft = 3 };

struct Profiler {
    bool show_fps = true;
    bool show_cpu = true;
    bool show_gpu = true;
    bool show_memory = true;
    bool show_graph = true;
    ProfilerCorner corner = ProfilerCorner::TopRight;
    float scale = 1.0f;     // tamano del texto
    float opacity = 0.75f;  // del fondo

    void reflect(PropertyVisitor& v);
};

// Post-proceso y efectos de pantalla como el Volume de Unity:
//
//   - Global: vale en toda la escena.
//   - Caja / Esfera: solo cuando la camara esta dentro (con la posicion, el
//     giro y la escala de la entidad), con una transicion suave de
//     `blend_distance` metros al acercarse desde fuera.
//
// Se mezclan de menor a mayor prioridad (a igual prioridad, los globales
// antes): cada volumen lleva el resultado hacia sus valores segun su peso y
// lo cerca que esta la camara. Un volumen local solo cambia las secciones
// que sobrescribe (el resto sale de los globales); uno global, todas.
enum class PostVolumeShape : int { Global = 0, Box = 1, Sphere = 2 };

// Secciones del post-proceso que puede sobrescribir un volumen local.
enum PostOverride : std::uint32_t {
    kPostExposure = 1u << 0,
    kPostTonemapping = 1u << 1,
    kPostBloom = 1u << 2,
    kPostColor = 1u << 3,
    kPostVignette = 1u << 4,
    kPostLens = 1u << 5,
    kPostLightShafts = 1u << 6,
    kPostAntialiasing = 1u << 7,
    kPostEffects = 1u << 8,
    kPostPerformance = 1u << 9,
    kPostAll = (1u << 10) - 1u,
};

struct PostProcessing {
    int priority = 0;
    PostVolumeShape shape = PostVolumeShape::Global;
    core::Vec3 size{10.0f, 10.0f, 10.0f};  // caja, en el espacio de la entidad
    float radius = 5.0f;                    // esfera (por la escala mayor)
    float blend_distance = 2.0f;            // metros de transicion desde fuera
    float weight = 1.0f;                    // 0..1
    std::uint32_t overrides = 0;            // PostOverride (solo los locales)
    gfx::PostProcessSettings settings{};

    bool isGlobal() const { return shape == PostVolumeShape::Global; }
    // Cuanto cuenta en `point` (0..1): peso por cercania. `world` es la
    // matriz de su entidad.
    float influence(const core::Mat4& world, const core::Vec3& point) const;

    void reflect(PropertyVisitor& v);
};

// Lleva `out` hacia los valores de `volume` en las secciones de `mask` (t =
// 0..1). Los numeros y colores se interpolan; los interruptores y el modo de
// tonemapping cambian a la mitad.
void blendPostProcess(gfx::PostProcessSettings& out, const gfx::PostProcessSettings& volume, float t,
                      std::uint32_t mask);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_COMPONENTS_H
