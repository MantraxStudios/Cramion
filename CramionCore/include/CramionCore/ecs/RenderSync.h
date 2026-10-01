#ifndef CRAMION_CORE_ECS_RENDER_SYNC_H
#define CRAMION_CORE_ECS_RENDER_SYNC_H

// Puente entre el mundo (ECS) y CramionFX: cada frame lleva las entidades a
// scene::Scene (modelos, actores, luces, sol) y los componentes de entorno al
// VulkanRenderer (cielo, clima, post-proceso).
//
// Es el unico dueño de la scene::Scene que recibe: al empezar a usarlo (o al
// cambiar de escena/proyecto) llamar a reset().
//
// Orden por frame:
//   scene.update(input, dt);        // camara del editor, sol
//   sync.sync(world, scene, renderer, dt);
//   renderer.drawFrame(scene);

#include "CramionCore/anim/Inertialization.h"
#include "CramionCore/asset/SurfaceShader.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/ecs/AnimatorController.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/anim/Humanoid.h"
#include "CramionCore/anim/Procedural.h"
#include "CramionCore/anim/Creature.h"
#include "CramionCore/anim/PhysBones.h"
#include "CramionCore/water/Ripples.h"
#include "CramionCore/water/Water.h"

#include <filesystem>
#include <functional>
#include <CramionFX/CramionFX.h>

#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cramion::physics {
struct Cloth;
struct SoftBody;
struct SoftBodyMesh;
}

namespace cramion::ecs {

class RenderSync {
public:
    struct Options {
        // La vista usa la Camera principal del mundo (vista de juego). Si es
        // false (vista de escena del editor), la camara de scene::Scene no se
        // toca.
        bool apply_main_camera = false;
        // Dibujar las camaras con Target Texture (Render Textures). La segunda
        // vista del editor no (ya lo hizo la principal este frame).
        bool render_textures = true;
    };

    explicit RenderSync(assets::AssetManager& assets) : assets_(assets) {}

    // Id de una Render Texture en el renderizador (la crea si hace falta; -1
    // si no existe): la vista previa del editor.
    std::int32_t renderTextureId(const Uuid& uuid) { return renderTextureFor(uuid); }
    // Por su ruta dentro de Assets (el hueco de un material).
    std::int32_t renderTextureIdForAsset(const std::string& relative);
    ~RenderSync();
    RenderSync(const RenderSync&) = delete;
    RenderSync& operator=(const RenderSync&) = delete;

    // Datos de los terrenos (el editor o el juego los comparte con la fisica).
    void setTerrainStore(terrain::TerrainStore* store) { terrain_store_ = store; }

    void sync(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer, float delta_seconds,
              const Options& options);
    void sync(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer, float delta_seconds) {
        sync(world, scene, renderer, delta_seconds, Options{});
    }

    // Vacia la escena de CramionFX y olvida los modelos subidos (y los
    // descarga del AssetManager para poder volver a leerlos).
    void reset(scene::Scene& scene);

    // --- Correspondencia entidad <-> actor (outline y seleccion por clic) ---
    // Indice en scene.actors() del MeshRenderer de la entidad, o -1.
    int actorIndex(Entity entity) const;
    Entity entityForActor(const World& world, std::uint32_t actor) const;
    // Actores de la entidad y de todos sus descendientes (para el outline de
    // una seleccion con hijos).
    std::vector<std::uint32_t> actorIndicesInSubtree(Entity entity) const;

    // Datos de la pieza que dibuja la entidad (esqueleto y clips), o nullptr.
    // Suelo bajo un punto (IK de pies): rayo en el mundo que no cuenta a `self`
    // ni a su jerarquia. La pone el editor/el juego con su fisica.
    using GroundQuery = std::function<bool(const core::Vec3& origin, const core::Vec3& direction, float max_distance,
                                           core::Vec3& point, core::Vec3& normal, Entity self)>;
    void setGroundQuery(GroundQuery query) { ground_query_ = std::move(query); }

    const asset::ModelData* actorModelData(Entity entity, const scene::Scene& scene) const;

    // Animator Controllers (.cranimator) en uso: se leen una vez; el editor
    // llama a reload... al guardarlos para que el cambio se vea al momento.
    std::shared_ptr<const AnimatorController> animatorController(const Uuid& uuid);
    void reloadAnimatorController(const Uuid& uuid);

    // Materiales (.crmat) en uso: se leen una vez. El editor llama a
    // reloadMaterial() al guardarlos: los colores y factores cambian en vivo;
    // texturas, tiling o modo rehacen los modelos que lo usan.
    std::shared_ptr<const assets::MaterialAsset> material(const Uuid& uuid);
    void reloadMaterial(const Uuid& uuid);
    // Igual, con los datos ya en memoria (el editor mientras se edita).
    void updateMaterial(const Uuid& uuid, const assets::MaterialAsset& data);

    // Shaders de superficie del usuario (.crshader, ruta dentro de Assets): se
    // compilan la primera vez que un material los usa. Devuelve su id en el
    // renderizador (-1 si no compila: el material usa el shader estandar).
    std::int32_t surfaceShader(const std::string& path);
    // Recompila los .crshader que cambiaron en disco (al guardarlos) y rehace
    // los materiales que los usan. Devuelve cuantos cambiaron.
    int reloadSurfaceShaders();
    // El shader leido (sus propiedades, para el editor de materiales) y su
    // ultimo error (vacio si compila).
    const assets::SurfaceShaderSource* surfaceShaderSource(const std::string& path);
    std::string surfaceShaderError(const std::string& path) const;
    // Modelos distintos que se dibujan (las variantes con materiales tambien):
    // los objetos que comparten uno se agrupan en las mismas llamadas.
    std::size_t variantCount() const { return variants_.size(); }

    // El asset de un modelo ya cargado (nodos, nombres de animaciones).
    std::shared_ptr<const assets::ModelAsset> modelAsset(const Uuid& uuid) const;
    // Caja en el espacio del modelo de un actor (para seleccionar con rayos).
    bool actorLocalBounds(std::uint32_t actor, core::Vec3& min, core::Vec3& max) const;

    // --- Esqueletos (Lua y el editor) ---
    // La entidad con esqueleto de `e`: ella o su primer descendiente animado
    // (que tenga `bone`, si se pide).
    Entity skinnedEntity(World& world, Entity e, const std::string* bone);
    // Matriz de mundo de un hueso (tal como se dibuja este frame).
    bool boneWorld(World& world, Entity e, const std::string& bone, core::Mat4& out);
    struct SkeletonPose {
        Entity source;                     // la pieza con el esqueleto
        std::vector<std::string> names;
        std::vector<int> nodes;            // indice en ModelData::nodes
        std::vector<core::Vec3> positions; // mundo
        std::vector<int> parents;          // indice en estas listas (-1 = raiz)
        float scale = 1.0f;                // de la entidad (metros por unidad)
    };
    // Los huesos para dibujarlos o listarlos.
    bool skeletonPose(World& world, Entity e, SkeletonPose& out);
    // Las particulas de los phys bones (para dibujarlas), o nullptr.
    const std::vector<physbone::Chain>* physBoneChains(World& world, Entity e);
    // El esqueleto (nodos y huesos) de `e` o de su pieza animada, y su escala.
    const asset::ModelData* skeletonData(World& world, Entity e, float* scale = nullptr);

private:
    struct PartKey {
        Uuid uuid;
        int part = 0;
        friend bool operator==(const PartKey&, const PartKey&) = default;
    };
    struct PartKeyHash {
        std::size_t operator()(const PartKey& k) const noexcept {
            return std::hash<Uuid>{}(k.uuid) ^ (static_cast<std::size_t>(k.part) * 0x9E3779B1u);
        }
    };
    // IK suavizado en el tiempo (por entidad): cambiar de objetivo o pisar
    // otra altura no salta de golpe; la mirada, los pies, la cadera, las
    // patas y los objetivos de manos y pies se acercan a lo pedido.
    struct IKSmoothing {
        bool look_valid = false;
        core::Vec3 look{};          // punto que se mira (mundo)
        float look_weight = 0.0f;   // peso que se aplica (sube y baja poco a poco)
        float feet[2] = {0.0f, 0.0f};  // cuanto sube o baja cada pie (humanoide)
        std::vector<float> legs;       // lo mismo por pata con suelo (animales)
        bool limb_valid[4] = {false, false, false, false};
        core::Vec3 limb[4]{};          // objetivo de cada mano y pie (mundo)
        // Pies bloqueados (humanoide).
        struct FootLock {
            bool has_last = false;
            core::Vec3 last{};         // pie de la animacion el frame anterior (mundo)
            float floor = 0.0f;        // altura del pie apoyado (sobre la base)
            bool locked = false;
            core::Vec3 position{};     // donde se clavo (mundo)
            float weight = 0.0f;       // 0..1, sube y baja rapido
        };
        FootLock lock[2];
    };
    struct AnimationState {
        std::uint32_t model = 0;
        anim::Animator animator;
        int clip = -2;  // el que esta sonando (-2 = sin elegir aun)
        bool loop = true;
        int controller_state = -1;  // estado del controlador que sono por ultima vez
        // Con controlador: fase (0..1) del estado actual; y el anterior mientras
        // dura el fundido de la transicion.
        float phase = 0.0f;
        int fade_state = -1;
        float fade_phase = 0.0f;
        float fade_time = 0.0f;
        float fade_duration = 0.0f;
        std::uint64_t seen = 0;     // ultimo frame en que se dibujo
        bool animated = false;      // el frame anterior se animaba (hay que copiar su pose)
        IKSmoothing ik;
        // Transiciones inerciales (controlador y cambios de clip).
        anim::Inertializer inertial;
    };
    struct ClipKey {
        std::uint32_t model = 0;
        Uuid clip;
        friend bool operator==(const ClipKey&, const ClipKey&) = default;
    };
    struct ClipKeyHash {
        std::size_t operator()(const ClipKey& k) const noexcept {
            return std::hash<Uuid>{}(k.clip) ^ (static_cast<std::size_t>(k.model) * 0x9E3779B1u);
        }
    };
    // Indice en ModelData::animations de un .cranim (se anade al modelo la
    // primera vez), o -1.
    int externalClip(std::uint32_t model, const Uuid& clip, scene::Scene& scene);

    std::optional<std::uint32_t> resolveModel(const assets::AssetRef& ref, int part,
                                              scene::Scene& scene, bool& added);
    // El modelo `base` con los materiales de `overrides` (una variante por
    // combinacion, compartida por todos los objetos que la usan).
    std::uint32_t resolveVariant(std::uint32_t base, const std::vector<assets::AssetRef>& overrides,
                                 scene::Scene& scene, bool& added);
    asset::ModelData buildVariant(const asset::ModelData& base, const std::vector<Uuid>& overrides);
    // Mallas creadas por codigo (MeshRenderer::mesh): su modelo en la escena,
    // rehecho (y subido solo el) cuando cambia su version.
    std::optional<std::uint32_t> resolveRuntimeMesh(const std::shared_ptr<Mesh>& mesh, scene::Scene& scene,
                                                    gfx::VulkanRenderer& renderer, bool full_upload_pending);
    void forgetVariantsOf(std::uint32_t base);
    void releaseRuntimeMeshes();
    void applyMaterialChanges(scene::Scene& scene, gfx::VulkanRenderer& renderer, bool& added);
    void syncActors(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                    float delta_seconds);
    void syncLightsAndEnvironment(World& world, scene::Scene& scene,
                                  gfx::VulkanRenderer& renderer);
    void syncCamera(World& world, scene::Scene& scene);
    void syncTerrains(World& world, gfx::VulkanRenderer& renderer, const core::Vec3& eye);
    core::Vec3 grass_eye_{};  // camara: los que apartan la hierba, los mas cercanos
    // Sistema de ambiente (environment/Environment.h): segundos del frame
    // (0 = segunda vista del editor) y lo aplicado (viento, estacion y nieve
    // de la hierba y los arboles).
    float environment_delta_ = 0.0f;
    environment::EnvironmentFrame environment_frame_;
    // Terrenos de este frame (para saber si bajo la camara hay suelo o agua).
    struct TerrainSample {
        std::shared_ptr<terrain::TerrainData> data;
        terrain::Terrain terrain;
        core::Vec3 origin{};
    };
    std::vector<TerrainSample> terrain_samples_;
    bool groundHeight(float x, float z, float& height) const;
    void syncWater(World& world, gfx::VulkanRenderer& renderer, float delta_seconds,
                   const core::Vec3& camera_position);
    struct RiverMesh {
        std::uint64_t hash = 0;
        std::uint64_t version = 0;
        std::vector<gfx::WaterVertex> vertices;
        std::vector<std::uint32_t> indices;
    };
    std::unordered_map<entt::entity, RiverMesh> rivers_;
    // Cinematica inversa: humanoide de cada modelo (sus huesos y ejes).
    struct HumanoidInfo {
        humanoid::Map map;
        std::vector<core::Mat4> rest;
        core::Vec3 up{0.0f, 1.0f, 0.0f};
        core::Vec3 forward{0.0f, 0.0f, 1.0f};
        creature::Rig rig;  // cualquier esqueleto: patas, cabeza, delante
    };
    std::unordered_map<std::uint32_t, HumanoidInfo> humanoids_;
    // Animacion procedural: lo que se simula por entidad (muelles, pies) y
    // su movimiento (velocidad y aceleracion para inclinarse).
    struct ProceduralState {
        std::uint64_t signature = 0;
        std::uint32_t model = 0;
        std::vector<procedural::SpringChain> springs;
        std::vector<procedural::Leg> legs;
        int body = -1;
        core::Vec3 last_position{};
        core::Vec3 velocity{};
        core::Vec3 acceleration{};
        float time = 0.0f;
        bool has_last = false;
    };
    std::unordered_map<entt::entity, ProceduralState> procedural_;
    void applyProceduralBefore(World& world, Entity entity, const ProceduralAnimation& proc, anim::Animator& animator,
                               const asset::ModelData& data, std::uint32_t model, float delta_seconds);
    void applyProceduralSprings(Entity entity, const ProceduralAnimation& proc, anim::Animator& animator,
                                const asset::ModelData& data, std::uint32_t model, float delta_seconds);
    GroundQuery ground_query_;
    const HumanoidInfo& humanoidInfo(std::uint32_t model, const asset::ModelData& data);
    // Esqueletos: que componentes afectan a una pieza (en ella o en un
    // antepasado) y la pose compartida entre las piezas de un mismo modelo.
    struct RigComponents {
        const InverseKinematics* ik = nullptr;
        const ProceduralAnimation* proc = nullptr;
        const Skeleton* skeleton = nullptr;
        const PhysBones* physbones = nullptr;
        Ragdoll* ragdoll = nullptr;
        entt::entity ragdoll_owner = entt::null;
        entt::entity share = entt::null;  // dueno en un antepasado: las piezas comparten la pose
        bool drive = false;               // hay sockets que mueven huesos de esta pieza
        bool any() const {
            return (ik != nullptr && ik->enabled) || (proc != nullptr && proc->enabled) || skeleton != nullptr ||
                   (physbones != nullptr && physbones->enabled) || ragdoll != nullptr || drive;
        }
    };
    RigComponents gatherRig(Entity e);
    struct SharedPose {
        std::uint64_t frame = 0;
        std::vector<core::Mat4> locals;
    };
    std::unordered_map<entt::entity, SharedPose> shared_poses_;
    bool copySharedPose(const RigComponents& rig, anim::Animator& animator, const asset::ModelData& data);
    void storeSharedPose(const RigComponents& rig, anim::Animator& animator);
    void applyBoneOverrides(const Skeleton& skeleton, anim::Animator& animator, const asset::ModelData& data);
    void applyDriveSockets(World& world, Entity entity, anim::Animator& animator, const asset::ModelData& data);
    struct PhysBoneState {
        std::uint64_t signature = 0;
        std::vector<physbone::Chain> chains;
    };
    std::unordered_map<entt::entity, PhysBoneState> physbones_;
    void applyPhysBones(World& world, Entity entity, const PhysBones& bones, anim::Animator& animator,
                        const asset::ModelData& data, float delta_seconds);
    struct RagdollTips {
        std::vector<int> tips;
        std::vector<float> lengths;  // modelo
    };
    std::unordered_map<entt::entity, RagdollTips> ragdoll_tips_;
    void applyRagdoll(Entity entity, Ragdoll& ragdoll, anim::Animator& animator, const asset::ModelData& data,
                      float delta_seconds);
    // Bone Socket -> la pieza animada que lo lleva; piezas -> sockets que
    // mueven sus huesos (del frame anterior).
    std::unordered_map<entt::entity, entt::entity> socket_sources_;
    std::unordered_map<entt::entity, std::vector<entt::entity>> drive_sockets_;
    void updateSockets(World& world, scene::Scene& scene);
    void applyInverseKinematics(World& world, Entity entity, const InverseKinematics& ik, anim::Animator& animator,
                                const asset::ModelData& data, std::uint32_t model, IKSmoothing& smooth,
                                float delta_seconds);
    // Olas interactivas: la simulacion y donde estaba cada cuerpo el frame
    // anterior (su velocidad).
    water::RippleSimulation ripples_;
    std::unordered_map<entt::entity, core::Vec3> ripple_previous_;
    void updateRipples(World& world, gfx::VulkanRenderer& renderer, float delta_seconds,
                       const core::Vec3& camera_position,
                       const std::vector<std::pair<const water::WaterBody*, core::Mat4>>& bodies);
    std::uint64_t river_version_ = 0;
    void destroyTerrains();

    // Render Textures (.crrt): una por archivo, creada al usarla (camara con
    // Target Texture o material que la lee) con el tamano del asset. Si el
    // archivo cambia (otro tamano) se rehace.
    struct RenderTextureGpu {
        std::int32_t id = -1;
        std::filesystem::file_time_type stamp{};
    };
    std::unordered_map<std::string, RenderTextureGpu> render_textures_;  // por ruta absoluta
    std::uint64_t render_texture_checks_ = 0;
    std::int32_t renderTextureForPath(const std::filesystem::path& file);
    std::int32_t renderTextureFor(const Uuid& uuid);
    void refreshRenderTextures();
    void renderCameraTextures(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer);
    void destroyRenderTextures();

    // Vegetacion (foliage::Foliage): se siembra en otro hilo cuando cambia
    // algo (el componente, su posicion o el terreno al terminar un trazo) y
    // se sube entera al renderizador al terminar.
    void syncFoliage(World& world, gfx::VulkanRenderer& renderer);
    std::future<std::vector<gfx::FoliageInstance>> foliage_job_;
    std::uint64_t foliage_signature_ = 0;      // lo que hay en la GPU
    std::uint64_t foliage_job_signature_ = 0;  // lo que se esta sembrando
    bool foliage_uploaded_ = false;
    std::uint64_t foliage_count_ = 0;

public:
    // Arboles de la vegetacion en la GPU (0 si no hay o aun se siembran).
    std::uint64_t foliageCount() const { return foliage_count_; }
    bool foliageGenerating() const { return foliage_job_.valid(); }

private:

    struct TerrainGpu {
        std::uint32_t id = 0;
        std::shared_ptr<terrain::TerrainData> data;
        std::uint32_t resolution = 0;
        std::uint32_t splat_resolution = 0;
    };
    terrain::TerrainStore* terrain_store_ = nullptr;
    gfx::VulkanRenderer* renderer_ = nullptr;
    std::unordered_map<entt::entity, TerrainGpu> terrains_;

    assets::AssetManager& assets_;

    std::unordered_map<PartKey, std::uint32_t, PartKeyHash> models_;
    std::unordered_map<Uuid, std::shared_ptr<const assets::ModelAsset>> loaded_;
    std::unordered_set<Uuid> failed_;
    std::vector<anim::Aabb> model_bounds_;  // por indice de modelo de la escena

    std::unordered_map<entt::entity, AnimationState> animations_;
    std::unordered_map<Uuid, std::shared_ptr<const AnimatorController>> controllers_;
    std::unordered_set<Uuid> failed_controllers_;
    std::unordered_map<ClipKey, int, ClipKeyHash> external_clips_;
    std::unordered_map<std::string, int> decal_textures_;  // ruta -> ranura del renderizador
    std::vector<entt::entity> actor_entities_;
    std::vector<entt::entity> previous_entities_;  // los del frame anterior (reutilizar actores)
    std::uint64_t frame_ = 0;
    std::vector<std::uint32_t> actor_models_;  // modelo de cada actor
    std::unordered_map<entt::entity, std::uint32_t> entity_actor_;

    struct MaterialEntry {
        std::shared_ptr<assets::MaterialAsset> data;
        std::uint64_t structure = 0;
    };
    std::unordered_map<Uuid, MaterialEntry> materials_;
    std::unordered_set<Uuid> failed_materials_;
    struct Variant {
        std::uint32_t base = 0;
        std::uint32_t index = 0;  // en scene.models()
        std::vector<Uuid> overrides;
    };
    std::vector<Variant> variants_;
    struct RuntimeSlot {
        std::weak_ptr<Mesh> mesh;
        std::uint64_t version = 0;
        std::uint64_t material_version = 0;
        std::string material_layout;  // texturas y repeticion (si cambian, se resube)
        std::uint32_t index = 0;  // en scene.models()
    };
    std::unordered_map<const Mesh*, RuntimeSlot> runtime_meshes_;
    std::vector<std::uint32_t> free_runtime_models_;  // huecos de mallas destruidas
    // Telas (physics::Cloth): su rejilla con un hueso por particula, por entidad.
    struct ClothSlot {
        std::string layout;
        std::uint32_t index = 0;
        std::uint64_t frame = 0;
        std::shared_ptr<const physics::SoftBodyMesh> soft_mesh;  // cuerpos blandos
    };
    std::unordered_map<entt::entity, ClothSlot> cloth_models_;
    std::optional<std::uint32_t> resolveSoftBodyModel(Entity e, const physics::SoftBody& body, scene::Scene& scene,
                                                      gfx::VulkanRenderer& renderer, bool full_upload_pending);
    std::optional<std::uint32_t> resolveClothModel(Entity e, const physics::Cloth& cloth, scene::Scene& scene,
                                                   gfx::VulkanRenderer& renderer, bool full_upload_pending);
    void releaseClothModels();
    std::unordered_set<const Mesh*> warned_meshes_;
    std::unordered_map<std::string, std::uint32_t> variant_lookup_;  // clave -> variants_
    std::unordered_set<Uuid> rebuild_materials_;  // cambio de texturas/tiling/modo
    struct SurfaceShaderEntry {
        std::int32_t id = -1;  // en el renderizador
        std::filesystem::file_time_type time{};
        assets::SurfaceShaderSource source;
        bool parsed = false;
        std::string error;
    };
    std::unordered_map<std::string, SurfaceShaderEntry> surface_shaders_;
    void compileSurfaceShader(const std::string& path, SurfaceShaderEntry& entry);
    // Shader y propiedades del .crmat en el material del renderizador; con
    // `texture`, tambien sus texturas (indice en el modelo).
    void applySurface(asset::MaterialData& data, const assets::MaterialAsset& material,
                      const std::function<std::int32_t(const std::string&)>* texture);
    std::unordered_set<Uuid> live_materials_;     // solo factores

    Uuid loaded_environment_{};
    Uuid failed_environment_{};
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_RENDER_SYNC_H
