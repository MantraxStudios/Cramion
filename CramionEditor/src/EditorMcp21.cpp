// Herramientas MCP de la 2.1 (las declara EditorMcp.cpp): pruebas
// automaticas, iluminacion horneada, VFX Graph, Visual Scripting, Shader
// Graph y Behavior Trees. Entrada y salida en JSON.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/ai/BehaviorTree.h>
#include <CramionCore/asset/ShaderGraph.h>
#include <CramionCore/lighting/ProbeBaker.h>
#include <CramionCore/scripting/VisualScript.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using nlohmann::json;

namespace {

std::filesystem::path assetFile(const std::filesystem::path& assets, const std::string& folder, const std::string& name,
                                const std::string& extension) {
    std::filesystem::path dir = assets / dialogs::fromUtf8(folder);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string stem = name.empty() ? std::string("Nuevo") : name;
    if (stem.size() > extension.size() && stem.substr(stem.size() - extension.size()) == extension) {
        stem = stem.substr(0, stem.size() - extension.size());
    }
    return dir / dialogs::fromUtf8(stem + extension);
}

}  // namespace

std::string EditorApp::mcpTools21(const std::string& name, const std::string& args_json, std::string& error) {
    const json args = json::parse(args_json, nullptr, false);
    const auto str = [&](const char* key, const std::string& fallback = {}) {
        return args.is_object() && args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : fallback;
    };
    const auto entityOf = [&](const char* key) -> ecs::Entity {
        const std::string text = str(key);
        if (text.empty()) return {};
        if (const Uuid u = Uuid::parse(text); u.valid()) {
            if (ecs::Entity e = world_.find(u); e.valid()) return e;
        }
        return world_.findByName(text);
    };

    if (name == "run_tests") {
        const std::string mode = str("mode", "all");
        startTestRun(mode == "edit" ? 1 : (mode == "play" ? 2 : 0));
        return json{{"started", testRunActive()}, {"note", "Usa test_results para ver el resultado (las pruebas de Play tardan)."}}.dump();
    }
    if (name == "test_results") {
        return json{{"running", testRunActive()}, {"results", json::parse(testResultsJson(), nullptr, false)}}.dump();
    }
    if (name == "bake_lighting") {
        int sun_mode = -1;
        if (args.contains("sun_mode") && args["sun_mode"].is_string()) {
            const std::string m = args["sun_mode"].get<std::string>();
            sun_mode = m == "baked" ? 1 : (m == "realtime" ? 2 : (m == "mixed" ? 0 : -1));
        }
        configureLightingBake(args.value("rays", -1), args.value("bounces", -1), args.value("spacing", -1.0f), sun_mode,
                              args.value("surface_texel", -1.0f), args.value("direct_samples", -1));
        configureLightmapBake(args_json);
        startLightingBake();
        return lightingStateJson();
    }
    if (name == "lighting_state") return lightingStateJson();
    if (name == "set_lighting_mode") return setLightingModeJson(args_json);
    if (name == "generate_lightmap_uvs") return generateLightmapUvsJson(args_json);

    if (name == "create_vfx") {
        int preset = 0;
        const std::string wanted = str("preset", "Chispas");
        for (int i = 0; i < vfx::vfxPresetCount(); ++i) {
            if (dialogs::utf8(dialogs::fromUtf8(vfx::vfxPresetName(i))) == wanted) preset = i;
        }
        if (args.contains("preset") && args["preset"].is_number_integer()) preset = args["preset"].get<int>();
        vfx::VfxGraph graph = vfx::vfxPreset(preset);
        if (args.contains("graph") && args["graph"].is_object()) {
            vfx::VfxGraph custom;
            std::string parse_error;
            if (!vfx::vfxGraphFromText(args["graph"].dump(), custom, &parse_error)) {
                error = "graph: " + parse_error;
                return {};
            }
            if (!custom.uuid.valid()) custom.uuid = graph.uuid;
            graph = std::move(custom);
        }
        // Escalabilidad: tipo de efecto (Hero, Combat, Impact, Ambient, None) y orden de dibujo.
        if (const std::string type = str("effect_type"); !type.empty()) {
            const vfx::EffectType parsed = vfx::effectTypeFromKey(type, static_cast<vfx::EffectType>(-1));
            if (static_cast<int>(parsed) < 0) {
                error = "effect_type: Hero, Combat, Impact, Ambient o None";
                return {};
            }
            graph.effect_type = parsed;
        }
        if (args.contains("sort_order") && args["sort_order"].is_number()) graph.sort_order = args["sort_order"].get<int>();
        // Tipo de salida sobre la plantilla o el JSON: Sprite (quads), Ribbon
        // (cinta) o Mesh; y el flipbook fundido.
        if (args.contains("output_type") && args["output_type"].is_string()) {
            const std::string type = args["output_type"].get<std::string>();
            if (type == "Sprite" || type == "Quads") graph.output.type = gfx::VfxOutput::Quads;
            else if (type == "Ribbon" || type == "Cinta") graph.output.type = gfx::VfxOutput::Ribbon;
            else if (type == "Mesh" || type == "Malla") graph.output.type = gfx::VfxOutput::Mesh;
        }
        if (args.contains("flip_blend") && args["flip_blend"].is_boolean()) graph.output.flip_blend = args["flip_blend"].get<bool>();
        // Luz por particula: anade (o cambia) el bloque ParticleLight de la salida.
        if (args.contains("light") && args["light"].is_object()) {
            const json& l = args["light"];
            auto found = std::find_if(graph.output_blocks.begin(), graph.output_blocks.end(),
                                      [](const vfx::Block& b) { return b.kind == vfx::BlockKind::ParticleLight; });
            if (found == graph.output_blocks.end()) {
                graph.output_blocks.push_back(vfx::makeBlock(vfx::BlockKind::ParticleLight));
                found = std::prev(graph.output_blocks.end());
            }
            vfx::Block& b = *found;
            b.enabled = true;
            if (l.contains("enabled") && l["enabled"].is_boolean()) b.values[0] = l["enabled"].get<bool>() ? 1.0f : 0.0f;
            if (l.contains("radius") && l["radius"].is_number()) b.values[1] = l["radius"].get<float>();
            if (l.contains("intensity") && l["intensity"].is_number()) b.values[2] = l["intensity"].get<float>();
            if (l.contains("falloff") && l["falloff"].is_string()) {
                const std::string f = l["falloff"].get<std::string>();
                b.values[3] = f == "linear" || f == "Linear" || f == "lineal" ? 1.0f : 0.0f;
            }
            if (l.contains("exponent") && l["exponent"].is_number()) b.values[4] = l["exponent"].get<float>();
            if (l.contains("max_lights") && l["max_lights"].is_number()) b.values[5] = l["max_lights"].get<float>();
            if (l.contains("priority") && l["priority"].is_string()) {
                const std::string p = l["priority"].get<std::string>();
                b.values[6] = p == "distance" || p == "Distance" || p == "camera" ? 1.0f : 0.0f;
            }
            if (l.contains("affects_translucent") && l["affects_translucent"].is_boolean()) {
                b.values[7] = l["affects_translucent"].get<bool>() ? 1.0f : 0.0f;
            }
        }
        const std::filesystem::path file = assetFile(project_.assetsFolder(), str("folder", "Efectos"), str("name", vfx::vfxPresetName(preset)),
                                                     vfx::kVfxExtension);
        std::string save_error;
        if (!vfx::saveVfxGraph(graph, file, &save_error)) {
            error = save_error;
            return {};
        }
        refreshDatabase();
        vfx_.reloadGraphs();
        json out{{"file", assetRelative(file)}, {"uuid", graph.uuid.toString()}};
        if (ecs::Entity e = entityOf("attach_to"); e.valid()) {
            vfx::VisualEffect& c = e.has<vfx::VisualEffect>() ? e.get<vfx::VisualEffect>() : e.add<vfx::VisualEffect>();
            c.graph = assets::AssetRef{graph.uuid, assets::AssetType::VisualEffect};
            commit();
            out["attached_to"] = e.name();
        }
        return out.dump();
    }
    if (name == "vfx_control") {
        // Estadisticas por tipo de efecto y la tabla de la calidad (sin entidad).
        if (str("action") == "stats") {
            const vfx::VfxSystemStats s = vfx_.stats();
            json types = json::object();
            for (int i = 0; i < vfx::kEffectTypeSlots; ++i) {
                const vfx::VfxTypeStats& t = s.types[static_cast<std::size_t>(i)];
                json row{{"instances", t.instances}, {"culled", t.culled}, {"particles", t.particles},
                         {"cpu_ms", t.cpu_ms}, {"gpu_ms_estimate", t.gpu_ms}};
                if (i < vfx::kEffectTypes) {
                    const vfx::EffectTypeBudget& b = vfx_.scalability().types[static_cast<std::size_t>(i)];
                    row["budget"] = {{"max_distance", b.max_distance}, {"max_instances", b.max_instances},
                                     {"particle_scale", b.particle_scale}, {"disabled_outputs", b.disabled_outputs},
                                     {"policy", vfx::overBudgetKey(b.policy)}};
                }
                types[vfx::effectTypeKey(static_cast<vfx::EffectType>(i))] = row;
            }
            return json{{"quality", s.quality}, {"effects", s.effects}, {"alive", s.alive}, {"parked_slots", s.parked},
                        {"one_shots", s.one_shots}, {"time_scale", vfx_.timeScale()}, {"types", types}}
                .dump();
        }
        // Un efecto suelto (pool de instancias): effect = UUID del .crvfx, position [x,y,z].
        if (str("action") == "play_at") {
            const Uuid graph_uuid = Uuid::parse(str("effect"));
            core::Vec3 position{};
            if (args.contains("position") && args["position"].is_array() && args["position"].size() >= 3) {
                position = core::Vec3{args["position"][0].get<float>(), args["position"][1].get<float>(),
                                      args["position"][2].get<float>()};
            }
            const bool played = graph_uuid.valid() && vfx_.playAt(graph_uuid, position);
            return json{{"played", played}}.dump();
        }
        // Tiempo de los efectos (0 = pausa, < 1 camara lenta).
        if (str("action") == "time_scale") {
            if (args.contains("value") && args["value"].is_number()) vfx_.setTimeScale(args["value"].get<float>());
            return json{{"time_scale", vfx_.timeScale()}}.dump();
        }
        ecs::Entity e = entityOf("entity");
        if (!e.valid()) {
            error = "no existe la entidad";
            return {};
        }
        const std::string action = str("action", "play");
        if (action == "play") vfx_.play(e);
        else if (action == "stop") vfx_.stop(e, args.value("clear", false));
        else if (action == "event") vfx_.sendEvent(e, str("event"));
        else if (action == "set") {
            const std::string param = str("param");
            const json& v = args.contains("value") ? args["value"] : json();
            if (v.is_number()) vfx_.setFloat(e, param, v.get<float>());
            else if (v.is_boolean()) vfx_.setBool(e, param, v.get<bool>());
            else if (v.is_array() && v.size() >= 4) vfx_.setColor(e, param, core::Vec4{v[0].get<float>(), v[1].get<float>(), v[2].get<float>(), v[3].get<float>()});
            else if (v.is_array() && v.size() >= 3) vfx_.setVector(e, param, core::Vec3{v[0].get<float>(), v[1].get<float>(), v[2].get<float>()});
        }
        return json{{"playing", vfx_.isPlaying(e)}, {"alive", vfx_.aliveCount(e)}}.dump();
    }
    if (name == "vfx_push_event") {
        // Canal de datos (como Effects.pushEvent): lo dibujan el frame
        // siguiente los efectos que escuchan ese canal.
        const std::string channel = str("channel");
        if (channel.empty()) {
            error = "falta 'channel'";
            return {};
        }
        const auto number = [&](const char* key, float fallback) {
            return args.contains(key) && args[key].is_number() ? args[key].get<float>() : fallback;
        };
        const auto vec = [&](const char* key, core::Vec4 fallback) {
            if (!args.contains(key) || !args[key].is_array()) return fallback;
            const json& a = args[key];
            float* parts[4] = {&fallback.x, &fallback.y, &fallback.z, &fallback.w};
            for (std::size_t i = 0; i < a.size() && i < 4; ++i) {
                if (a[i].is_number()) *parts[i] = a[i].get<float>();
            }
            return fallback;
        };
        gfx::VfxEventRecord event;
        event.type = std::max(std::round(number("type", 0.0f)), 0.0f);
        const core::Vec4 p = vec("position", core::Vec4{});
        const core::Vec4 v = vec("velocity", core::Vec4{});
        event.position = core::Vec3{p.x, p.y, p.z};
        event.velocity = core::Vec3{v.x, v.y, v.z};
        event.color = vec("color", core::Vec4{1.0f, 1.0f, 1.0f, 1.0f});
        event.value = number("value", 1.0f);
        const int count = std::clamp(static_cast<int>(number("count", 1.0f)), 1, static_cast<int>(gfx::VfxPass::kMaxChannelEvents));
        int pushed = 0;
        for (int i = 0; i < count; ++i) pushed += vfx_.pushGpuEvent(channel, event) ? 1 : 0;
        if (pushed == 0) {
            error = "no caben mas canales de datos (maximo " + std::to_string(gfx::VfxPass::kMaxChannels) + ")";
            return {};
        }
        return json{{"channel", channel}, {"channel_id", vfx_.channelId(channel)}, {"pushed", pushed},
                    {"pending", vfx_.pendingGpuEvents()}}.dump();
    }
    if (name == "create_visual_script") {
        vscript::Graph graph = vscript::exampleGraph();
        if (args.contains("graph") && args["graph"].is_object()) {
            std::string parse_error;
            if (!vscript::graphFromJson(args["graph"].dump(), graph, &parse_error)) {
                error = "graph: " + parse_error;
                return {};
            }
        }
        if (!graph.uuid.valid()) graph.uuid = Uuid::generate();
        const std::filesystem::path file = assetFile(project_.assetsFolder(), str("folder", "Scripts"), str("name", "VisualScript"),
                                                     vscript::kGraphExtension);
        std::string save_error;
        if (!vscript::saveGraph(graph, file, &save_error)) {
            error = save_error;
            return {};
        }
        refreshDatabase();
        const vscript::CompileResult compiled = vscript::compileGraph(graph, assetRelative(file));
        json errors = json::array();
        for (const vscript::NodeError& ne : compiled.errors) errors.push_back(json{{"node", ne.node}, {"message", ne.message}});
        json out{{"file", assetRelative(file)}, {"compiles", compiled.ok}, {"errors", errors}};
        if (ecs::Entity e = entityOf("attach_to"); e.valid()) {
            vscript::VisualScript& c = e.has<vscript::VisualScript>() ? e.get<vscript::VisualScript>() : e.add<vscript::VisualScript>();
            c.graph = assetRelative(file);
            commit();
            out["attached_to"] = e.name();
        }
        return out.dump();
    }
    if (name == "create_shader_graph") {
        namespace sg = assets::shadergraph;
        sg::Graph graph = sg::makeDefault();
        if (args.contains("graph") && args["graph"].is_object()) {
            std::string parse_error;
            if (!sg::parse(args["graph"].dump(), graph, &parse_error)) {
                error = "graph: " + parse_error;
                return {};
            }
        }
        graph.normalize();
        const std::filesystem::path file = assetFile(project_.assetsFolder(), str("folder", "Shaders"), str("name", "ShaderGraph"),
                                                     assets::kShaderGraphExtension);
        if (!sg::save(graph, file)) {
            error = "no se pudo guardar " + assetRelative(file);
            return {};
        }
        sg::GenerateResult result;
        std::string gen_error;
        sg::writeGeneratedShader(graph, file, result, &gen_error);
        refreshDatabase();
        if (sync_) sync_->reloadSurfaceShaders();
        return json{{"file", assetRelative(file)}, {"shader", assetRelative(sg::generatedShaderPath(file))}, {"errors", result.errors},
                    {"code", result.code}}
            .dump();
    }
    if (name == "create_behavior_tree") {
        ai::BehaviorTreeAsset tree = args.value("example", false) ? ai::exampleGuardBehaviorTree() : ai::BehaviorTreeAsset{};
        if (args.contains("tree") && args["tree"].is_object()) {
            std::string parse_error;
            if (!ai::behaviorTreeFromJson(args["tree"].dump(), tree, &parse_error)) {
                error = "tree: " + parse_error;
                return {};
            }
        }
        tree.ensureRoot();
        if (!tree.uuid.valid()) tree.uuid = Uuid::generate();
        const std::filesystem::path file = assetFile(project_.assetsFolder(), str("folder", "IA"), str("name", "Arbol"), ai::kBehaviorTreeExtension);
        std::string save_error;
        if (!ai::saveBehaviorTree(tree, file, &save_error)) {
            error = save_error;
            return {};
        }
        refreshDatabase();
        json out{{"file", assetRelative(file)}, {"uuid", tree.uuid.toString()}, {"problems", ai::validateBehaviorTree(tree)}};
        if (ecs::Entity e = entityOf("attach_to"); e.valid()) {
            ai::BehaviorTree& c = e.has<ai::BehaviorTree>() ? e.get<ai::BehaviorTree>() : e.add<ai::BehaviorTree>();
            c.tree = assets::AssetRef{tree.uuid, assets::AssetType::BehaviorTree};
            commit();
            out["attached_to"] = e.name();
        }
        return out.dump();
    }
    error = "herramienta desconocida: " + name;
    return {};
}

}  // namespace cramion::editor
