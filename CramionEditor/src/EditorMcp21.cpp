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
        configureLightingBake(args.value("rays", -1), args.value("bounces", -1), args.value("spacing", -1.0f));
        startLightingBake();
        return lightingStateJson();
    }
    if (name == "lighting_state") return lightingStateJson();

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
