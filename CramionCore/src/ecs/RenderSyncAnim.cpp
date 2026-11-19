// RenderSync: capas del Animator, montages, eventos y curvas de los clips y
// root motion (ver AnimatorController.h). La base (la maquina de siempre) la
// hace syncActors como antes; aqui se anade lo de encima sobre sus muestras y
// se deja todo en state.plan para evaluarlo en paralelo (evaluateLayered).

#include "CramionCore/ecs/RenderSync.h"

#include <algorithm>
#include <iostream>
#include <system_error>

namespace cramion::ecs {

void RenderSync::ensureClipMeta(std::uint32_t model, const Uuid& model_uuid, scene::Scene& scene) {
    if (clip_meta_done_.contains(model)) return;
    clip_meta_done_.insert(model);
    if (!model_uuid.valid() || model >= scene.models().size()) return;
    const auto info = assets_.database().find(model_uuid);
    if (!info || info->type != assets::AssetType::Model) return;
    const std::filesystem::path sidecar = clipMetaSidecarPath(info->path);
    std::error_code ec;
    if (!std::filesystem::exists(sidecar, ec)) return;  // lo normal: un stat por modelo y escena
    std::map<std::string, ClipMeta> metas;
    std::string error;
    if (!loadModelClipMeta(sidecar, metas, &error)) {
        std::cerr << "[RenderSync] " << error << "\n";
        return;
    }
    if (metas.empty()) return;
    asset::ModelData* data = scene.modelData(model);  // (avisa al streaming)
    if (data == nullptr) return;
    for (asset::AnimationClip& clip : data->animations) {
        if (const auto it = metas.find(clip.name); it != metas.end()) applyClipMeta(clip, it->second);
    }
}

void RenderSync::reloadClipMeta() {
    clip_meta_done_.clear();
}

Uuid RenderSync::actorClipAsset(Entity entity, int clip) const {
    const int actor = actorIndex(entity);
    if (actor < 0 || static_cast<std::size_t>(actor) >= actor_models_.size() || clip < 0) return {};
    const std::uint32_t model = actor_models_[static_cast<std::size_t>(actor)];
    for (const auto& [key, index] : external_clips_) {
        if (key.model == model && index == clip) return key.clip;
    }
    return {};
}

void RenderSync::setActorClipMeta(Entity entity, int clip, const ClipMeta& meta, scene::Scene& scene) {
    const int actor = actorIndex(entity);
    if (actor < 0 || static_cast<std::size_t>(actor) >= actor_models_.size() || clip < 0) return;
    const std::uint32_t model = actor_models_[static_cast<std::size_t>(actor)];
    if (model >= scene.models().size()) return;
    asset::ModelData* data = scene.modelData(model);  // (avisa al streaming)
    if (data == nullptr || static_cast<std::size_t>(clip) >= data->animations.size()) return;
    applyClipMeta(data->animations[static_cast<std::size_t>(clip)], meta);
}

std::shared_ptr<const std::vector<float>> RenderSync::layerMask(const AnimatorController& controller, int layer,
                                                                std::uint32_t model, const asset::ModelData& data) {
    if (layer < 1 || layer > static_cast<int>(controller.layers.size())) return nullptr;
    const AnimatorLayer& def = controller.layers[static_cast<std::size_t>(layer - 1)];
    if (!def.mask.valid() && def.mask_bones.empty()) return nullptr;  // todo el cuerpo
    const std::uint64_t key = (controller.uuid.high * 0x9E3779B97F4A7C15ull) ^ controller.uuid.low ^
                              (static_cast<std::uint64_t>(layer) << 48) ^ (static_cast<std::uint64_t>(model) << 20);
    LayerMaskEntry& entry = layer_masks_[key];
    if (entry.owner == &controller && entry.data == &data && entry.weights &&
        entry.weights->size() == data.nodes.size()) {
        return entry.weights;
    }
    // Rehecha: controlador recargado, otro modelo o el primer uso.
    std::vector<BoneMaskEntry> bones = def.mask_bones;
    if (def.mask.valid()) {
        const auto info = assets_.database().find(def.mask.uuid);
        BoneMask mask;
        std::string error;
        if (info && info->type == assets::AssetType::BoneMask && loadBoneMask(info->path, mask, &error)) {
            bones = std::move(mask.bones);
        } else {
            std::cerr << "[RenderSync] Mascara de la capa " << def.name << " no encontrada " << error << "\n";
        }
    }
    entry.owner = &controller;
    entry.data = &data;
    entry.weights = std::make_shared<const std::vector<float>>(buildMaskWeights(data.nodes, bones));
    return entry.weights;
}

int RenderSync::rootMotionNode(AnimationState& state, const asset::ModelData& data, const std::string& bone) {
    if (state.root_node != -2 && state.root_model == &data && state.root_bone == bone) return state.root_node;
    int found = -1;
    if (!bone.empty()) {
        for (std::size_t i = 0; i < data.nodes.size(); ++i) {
            if (boneNameMatches(data.nodes[i].name, bone)) {
                found = static_cast<int>(i);
                break;
            }
        }
    } else {
        // El hueso mas alto del esqueleto (Hips en Mixamo).
        int best_depth = 1 << 30;
        for (const asset::Bone& b : data.bones) {
            if (b.node < 0 || static_cast<std::size_t>(b.node) >= data.nodes.size()) continue;
            int depth = 0;
            for (int p = data.nodes[static_cast<std::size_t>(b.node)].parent; p >= 0 && depth < 512;
                 p = data.nodes[static_cast<std::size_t>(p)].parent) {
                ++depth;
            }
            if (depth < best_depth) {
                best_depth = depth;
                found = b.node;
            }
        }
    }
    state.root_node = found;
    state.root_model = &data;
    state.root_bone = bone;
    return found;
}

bool RenderSync::buildAnimationLayers(Entity e, Animator& animator, const AnimatorController* controller,
                                      AnimationState& state, std::uint32_t model, const asset::ModelData& data,
                                      scene::Scene& scene, float dt, const AnimatorSyncSource& base_sync,
                                      std::size_t base_current_begin) {
    (void)e;
    AnimatorRuntime& runtime = animator.runtime;
    anim::LayeredPosePlan& plan = state.plan;
    plan.layer_count = 0;
    const bool replay = runtime.replay_control;
    const float step = animator.playing ? dt : 0.0f;

    // Lo normal (sin capas, montage, eventos ni curvas): nada que hacer.
    if (controller == nullptr || controller->layers.empty()) {
        if (!runtime.layer_requests.empty()) runtime.layer_requests.clear();  // capas que no hay
    }
    if ((controller == nullptr || controller->layers.empty()) && !runtime.montage.active) {
        bool meta = false;
        for (const anim::ClipSample& s : plan.base) {
            if (s.clip < 0 || static_cast<std::size_t>(s.clip) >= data.animations.size()) continue;
            const asset::AnimationClip& clip = data.animations[static_cast<std::size_t>(s.clip)];
            meta = meta || !clip.events.empty() || !clip.curves.empty();
        }
        if (!meta) {
            if (!runtime.curves.empty()) runtime.curves.clear();
            return false;
        }
    }

    // Eventos del estado de la base que suena y curvas de todo lo que suena.
    if (!replay && base_current_begin <= plan.base.size()) {
        collectSampleEvents(data, plan.base.data() + base_current_begin, plan.base.size() - base_current_begin,
                            base_sync.entered, 0, runtime);
    }
    if (!runtime.curves.empty()) runtime.curves.clear();
    accumulateSampleCurves(data, plan.base.data(), plan.base.size(), 1.0f, runtime.curves);

    const auto resolve = [&](const std::string& name, const assets::AssetRef& ref) -> int {
        if (ref.valid()) return externalClip(model, ref.uuid, scene);
        if (name.empty()) return -1;
        const asset::ModelData& now = *scene.models()[model];
        for (std::size_t i = 0; i < now.animations.size(); ++i) {
            if (now.animations[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    // (externalClip puede anadir clips al modelo: siempre por indice.)
    const AnimatorClipLookup lookup{resolve, [&](int c) {
                                        const asset::ModelData& now = *scene.models()[model];
                                        return c >= 0 && static_cast<std::size_t>(c) < now.animations.size()
                                                   ? now.animations[static_cast<std::size_t>(c)].duration
                                                   : 0.0f;
                                    }};

    // --- Capas de encima ---
    if (controller != nullptr && !controller->layers.empty()) {
        if (runtime.layers.size() < controller->layers.size()) runtime.layers.resize(controller->layers.size());
        // Nombres para buscar desde los scripts (tambien si el controlador se
        // recargo con otros nombres).
        bool names_ok = runtime.layer_names.size() == controller->layers.size() + 1 &&
                        runtime.layer_names[0] == controller->base_layer_name;
        for (std::size_t i = 0; names_ok && i < controller->layers.size(); ++i) {
            names_ok = runtime.layer_names[i + 1] == controller->layers[i].name;
        }
        if (!names_ok) {
            runtime.layer_names.clear();
            runtime.layer_names.push_back(controller->base_layer_name);
            for (const AnimatorLayer& l : controller->layers) runtime.layer_names.push_back(l.name);
        }
        // Lo que pidieron los scripts antes de conocer las capas.
        for (const AnimatorRuntime::LayerRequest& request : runtime.layer_requests) {
            const int k = controller->findLayer(request.layer);
            if (k == 0 && !request.cross_fade.empty()) {
                runtime.cross_fade = request.cross_fade;
                runtime.cross_fade_time = request.cross_fade_time;
            }
            if (k < 1) continue;
            AnimatorLayerRuntime& lr = runtime.layers[static_cast<std::size_t>(k - 1)];
            if (request.weight >= 0.0f) lr.weight = request.weight;
            if (!request.cross_fade.empty()) {
                lr.cross_fade = request.cross_fade;
                lr.cross_fade_time = request.cross_fade_time;
            }
        }
        runtime.layer_requests.clear();
        for (int k = 1; k <= static_cast<int>(controller->layers.size()); ++k) {
            const AnimatorLayer& def = controller->layers[static_cast<std::size_t>(k - 1)];
            AnimatorSyncSource sync{};
            const AnimatorSyncSource* sync_ptr = nullptr;
            if (def.sync_layer == 0) {
                sync = base_sync;
                sync_ptr = &sync;
            } else if (def.sync_layer > 0 && def.sync_layer < k) {
                const AnimatorLayerRuntime& src = runtime.layers[static_cast<std::size_t>(def.sync_layer - 1)];
                sync = AnimatorSyncSource{src.state, src.phase, src.dphase, src.fade_state, src.fade_phase,
                                          src.fade_weight, src.entered};
                sync_ptr = &sync;
            }
            std::size_t begin = 0;
            stepAnimatorLayer(*controller, k, runtime, replay ? 0.0f : step, animator.speed, lookup, layer_samples_,
                              begin, sync_ptr, blend_weights_);
            // Lo que aun se esta leyendo (o no existe) no suena: nada de pose T.
            std::size_t kept_begin = begin;
            std::size_t w = 0;
            for (std::size_t r = 0; r < layer_samples_.size(); ++r) {
                if (layer_samples_[r].clip < 0) {
                    if (r < begin) --kept_begin;
                    continue;
                }
                layer_samples_[w++] = layer_samples_[r];
            }
            layer_samples_.resize(w);
            const float weight = animatorLayerWeight(*controller, runtime, k);
            runtime.layers[static_cast<std::size_t>(k - 1)].effective_weight = weight;
            if (!replay) {
                collectSampleEvents(data, layer_samples_.data() + kept_begin, layer_samples_.size() - kept_begin,
                                    runtime.layers[static_cast<std::size_t>(k - 1)].entered, k, runtime);
            }
            if (layer_samples_.empty() || weight <= 1e-5f) continue;
            accumulateSampleCurves(data, layer_samples_.data(), layer_samples_.size(), weight, runtime.curves);
            anim::PoseLayerJob& job = plan.addLayer();
            job.samples.assign(layer_samples_.begin(), layer_samples_.end());
            job.weight = weight;
            job.mode = def.blending == AnimatorLayerBlend::Additive ? anim::LayerBlendMode::Additive
                                                                     : anim::LayerBlendMode::Override;
            job.mask = layerMask(*controller, k, model, data);
            if (job.mode == anim::LayerBlendMode::Additive && (!def.reference_clip_name.empty() || def.reference_clip.valid())) {
                const int reference = resolve(def.reference_clip_name, def.reference_clip);
                if (reference >= 0) job.reference.push_back(anim::ClipSample{reference, def.reference_time, 1.0f});
            }
        }
    }

    // --- Montage ---
    AnimatorMontage& m = runtime.montage;
    if (m.active) {
        const int c = resolve(m.clip_name, m.clip);
        if (c < 0) {
            // Leyendose (streaming): espera; si no existe, se olvida.
            if (!(m.clip.valid() && clipPending(model, m.clip.uuid))) {
                m.active = false;
                m.weight = 0.0f;
            }
        } else {
            m.clip_index = c;
            fired_events_.clear();
            const asset::AnimationClip& clip = scene.models()[model]->animations[static_cast<std::size_t>(c)];
            stepMontage(m, clip, replay ? 0.0f : step, replay ? nullptr : &fired_events_);
            for (const std::uint32_t k : fired_events_) {
                const asset::ClipEvent& ev = clip.events[k];
                runtime.pushEvent(AnimatorFiredEvent{ev.name, clip.name, ev.params, ev.time, -1});
            }
            if (m.weight > 1e-5f) {
                anim::PoseLayerJob& job = plan.addLayer();
                job.samples.push_back(anim::ClipSample{c, m.time, 1.0f, m.delta, false});
                job.weight = m.weight;
                job.mode = anim::LayerBlendMode::Override;
                if (!m.slot.empty() && controller != nullptr) {
                    const int slot = controller->findLayer(m.slot);
                    if (slot >= 1) job.mask = layerMask(*controller, slot, model, data);
                }
                // En el cuerpo entero el montage mueve al personaje (esquivas).
                job.root_motion = !job.mask;
                accumulateSampleCurves(data, job.samples.data(), 1, m.weight, runtime.curves);
            }
        }
    }
    return plan.layer_count > 0;
}

}  // namespace cramion::ecs
