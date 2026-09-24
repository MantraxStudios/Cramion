#include "CramionCore/ecs/RenderSync.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <iostream>

namespace cramion::ecs {

using core::Mat4;
using core::Vec3;

namespace {
constexpr float kDegToRad = core::kPi / 180.0f;
}  // namespace

std::shared_ptr<const AnimatorController> RenderSync::animatorController(const Uuid& uuid) {
    if (const auto it = controllers_.find(uuid); it != controllers_.end()) {
        return it->second;
    }
    if (failed_controllers_.contains(uuid)) {
        return nullptr;
    }
    const auto info = assets_.database().find(uuid);
    auto controller = std::make_shared<AnimatorController>();
    std::string error;
    if (!info || info->type != assets::AssetType::AnimatorController ||
        !loadAnimatorController(info->path, *controller, &error)) {
        failed_controllers_.insert(uuid);
        std::cerr << "[RenderSync] No se pudo cargar el Animator " << uuid.toString() << " " << error << "\n";
        return nullptr;
    }
    controllers_[uuid] = controller;
    return controller;
}

void RenderSync::reloadAnimatorController(const Uuid& uuid) {
    controllers_.erase(uuid);
    failed_controllers_.erase(uuid);
    // Los clips sueltos del controlador pueden haber cambiado tambien.
    for (auto it = external_clips_.begin(); it != external_clips_.end();) {
        it = it->second < 0 ? external_clips_.erase(it) : std::next(it);
    }
}

int RenderSync::externalClip(std::uint32_t model, const Uuid& clip, scene::Scene& scene) {
    const ClipKey key{model, clip};
    if (const auto it = external_clips_.find(key); it != external_clips_.end()) {
        return it->second;
    }
    int index = -1;
    const auto info = assets_.database().find(clip);
    if (info && info->type == assets::AssetType::AnimationClip && model < scene.models().size()) {
        asset::ModelData& data = *scene.models()[model];
        asset::AnimationClip loaded;
        std::string error;
        if (loadAnimationClip(info->path, data, loaded, &error) && !loaded.channels.empty()) {
            // Se anade al modelo (el Animator reproduce por indice).
            data.animations.push_back(std::move(loaded));
            index = static_cast<int>(data.animations.size()) - 1;
        } else {
            std::cerr << "[RenderSync] Clip " << info->name << " no sirve para este modelo " << error << "\n";
        }
    }
    external_clips_[key] = index;
    return index;
}

const asset::ModelData* RenderSync::actorModelData(Entity entity, const scene::Scene& scene) const {
    const int actor = actorIndex(entity);
    if (actor < 0 || static_cast<std::size_t>(actor) >= actor_models_.size()) {
        return nullptr;
    }
    const std::uint32_t model = actor_models_[static_cast<std::size_t>(actor)];
    return model < scene.models().size() ? scene.models()[model].get() : nullptr;
}

void RenderSync::reset(scene::Scene& scene) {
    scene.actors().clear();
    scene.clear();
    for (const auto& [uuid, asset] : loaded_) {
        // Las piezas se movieron a la escena: el AssetManager ya no las tiene
        // completas. Se descargan para que la proxima vez se relean.
        assets_.unload(uuid);
    }
    models_.clear();
    loaded_.clear();
    failed_.clear();
    model_bounds_.clear();
    animations_.clear();
    controllers_.clear();
    failed_controllers_.clear();
    external_clips_.clear();
    decal_textures_.clear();
    actor_entities_.clear();
    previous_entities_.clear();
    actor_models_.clear();
    entity_actor_.clear();
    loaded_environment_ = {};
    failed_environment_ = {};
}

std::optional<std::uint32_t> RenderSync::resolveModel(const assets::AssetRef& ref, int part,
                                                       scene::Scene& scene, bool& added) {
    if (!ref.valid() || part < 0) {
        return std::nullopt;
    }
    const PartKey key{ref.uuid, part};
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    if (failed_.contains(ref.uuid)) {
        return std::nullopt;
    }

    std::shared_ptr<const assets::ModelAsset> asset;
    if (const auto it = loaded_.find(ref.uuid); it != loaded_.end()) {
        asset = it->second;
    } else {
        asset = assets_.loadModel(ref.uuid);
        if (!asset) {
            failed_.insert(ref.uuid);
            std::cerr << "[RenderSync] No se pudo cargar el modelo " << ref.uuid.toString() << "\n";
            return std::nullopt;
        }
        loaded_[ref.uuid] = asset;
    }
    if (static_cast<std::size_t>(part) >= asset->parts.size() || !asset->parts[part] ||
        asset->parts[part]->indices.empty()) {
        return std::nullopt;
    }

    // La pieza pasa a la escena UNA vez (sin copiar gigas de mallas): el
    // AssetManager se queda con un ModelData vacio de esa pieza.
    const std::uint32_t index = scene.addModel(std::move(*asset->parts[part]));
    const asset::ModelData& data = *scene.models()[index];
    anim::Animator bind(data);
    bind.play(-1);
    if (model_bounds_.size() <= index) {
        model_bounds_.resize(index + 1);
    }
    model_bounds_[index] = anim::skinnedBounds(data, bind.boneMatrices());
    models_[key] = index;
    added = true;
    return index;
}

void RenderSync::sync(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                      float delta_seconds, const Options& options) {
    // La animacion la lleva el componente Animator, no scene.update().
    scene.setAnimateActors(false);
    syncActors(world, scene, renderer, delta_seconds);
    syncLightsAndEnvironment(world, scene, renderer);
    if (options.apply_main_camera) {
        syncCamera(world, scene);
    }
}

void RenderSync::syncActors(World& world, scene::Scene& scene, gfx::VulkanRenderer& renderer,
                            float delta_seconds) {
    bool added = false;
    // Los actores se rellenan en su sitio (sin reconstruir el vector ni copiar
    // el animador de lo que no se anima): con cientos de objetos, rehacerlo
    // todo cada frame eran miles de reservas de memoria.
    std::vector<scene::Actor>& actors = scene.actors();
    previous_entities_.swap(actor_entities_);
    actor_entities_.clear();
    actor_models_.clear();
    std::size_t count = 0;
    ++frame_;

    // En profundidad: el orden de los actores sigue al de la Jerarquia (estable
    // entre frames, lo que agradecen las cascadas de sombra).
    world.forEachDepthFirst([&](Entity e) {
        const MeshRenderer* renderer_component = e.tryGet<MeshRenderer>();
        if (renderer_component == nullptr || !renderer_component->visible ||
            !e.activeInHierarchy()) {
            return;
        }
        const std::optional<std::uint32_t> model =
            resolveModel(renderer_component->model, renderer_component->part, scene, added);
        if (!model) {
            return;
        }
        const asset::ModelData& data = *scene.models()[*model];

        // Animador persistente por entidad (su tiempo sobrevive entre frames).
        AnimationState& state = animations_[e.handle()];
        if (state.clip == -2 || state.model != *model) {
            state = AnimationState{};
            state.model = *model;
            state.animator = anim::Animator(data);
            state.clip = -1;
            state.animator.play(-1);
        }
        state.seen = frame_;

        bool animating = false;
        if (Animator* animator = e.tryGet<Animator>(); animator != nullptr && !data.animations.empty()) {
            animating = true;
            int clip = animator->clip;
            bool loop = animator->loop;
            float speed = animator->speed;
            bool restart = false;

            // Con controlador: la maquina de estados elige el clip.
            const std::shared_ptr<const AnimatorController> controller =
                animator->controller.valid() ? animatorController(animator->controller.uuid) : nullptr;
            if (controller && !controller->states.empty()) {
                AnimatorRuntime& runtime = animator->runtime;
                const auto clipOf = [&](int index) {
                    if (index < 0 || index >= static_cast<int>(controller->states.size())) return -1;
                    const AnimatorState& st = controller->states[index];
                    if (st.clip.valid()) return externalClip(*model, st.clip.uuid, scene);
                    for (std::size_t i = 0; i < data.animations.size(); ++i) {
                        if (data.animations[i].name == st.clip_name) return static_cast<int>(i);
                    }
                    return -1;
                };
                const int current = clipOf(runtime.state);
                const float duration = current >= 0 ? data.animations[current].duration : 0.0f;
                stepAnimatorController(*controller, runtime, duration);
                const AnimatorState& st = controller->states[runtime.state];
                clip = clipOf(runtime.state);
                loop = st.loop;
                speed = animator->speed * st.speed;
                if (runtime.state != state.controller_state) {
                    state.controller_state = runtime.state;
                    animator->time = 0.0f;
                    restart = true;
                }
                if (animator->playing) runtime.state_time += delta_seconds * std::abs(speed);
            } else {
                state.controller_state = -1;
                if (!animator->clip_name.empty()) {
                    for (std::size_t i = 0; i < data.animations.size(); ++i) {
                        if (data.animations[i].name == animator->clip_name) {
                            clip = static_cast<int>(i);
                            break;
                        }
                    }
                }
            }
            clip = std::clamp(clip, -1, static_cast<int>(data.animations.size()) - 1);
            if (restart || clip != state.clip || loop != state.loop) {
                state.animator.play(clip, loop);
                state.animator.setTime(animator->time);
                state.clip = clip;
                state.loop = loop;
            }
            state.animator.setSpeed(speed);
            if (animator->playing) {
                state.animator.update(delta_seconds);
                animator->time = state.animator.time();
            } else if (std::abs(state.animator.time() - animator->time) > 1e-5f) {
                // Pausado: el tiempo del Inspector manda (arrastrarlo = scrub).
                state.animator.setTime(animator->time);
                state.animator.evaluate();
            }
        }

        const Mat4& world_matrix = e.worldMatrix();
        if (count >= actors.size()) actors.emplace_back();
        scene::Actor& actor = actors[count];
        // El mismo objeto en la misma posicion de la lista y sin animar: su
        // animador (pose de reposo) ya esta; no se copia.
        const bool same_slot = count < previous_entities_.size() && previous_entities_[count] == e.handle() &&
                               actor.model == *model;
        actor.model = *model;
        actor.transform = world_matrix;
        if (!same_slot || animating || state.animated) actor.animator = state.animator;
        state.animated = animating;
        const anim::Aabb& box = model_bounds_[*model];
        const Vec3 center = (box.min + box.max) * 0.5f;
        actor.bounds_center = transformPoint(world_matrix, center);
        actor.bounds_radius = core::length(box.max - box.min) * 0.5f * maxAxisScale(world_matrix);

        actor_entities_.push_back(e.handle());
        actor_models_.push_back(*model);
        ++count;
    });
    actors.resize(count);

    // El indice entidad -> actor solo se rehace si la lista cambio.
    if (actor_entities_ != previous_entities_) {
        entity_actor_.clear();
        for (std::size_t i = 0; i < actor_entities_.size(); ++i) {
            entity_actor_[actor_entities_[i]] = static_cast<std::uint32_t>(i);
        }
    }

    // Olvida los animadores de lo que ya no se dibuja.
    for (auto it = animations_.begin(); it != animations_.end();) {
        it = it->second.seen == frame_ ? std::next(it) : animations_.erase(it);
    }

    if (added) {
        // Piezas nuevas: el renderizador sube la escena entera (mallas,
        // texturas y la estructura de los rayos).
        renderer.uploadModels(scene);
    }
}

void RenderSync::syncLightsAndEnvironment(World& world, scene::Scene& scene,
                                          gfx::VulkanRenderer& renderer) {
    scene::LightSet& lights = scene.lights();
    lights.points.clear();
    lights.spots.clear();

    Entity directional;
    Entity sky_entity;
    Entity weather_entity;
    const PostProcessing* post = nullptr;

    world.forEachDepthFirst([&](Entity e) {
        if (!e.activeInHierarchy()) {
            return;
        }
        if (const Light* light = e.tryGet<Light>()) {
            switch (light->type) {
                case LightType::Directional:
                    if (!directional.valid()) {
                        directional = e;
                    }
                    break;
                case LightType::Point:
                    if (lights.points.size() < scene::kMaxPointLights) {
                        scene::PointLight p{};
                        p.position = e.worldPosition();
                        p.color = light->color;
                        p.intensity = light->intensity;
                        p.range = light->range;
                        lights.points.push_back(p);
                    }
                    break;
                case LightType::Spot:
                    if (lights.spots.size() < scene::kMaxSpotLights) {
                        scene::SpotLight s{};
                        s.position = e.worldPosition();
                        s.direction = e.forward();
                        s.color = light->color;
                        s.intensity = light->intensity;
                        s.range = light->range;
                        const float inner = std::min(light->inner_angle, light->outer_angle - 0.5f);
                        s.inner_angle = std::max(inner, 0.5f) * kDegToRad;
                        s.outer_angle = light->outer_angle * kDegToRad;
                        s.enabled = true;
                        lights.spots.push_back(s);
                    }
                    break;
            }
        }
        if (!sky_entity.valid() && e.has<Sky>()) {
            sky_entity = e;
        }
        if (!weather_entity.valid() && e.has<Weather>()) {
            weather_entity = e;
        }
        if (const PostProcessing* p = e.tryGet<PostProcessing>()) {
            if (post == nullptr || p->priority > post->priority) {
                post = p;
            }
        }
    });

    // --- Cielo HDR ---
    Sky* sky = sky_entity.valid() ? sky_entity.tryGet<Sky>() : nullptr;
    bool hdr_active = false;
    if (sky != nullptr && sky->use_hdr && sky->environment.valid()) {
        const Uuid& wanted = sky->environment.uuid;
        if (wanted != loaded_environment_ && wanted != failed_environment_) {
            const std::filesystem::path file = assets_.environmentFile(wanted);
            if (!file.empty() && renderer.loadEnvironment(file)) {
                loaded_environment_ = wanted;
            } else {
                failed_environment_ = wanted;
                std::cerr << "[RenderSync] No se pudo cargar el cielo " << wanted.toString() << "\n";
            }
        }
        hdr_active = loaded_environment_ == wanted;
    }
    renderer.setEnvironmentEnabled(hdr_active);
    renderer.setCloudsEnabled(sky == nullptr || sky->clouds);

    // --- Sol ---
    // Luz direccional > sol de la foto HDR > hora del cielo.
    if (directional.valid()) {
        const Light& light = directional.get<Light>();
        scene.setFixedSun(-directional.forward());
        // scene.update() ya calculo el sol fisico de este frame: el componente
        // lo tiñe y lo escala.
        lights.sun.color = lights.sun.color * light.color;
        lights.sun.intensity *= light.intensity;
    } else if (hdr_active) {
        scene.setFixedSun(renderer.environmentSunDirection());
    } else {
        scene.setFixedSun(std::nullopt);
        if (sky != nullptr) {
            scene.setDayCycleEnabled(sky->day_cycle);
            if (sky->day_cycle) {
                sky->time_of_day = scene.timeOfDayHours();  // el Inspector ve la hora
            } else {
                scene.setTimeOfDayHours(sky->time_of_day);
            }
        }
    }

    // --- Clima ---
    if (const Weather* weather = weather_entity.valid() ? weather_entity.tryGet<Weather>() : nullptr) {
        renderer.setRainEnabled(weather->rain);
        renderer.setWeather(weather->wetness, weather->puddles);
        renderer.setWater(weather->flood_center,
                          weather->flood ? weather->flood_radii : core::Vec2{});
        renderer.setWaterEnabled(weather->flood);
    } else {
        renderer.setRainEnabled(false);
        renderer.setWeather(0.0f, 0.0f);
        renderer.setWater(core::Vec2{}, core::Vec2{});
    }

    // --- Decals ---
    const Weather* global_weather = weather_entity.valid() ? weather_entity.tryGet<Weather>() : nullptr;
    const float rain_puddles = global_weather != nullptr && global_weather->rain ? global_weather->puddles : 0.0f;
    std::vector<gfx::VulkanRenderer::Decal> decals;
    for (const entt::entity handle : world.registry().view<Decal>()) {
        const Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const Decal& d = *e.tryGet<Decal>();
        gfx::VulkanRenderer::Decal out;
        out.world_to_decal = core::inverse(e.worldMatrix());
        out.axis = e.up();
        out.color = d.color;
        out.opacity = d.opacity;
        out.type = static_cast<int>(d.type);
        out.edge_softness = d.edge_softness * 0.5f + 0.001f;
        out.angle_fade = std::cos(std::clamp(d.max_angle, 1.0f, 90.0f) * kDegToRad);
        out.roughness = d.roughness;
        out.roughness_amount = d.roughness_amount;
        out.metallic = d.metallic;
        out.amount = d.amount * (d.type != DecalType::Stamp && d.follow_rain ? std::min(rain_puddles * 2.0f, 1.0f) : 1.0f);
        if (!d.texture.empty()) {
            const auto it = decal_textures_.find(d.texture);
            if (it != decal_textures_.end()) {
                out.texture = it->second;
            } else {
                const std::filesystem::path file = assets_.database().root() /
                    std::filesystem::path(std::u8string(d.texture.begin(), d.texture.end()));
                out.texture = renderer.loadDecalTexture(file);
                decal_textures_[d.texture] = out.texture;
            }
        }
        if (out.amount <= 0.0f && d.type != DecalType::Stamp) continue;
        decals.push_back(out);
    }
    renderer.setDecals(std::move(decals));

    // --- Post-proceso ---
    renderer.setPostProcess(post != nullptr ? post->settings : gfx::PostProcessSettings{});
}

void RenderSync::syncCamera(World& world, scene::Scene& scene) {
    Entity main;
    world.forEachDepthFirst([&](Entity e) {
        const Camera* camera = e.tryGet<Camera>();
        if (camera != nullptr && e.activeInHierarchy() && (!main.valid() || camera->is_main)) {
            if (!main.valid() || !main.get<Camera>().is_main) {
                main = e;
            }
        }
    });
    if (!main.valid()) {
        return;
    }
    const Camera& camera = main.get<Camera>();
    scene::Camera& view = scene.camera();
    view.setPosition(main.worldPosition());
    view.setOrientation(main.forward(), main.up());
    view.setFovY(camera.fov * kDegToRad);
    // (Los planos cercano y lejano de scene::Camera no se pueden fijar desde
    // fuera todavia.)
}

int RenderSync::actorIndex(Entity entity) const {
    const auto it = entity_actor_.find(entity.handle());
    return it == entity_actor_.end() ? -1 : static_cast<int>(it->second);
}

Entity RenderSync::entityForActor(const World& world, std::uint32_t actor) const {
    if (actor >= actor_entities_.size() || !world.valid(actor_entities_[actor])) {
        return {};
    }
    return world.wrap(actor_entities_[actor]);
}

std::vector<std::uint32_t> RenderSync::actorIndicesInSubtree(Entity entity) const {
    std::vector<std::uint32_t> result;
    if (!entity.valid()) {
        return result;
    }
    std::vector<entt::entity> stack{entity.handle()};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (const auto it = entity_actor_.find(e); it != entity_actor_.end()) {
            result.push_back(it->second);
        }
        const auto& children = entity.world()->wrap(e).children();
        stack.insert(stack.end(), children.begin(), children.end());
    }
    return result;
}

std::shared_ptr<const assets::ModelAsset> RenderSync::modelAsset(const Uuid& uuid) const {
    const auto it = loaded_.find(uuid);
    return it == loaded_.end() ? nullptr : it->second;
}

bool RenderSync::actorLocalBounds(std::uint32_t actor, Vec3& min, Vec3& max) const {
    if (actor >= actor_models_.size() || actor_models_[actor] >= model_bounds_.size()) {
        return false;
    }
    min = model_bounds_[actor_models_[actor]].min;
    max = model_bounds_[actor_models_[actor]].max;
    return true;
}

}  // namespace cramion::ecs
