// IntelliSense de Lua: lo que el editor le cuenta al autocompletado.
//   - La API del motor (una vez): ScriptSystem::apiReference.
//   - Lo del proyecto (cada pocos segundos mientras se escribe): componentes y
//     sus campos, acciones y contextos de entrada, teclas, tags, objetos de la
//     escena, escenas, prefabs, materiales, sonidos, texturas y scripts.

#include "EditorApp.h"

#include "Dialogs.h"
#include "LuaCompletion.h"

#include <imgui.h>

#include <algorithm>
#include <set>

namespace cramion::editor {

namespace {

// Las claves de las propiedades de un componente (las de getField/setField).
class FieldCollector : public ecs::PropertyVisitor {
public:
    std::vector<std::pair<std::string, std::string>> fields;

    bool field(const ecs::Meta& m, float&, const ecs::FloatRange&) override { return add(m, "numero"); }
    bool field(const ecs::Meta& m, int&, int, int) override { return add(m, "entero"); }
    bool field(const ecs::Meta& m, bool&) override { return add(m, "true/false"); }
    bool field(const ecs::Meta& m, std::string&) override { return add(m, "texto"); }
    bool field(const ecs::Meta& m, core::Vec3&, ecs::Vec3Kind) override { return add(m, "Vec3"); }
    bool field(const ecs::Meta& m, core::Vec2&, float) override { return add(m, "{x, y}"); }
    bool enumeration(const ecs::Meta& m, int&, std::span<const char* const> names) override {
        std::string choices;
        for (const char* n : names) choices += (choices.empty() ? "" : ", ") + std::string("\"") + n + "\"";
        return add(m, choices.c_str());
    }
    bool asset(const ecs::Meta& m, assets::AssetRef&, assets::AssetType) override { return add(m, "asset (UUID)"); }

private:
    std::set<std::string> seen_;
    bool add(const ecs::Meta& m, const char* type) {
        if (m.key == nullptr || !seen_.insert(m.key).second) return false;
        std::string detail = std::string(m.label != nullptr ? m.label : m.key) + "  -  " + type;
        if (m.tooltip != nullptr) detail += ". " + std::string(m.tooltip);
        fields.emplace_back(m.key, detail);
        return false;
    }
};

const std::vector<LuaProjectSymbols::Component>& componentSymbols() {
    static std::vector<LuaProjectSymbols::Component> list;
    static std::size_t registered = 0;
    const std::vector<ecs::ComponentType>& types = ecs::ComponentRegistry::instance().types();
    if (registered == types.size()) return list;
    registered = types.size();
    list.clear();
    for (const ecs::ComponentType& type : types) {
        LuaProjectSymbols::Component c{type.name, type.label, {}};
        ecs::World world;
        ecs::Entity e = world.create("x");
        if (!type.has(world, e.handle())) type.add(world, e.handle());
        FieldCollector collector;
        type.reflect(world, e.handle(), collector);
        c.fields = std::move(collector.fields);
        list.push_back(std::move(c));
    }
    return list;
}

}  // namespace

void EditorApp::refreshLuaSymbols() {
    static bool api_loaded = false;
    if (!api_loaded) {
        api_loaded = true;
        std::map<std::string, std::vector<LuaApiMember>> reference;
        for (const auto& [owner, members] : scripting::ScriptSystem::apiReference()) {
            for (const auto& m : members) reference[owner].push_back(LuaApiMember{m.name, m.function});
        }
        setLuaApiReference(reference);
    }
    const double now = ImGui::GetTime();
    if (lua_symbols_time_ >= 0.0 && now - lua_symbols_time_ < 3.0) return;
    lua_symbols_time_ = now;

    LuaProjectSymbols s;
    s.components = componentSymbols();
    loadInputActions();
    for (const input::InputAction& a : input_actions_.actions) s.actions.push_back(a.name);
    for (const input::MappingContext& c : input_actions_.contexts) s.contexts.push_back(c.name);
    for (const input::SourceGroup& g : input::sourceGroups()) {
        s.input_sources.insert(s.input_sources.end(), g.names.begin(), g.names.end());
        if (std::string_view(g.title) == "Teclado") s.keys = g.names;
    }
    s.tags = ecs::projectTags();
    std::set<std::string> names;
    world_.forEachDepthFirst([&](ecs::Entity e) { names.insert(e.name()); });
    s.entities.assign(names.begin(), names.end());

    // Assets: los de la base de datos y, por extension, los que no importa.
    if (has_project_) {
        const std::filesystem::path root = project_.assetsFolder();
        const auto relative = [&](const std::filesystem::path& file, bool keep_extension) {
            std::filesystem::path p = file.is_absolute() ? std::filesystem::relative(file, root) : file;
            if (!keep_extension) p.replace_extension();
            std::string text = dialogs::utf8(p);
            std::replace(text.begin(), text.end(), '\\', '/');
            return text;
        };
        if (database_) {
            for (const assets::AssetInfo& info : database_->all()) {
                if (info.path.empty()) continue;
                switch (info.type) {
                    case assets::AssetType::Scene: s.scenes.push_back(dialogs::utf8(info.path.stem())); break;
                    case assets::AssetType::Prefab: s.prefabs.push_back(relative(info.path, false)); break;
                    case assets::AssetType::Material: s.materials.push_back(relative(info.path, true)); break;
                    default: break;
                }
            }
        }
        std::error_code e;
        int budget = 20000;  // proyectos enormes: no se recorre todo cada vez
        for (std::filesystem::recursive_directory_iterator it(root, e), end; !e && it != end && --budget > 0; it.increment(e)) {
            if (!it->is_regular_file(e)) continue;
            std::string ext = dialogs::utf8(it->path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
                s.audio.push_back(relative(it->path(), true));
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
                s.textures.push_back(relative(it->path(), true));
            } else if (ext == ".lua") {
                s.scripts.push_back(relative(it->path(), true));
            }
        }
    }
    for (std::vector<std::string>* list : {&s.scenes, &s.prefabs, &s.materials, &s.audio, &s.textures, &s.scripts}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    setLuaProjectSymbols(std::move(s));
}

}  // namespace cramion::editor
