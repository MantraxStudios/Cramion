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

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/AnimatorController.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/CramionFX.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cramion::ecs {

class RenderSync {
public:
    struct Options {
        // La vista usa la Camera principal del mundo (vista de juego). Si es
        // false (vista de escena del editor), la camara de scene::Scene no se
        // toca.
        bool apply_main_camera = false;
    };

    explicit RenderSync(assets::AssetManager& assets) : assets_(assets) {}

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
    const asset::ModelData* actorModelData(Entity entity, const scene::Scene& scene) const;

    // Animator Controllers (.cranimator) en uso: se leen una vez; el editor
    // llama a reload... al guardarlos para que el cambio se vea al momento.
    std::shared_ptr<const AnimatorController> animatorController(const Uuid& uuid);
    void reloadAnimatorController(const Uuid& uuid);

    // El asset de un modelo ya cargado (nodos, nombres de animaciones).
    std::shared_ptr<const assets::ModelAsset> modelAsset(const Uuid& uuid) const;
    // Caja en el espacio del modelo de un actor (para seleccionar con rayos).
    bool actorLocalBounds(std::uint32_t actor, core::Vec3& min, core::Vec3& max) const;

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
    struct AnimationState {
        std::uint32_t model = 0;
        anim::Animator animator;
        int clip = -2;  // el que esta sonando (-2 = sin elegir aun)
        bool loop = true;
        int controller_state = -1;  // estado del controlador que sono por ultima vez
        std::uint64_t seen = 0;     // ultimo frame en que se dibujo
        bool animated = false;      // el frame anterior se animaba (hay que copiar su pose)
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
    void syncActors(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                    float delta_seconds);
    void syncLightsAndEnvironment(World& world, scene::Scene& scene,
                                  gfx::VulkanRenderer& renderer);
    void syncCamera(World& world, scene::Scene& scene);

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

    Uuid loaded_environment_{};
    Uuid failed_environment_{};
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_RENDER_SYNC_H
