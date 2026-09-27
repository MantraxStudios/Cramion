#include "CramionCore/scripting/Scripting.h"

#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/ui/UI.h"
#include "CramionCore/voxel/Voxel.h"

#include <CramionDM/Input.h>

#include "LuaMath.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <variant>

namespace cramion::scripting {

using core::Vec3;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool readText(const std::filesystem::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Entidad vista desde Lua: el handle y el mundo (puede dejar de existir).
struct LuaEntity {
    entt::entity handle = entt::null;
    ecs::World* world = nullptr;

    ecs::Entity get() const {
        return world != nullptr && world->registry().valid(handle) ? world->wrap(handle) : ecs::Entity{};
    }
    bool valid() const { return get().valid(); }
};

std::string vecString(const Vec3& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return buffer;
}

// --- Graphics.post: los campos del post-procesado por su clave ---
// Recorre la reflexion del componente PostProcessing (la misma del
// Inspector y de los .crscene) para leer o cambiar un campo por su clave
// ("bloom", "bloom_intensity", "tonemapper"...). Sin las opciones del
// volumen (forma, prioridad...) ni las casillas de sobrescribir: Graphics.post
// es el aspecto de la escena, no el volumen.
struct PostValue {
    enum class Type { None, Bool, Number, Text, Vector } type = Type::None;
    bool flag = false;
    double number = 0.0;
    std::string text;
    Vec3 vector{};
    std::vector<std::string> choices;  // enumeraciones
};

class PostFieldVisitor final : public ecs::PropertyVisitor {
public:
    enum class Mode { Collect, Get, Set };

    PostFieldVisitor(Mode mode, std::string key = {}, PostValue value = {})
        : mode_(mode), key_(std::move(key)), value_(std::move(value)) {}

    bool beginGroup(const char* label, bool) override {
        skipping_ = std::string(label) == "Volumen";
        return true;
    }
    void endGroup() override { skipping_ = false; }

    bool field(const ecs::Meta& meta, float& v, const ecs::FloatRange& range) override {
        PostValue current;
        current.type = PostValue::Type::Number;
        current.number = v;
        if (!visit(meta, current)) return false;
        float next = static_cast<float>(value_.number);
        if (range.min != range.max) next = std::clamp(next, range.min, range.max);
        if (next == v) return false;
        v = next;
        return true;
    }
    bool field(const ecs::Meta& meta, int& v, int min, int max) override {
        PostValue current;
        current.type = PostValue::Type::Number;
        current.number = v;
        if (!visit(meta, current)) return false;
        int next = static_cast<int>(std::lround(value_.number));
        if (min != max) next = std::clamp(next, min, max);
        if (next == v) return false;
        v = next;
        return true;
    }
    bool field(const ecs::Meta& meta, bool& v) override {
        PostValue current;
        current.type = PostValue::Type::Bool;
        current.flag = v;
        if (!visit(meta, current) || value_.flag == v) return false;
        v = value_.flag;
        return true;
    }
    bool field(const ecs::Meta& meta, std::string& v) override {
        PostValue current;
        current.type = PostValue::Type::Text;
        current.text = v;
        if (!visit(meta, current) || value_.text == v) return false;
        v = value_.text;
        return true;
    }
    bool field(const ecs::Meta& meta, Vec3& v, ecs::Vec3Kind) override {
        PostValue current;
        current.type = PostValue::Type::Vector;
        current.vector = v;
        if (!visit(meta, current)) return false;
        v = value_.vector;
        return true;
    }
    bool field(const ecs::Meta& meta, core::Vec2& v, float) override {
        PostValue current;
        current.type = PostValue::Type::Vector;
        current.vector = Vec3{v.x, v.y, 0.0f};
        if (!visit(meta, current)) return false;
        v = core::Vec2{value_.vector.x, value_.vector.y};
        return true;
    }
    bool enumeration(const ecs::Meta& meta, int& v, std::span<const char* const> names) override {
        PostValue current;
        current.type = PostValue::Type::Text;
        current.text = v >= 0 && v < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(v)] : "";
        for (const char* name : names) current.choices.emplace_back(name);
        if (!visit(meta, current)) return false;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (lower(names[i]) == lower(value_.text)) {
                if (static_cast<int>(i) == v) return false;
                v = static_cast<int>(i);
                return true;
            }
        }
        error_ = "'" + value_.text + "' no es un valor de " + key_;
        return false;
    }
    bool asset(const ecs::Meta&, assets::AssetRef&, assets::AssetType) override { return false; }

    // Collect: clave -> valor de todos los campos, en orden.
    const std::vector<std::pair<std::string, PostValue>>& fields() const { return fields_; }
    bool found() const { return found_; }
    const PostValue& value() const { return value_; }
    const std::string& error() const { return error_; }

private:
    // true si hay que escribir `value_` en el campo (modo Set y es su clave).
    bool visit(const ecs::Meta& meta, PostValue& current) {
        if (skipping_ || meta.key == nullptr) return false;
        const std::string key = meta.key;
        if (key.rfind("override_", 0) == 0) return false;
        switch (mode_) {
            case Mode::Collect:
                fields_.emplace_back(key, current);
                return false;
            case Mode::Get:
                if (key == key_) {
                    found_ = true;
                    value_ = current;
                }
                return false;
            case Mode::Set:
                if (key != key_) return false;
                found_ = true;
                if (!convert(current)) return false;
                return true;
        }
        return false;
    }

    // Adapta el valor de Lua al tipo del campo (un numero vale para una
    // casilla, true/false para un numero...).
    bool convert(const PostValue& target) {
        using T = PostValue::Type;
        if (target.type == value_.type) return true;
        if (target.type == T::Bool && value_.type == T::Number) {
            value_.flag = value_.number != 0.0;
            return true;
        }
        if (target.type == T::Number && value_.type == T::Bool) {
            value_.number = value_.flag ? 1.0 : 0.0;
            return true;
        }
        if (target.type == T::Vector && value_.type == T::Number) {
            const float n = static_cast<float>(value_.number);
            value_.vector = Vec3{n, n, n};
            return true;
        }
        if (target.type == T::Text && value_.type == T::Number && !target.choices.empty()) {
            const int index = static_cast<int>(value_.number);
            if (index >= 0 && index < static_cast<int>(target.choices.size())) {
                value_.text = target.choices[static_cast<std::size_t>(index)];
                return true;
            }
        }
        error_ = "tipo de valor equivocado para " + key_;
        return false;
    }

    Mode mode_;
    std::string key_;
    PostValue value_;
    bool skipping_ = false;
    bool found_ = false;
    std::string error_;
    std::vector<std::pair<std::string, PostValue>> fields_;
};

}  // namespace

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

void Script::reflect(ecs::PropertyVisitor& v) {
    v.field({"file", "Script", "Archivo .lua en Assets"}, file);
    v.field({"enabled", "Activo"}, enabled);
    // El Inspector los dibuja aparte (con el control de su tipo).
    if (v.wantsAllFields()) {
        ecs::listField(v, {"properties", "Propiedades"}, properties, [](ScriptProperty& p, ecs::PropertyVisitor& item) {
            item.field({"name", "Nombre"}, p.name);
            int type = static_cast<int>(p.type);
            item.field({"type", "Tipo"}, type, 0, 3);
            p.type = static_cast<PropertyType>(std::clamp(type, 0, 3));
            item.field({"value", "Valor"}, p.value);
        });
    }
}

std::string scriptTemplate(const std::string& class_name) {
    std::string name;
    for (char c : class_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') name += c;
    }
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) name = "Script" + name;
    return "-- " + name + ".lua\n"
           "-- Se engancha a un objeto con el componente Script (o arrastrandolo a el).\n"
           "local " + name + " = {\n"
           "    -- Propiedades que se editan en el Inspector (numero, true/false, texto o Vec3).\n"
           "    properties = {\n"
           "        velocidad = 5.0,\n"
           "    },\n"
           "}\n\n"
           "-- Una vez, antes del primer Update.\n"
           "function " + name + ":Start()\n"
           "end\n\n"
           "-- Cada frame. dt = segundos desde el anterior.\n"
           "function " + name + ":Update(dt)\n"
           "    local mover = Vec3(Input.getAxis(\"Horizontal\"), 0, Input.getAxis(\"Vertical\"))\n"
           "    self.entity:translate(mover * self.velocidad * dt)\n"
           "end\n\n"
           "-- Choques (el objeto necesita collider; uno de los dos, Rigidbody).\n"
           "function " + name + ":OnCollisionEnter(other, contact)\n"
           "end\n\n"
           "return " + name + "\n";
}

void registerScriptComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("Script") == nullptr) {
        registry.registerComponent<Script>("Script", "Script (Lua)", "Scripting");
    }
}

// -----------------------------------------------------------------------------
// Sistema
// -----------------------------------------------------------------------------

struct ScriptSystem::Impl {
    std::filesystem::path root;
    const dm::Input* input = nullptr;
    physics::PhysicsSystem* physics = nullptr;
    audio::AudioSystem* audio = nullptr;
    navigation::NavigationSystem* navigation = nullptr;
    voxel::VoxelSystem* voxels = nullptr;
    GraphicsHost* graphics = nullptr;
    CursorLockCallback cursor_lock;
    bool cursor_locked = false;
    LogCallback log;
    std::vector<ScriptError> errors;

    std::unique_ptr<sol::state> lua;
    ecs::World* world = nullptr;
    bool running = false;
    float time = 0.0f;
    std::uint64_t frame = 0;
    int listener = -1;
    std::vector<physics::PhysicsEvent> events;
    std::vector<entt::entity> pending_destroy;

    struct Instance {
        std::string file;
        sol::table self;
        bool started = false;
        bool failed = false;
    };
    std::unordered_map<entt::entity, Instance> instances;
    std::unordered_map<std::string, sol::table> classes;
    std::unordered_map<std::string, std::string> failed_classes;  // archivo -> error

    struct DescribeCache {
        std::filesystem::file_time_type stamp{};
        std::vector<ScriptProperty> properties;
        std::string error;
    };
    std::unordered_map<std::string, DescribeCache> described;

    std::unordered_map<std::string, dm::Key> keys;

    // Cambio de escena y salida pedidos desde Lua; datos guardados (Prefs).
    std::filesystem::path scene_request;
    bool quit_request = false;
    std::string scene_name;
    std::map<std::string, std::string> prefs;
    std::filesystem::path prefs_file;

    void savePrefs() const {
        if (prefs_file.empty()) return;
        std::error_code e;
        std::filesystem::create_directories(prefs_file.parent_path(), e);
        std::ofstream out(prefs_file, std::ios::trunc);
        for (const auto& [key, value] : prefs) {
            if (key.find_first_of("=\n") != std::string::npos || value.find('\n') != std::string::npos) continue;
            out << key << '=' << value << '\n';
        }
    }

    // "Nivel2" o "Escenas/Nivel2.crscene" -> ruta del .crscene en Assets.
    std::filesystem::path findScene(const std::string& name) const {
        if (name.empty() || root.empty()) return {};
        std::filesystem::path direct = root / std::filesystem::path(std::u8string(name.begin(), name.end()));
        if (direct.extension() != ".crscene") direct += ".crscene";
        std::error_code e;
        if (std::filesystem::is_regular_file(direct, e)) return direct;
        std::string wanted = std::filesystem::path(std::u8string(name.begin(), name.end())).stem().string();
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, e);
             !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
            if (it->path().extension() != ".crscene") continue;
            std::string stem = it->path().stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (stem == wanted) return it->path();
        }
        return {};
    }

    // Prefabs leidos (se relee si el archivo cambia).
    struct PrefabFile {
        std::filesystem::file_time_type stamp{};
        std::string text;
    };
    std::unordered_map<std::string, PrefabFile> prefabs;
    std::string prefab_cache(const std::filesystem::path& file) {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (ec) return {};
        PrefabFile& entry = prefabs[file.string()];
        if (entry.text.empty() || entry.stamp != stamp) {
            entry.text = ecs::readPrefabFile(file);
            entry.stamp = stamp;
        }
        return entry.text;
    }

    // --- Mensajes y errores ---
    void write(int level, const std::string& message) {
        if (log) {
            log(level, message);
        } else if (level == 0) {
            std::cout << "[Lua] " << message << std::endl;
        } else {
            std::cerr << "[Lua] " << message << std::endl;
        }
    }

    void fail(const std::string& file, const std::string& message) {
        ScriptError error;
        error.file = file;
        error.message = message;
        // "Scripts/Jugador.lua:12: mensaje" -> linea 12.
        const std::string key = file + ":";
        const std::size_t at = message.find(key);
        if (at != std::string::npos) {
            error.line = std::atoi(message.c_str() + at + key.size());
        }
        if (errors.size() > 64) errors.erase(errors.begin());
        errors.push_back(error);
        write(2, message);
    }

    // --- Teclas por nombre ("W", "Space", "LeftShift", "Up"...) ---
    void buildKeys() {
        for (int k = 1; k < static_cast<int>(dm::Key::Count); ++k) {
            const char* name = dm::keyName(static_cast<dm::Key>(k));
            if (name != nullptr && name[0] != '\0') keys[lower(name)] = static_cast<dm::Key>(k);
        }
        const std::pair<const char*, dm::Key> aliases[] = {
            {"space", dm::Key::Space},        {"enter", dm::Key::Enter},        {"return", dm::Key::Enter},
            {"escape", dm::Key::Escape},      {"esc", dm::Key::Escape},         {"tab", dm::Key::Tab},
            {"shift", dm::Key::LeftShift},    {"leftshift", dm::Key::LeftShift}, {"rightshift", dm::Key::RightShift},
            {"ctrl", dm::Key::LeftControl},   {"control", dm::Key::LeftControl}, {"alt", dm::Key::LeftAlt},
            {"left", dm::Key::Left},          {"right", dm::Key::Right},        {"up", dm::Key::Up},
            {"down", dm::Key::Down},          {"backspace", dm::Key::Backspace}, {"delete", dm::Key::Delete},
        };
        for (const auto& [name, key] : aliases) keys[name] = key;
        for (char c = 'a'; c <= 'z'; ++c) {
            keys[std::string(1, c)] = static_cast<dm::Key>(static_cast<int>(dm::Key::A) + (c - 'a'));
        }
        for (char c = '0'; c <= '9'; ++c) {
            keys[std::string(1, c)] = static_cast<dm::Key>(static_cast<int>(dm::Key::Num0) + (c - '0'));
        }
        for (int f = 1; f <= 12; ++f) {
            keys["f" + std::to_string(f)] = static_cast<dm::Key>(static_cast<int>(dm::Key::F1) + f - 1);
        }
    }

    dm::Key key(const std::string& name) const {
        const auto it = keys.find(lower(name));
        return it != keys.end() ? it->second : dm::Key::Unknown;
    }

    bool keyDown(const std::string& name) const {
        const dm::Key k = key(name);
        return input != nullptr && k != dm::Key::Unknown && input->isKeyDown(k);
    }

    float axis(const std::string& name) const {
        if (input == nullptr) return 0.0f;
        const std::string n = lower(name);
        if (n == "horizontal") {
            return (keyDown("d") || keyDown("right") ? 1.0f : 0.0f) - (keyDown("a") || keyDown("left") ? 1.0f : 0.0f);
        }
        if (n == "vertical") {
            return (keyDown("w") || keyDown("up") ? 1.0f : 0.0f) - (keyDown("s") || keyDown("down") ? 1.0f : 0.0f);
        }
        if (n == "mouse x") return input->mouseDeltaX() * 0.1f;
        if (n == "mouse y") return -input->mouseDeltaY() * 0.1f;
        if (n == "mouse scrollwheel") return input->scrollY();
        return 0.0f;
    }

    // --- API de Lua ---
    void bind(sol::state& L) {
        L.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::coroutine,
                         sol::lib::utf8, sol::lib::os);
        // Sin acceso al sistema desde los scripts del juego.
        sol::table os = L["os"];
        for (const char* name : {"execute", "exit", "remove", "rename", "tmpname", "getenv", "setlocale"}) {
            os[name] = sol::lua_nil;
        }
        L["dofile"] = sol::lua_nil;
        L["loadfile"] = sol::lua_nil;

        // Vec3, Quat, Mathf y Random (LuaMath.cpp)
        bindMath(L);

        // Entity
        auto entity = L.new_usertype<LuaEntity>(
            "Entity", sol::no_constructor,
            "valid", &LuaEntity::valid,
            "name", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.name() : std::string{}; },
                                  [](LuaEntity& e, const std::string& n) { if (auto x = e.get(); x.valid()) x.setName(n); }),
            "tag", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.tag() : std::string{}; },
                                 [](LuaEntity& e, const std::string& t) { if (auto x = e.get(); x.valid()) x.setTag(t); }),
            "active", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() && x.activeSelf(); },
                                    [](LuaEntity& e, bool a) { if (auto x = e.get(); x.valid()) x.setActive(a); }),
            "position", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.worldPosition() : Vec3{}; },
                                      [](LuaEntity& e, const Vec3& p) { if (auto x = e.get(); x.valid()) x.setWorldPosition(p); }),
            "localPosition", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.localPosition() : Vec3{}; },
                                           [](LuaEntity& e, const Vec3& p) { if (auto x = e.get(); x.valid()) x.setLocalPosition(p); }),
            "rotation", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.localEulerDegrees() : Vec3{}; },
                                      [](LuaEntity& e, const Vec3& r) { if (auto x = e.get(); x.valid()) x.setLocalEulerDegrees(r); }),
            "scale", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.localScale() : Vec3{1, 1, 1}; },
                                   [](LuaEntity& e, const Vec3& s) { if (auto x = e.get(); x.valid()) x.setLocalScale(s); }),
            "forward", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.forward() : Vec3{0, 0, -1}; }),
            "right", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.right() : Vec3{1, 0, 0}; }),
            "up", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.up() : Vec3{0, 1, 0}; }),
            "parent", sol::property([this](const LuaEntity& e) -> sol::object {
                const ecs::Entity x = e.get();
                if (!x.valid() || !x.parent().valid()) return sol::lua_nil;
                return sol::make_object(*lua, LuaEntity{x.parent().handle(), world});
            }),
            // Interfaz: el texto (Texto o Campo), el valor (Slider o Casilla) y si responde.
            "text", sol::property(
                [](const LuaEntity& e) -> std::string {
                    const ecs::Entity x = e.get();
                    if (!x.valid()) return {};
                    if (const auto* t = x.tryGet<ui::Text>()) return t->text;
                    if (const auto* f = x.tryGet<ui::InputField>()) return f->text;
                    return {};
                },
                [](LuaEntity& e, const std::string& text) {
                    ecs::Entity x = e.get();
                    if (!x.valid()) return;
                    if (auto* t = x.tryGet<ui::Text>()) t->text = text;
                    if (auto* f = x.tryGet<ui::InputField>()) f->text = text;
                }),
            "value", sol::property(
                [](const LuaEntity& e) -> float {
                    const ecs::Entity x = e.get();
                    if (!x.valid()) return 0.0f;
                    if (const auto* s = x.tryGet<ui::Slider>()) return s->value;
                    if (const auto* t = x.tryGet<ui::Toggle>()) return t->on ? 1.0f : 0.0f;
                    return 0.0f;
                },
                [](LuaEntity& e, float value) {
                    ecs::Entity x = e.get();
                    if (!x.valid()) return;
                    if (auto* s = x.tryGet<ui::Slider>()) s->value = std::clamp(value, std::min(s->min, s->max), std::max(s->min, s->max));
                    if (auto* t = x.tryGet<ui::Toggle>()) t->on = value != 0.0f;
                }),
            "interactable", sol::property(
                [](const LuaEntity& e) {
                    const ecs::Entity x = e.get();
                    if (!x.valid()) return false;
                    if (const auto* b = x.tryGet<ui::Button>()) return b->interactable;
                    if (const auto* s = x.tryGet<ui::Slider>()) return s->interactable;
                    if (const auto* f = x.tryGet<ui::InputField>()) return f->interactable;
                    if (const auto* t = x.tryGet<ui::Toggle>()) return t->interactable;
                    return false;
                },
                [](LuaEntity& e, bool on) {
                    ecs::Entity x = e.get();
                    if (!x.valid()) return;
                    if (auto* b = x.tryGet<ui::Button>()) b->interactable = on;
                    if (auto* s = x.tryGet<ui::Slider>()) s->interactable = on;
                    if (auto* f = x.tryGet<ui::InputField>()) f->interactable = on;
                    if (auto* t = x.tryGet<ui::Toggle>()) t->interactable = on;
                }),
            "color", sol::property(
                [](const LuaEntity& e) -> Vec3 {
                    const ecs::Entity x = e.get();
                    if (!x.valid()) return Vec3{1, 1, 1};
                    if (const auto* i = x.tryGet<ui::Image>()) return i->color;
                    if (const auto* t = x.tryGet<ui::Text>()) return t->color;
                    return Vec3{1, 1, 1};
                },
                [](LuaEntity& e, const Vec3& c) {
                    ecs::Entity x = e.get();
                    if (!x.valid()) return;
                    if (auto* i = x.tryGet<ui::Image>()) i->color = c;
                    if (auto* t = x.tryGet<ui::Text>()) t->color = c;
                }),
            "velocity", sol::property(
                [this](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() && physics != nullptr ? physics->linearVelocity(x) : Vec3{}; },
                [this](LuaEntity& e, const Vec3& v) { if (auto x = e.get(); x.valid() && physics != nullptr) physics->setLinearVelocity(x, v); }),
            "angularVelocity", sol::property(
                [this](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() && physics != nullptr ? physics->angularVelocity(x) : Vec3{}; },
                [this](LuaEntity& e, const Vec3& v) { if (auto x = e.get(); x.valid() && physics != nullptr) physics->setAngularVelocity(x, v); }),
            "translate", [](LuaEntity& e, const Vec3& d) { if (auto x = e.get(); x.valid()) x.setWorldPosition(x.worldPosition() + d); },
            "translateLocal", [](LuaEntity& e, const Vec3& d) {
                if (auto x = e.get(); x.valid()) x.setWorldPosition(x.worldPosition() + x.right() * d.x + x.up() * d.y - x.forward() * d.z);
            },
            "quaternion", sol::property([](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() ? x.localRotation() : core::Quat{}; },
                                        [](LuaEntity& e, const core::Quat& q) { if (auto x = e.get(); x.valid()) x.setLocalRotation(q); }),
            "rotate", [](LuaEntity& e, const Vec3& degrees) { if (auto x = e.get(); x.valid()) x.setLocalEulerDegrees(x.localEulerDegrees() + degrees); },
            "lookAt", [](LuaEntity& e, const Vec3& target) {
                ecs::Entity x = e.get();
                if (!x.valid()) return;
                const Vec3 d = target - x.worldPosition();
                const float len = core::length(d);
                if (len < 1e-5f) return;
                const Vec3 dir = d * (1.0f / len);
                constexpr float kDeg = 57.2957795f;
                x.setLocalEulerDegrees(Vec3{std::asin(std::clamp(dir.y, -1.0f, 1.0f)) * kDeg, std::atan2(-dir.x, -dir.z) * kDeg, 0.0f});
            },
            "distanceTo", [](const LuaEntity& e, const LuaEntity& other) {
                const ecs::Entity a = e.get();
                const ecs::Entity b = other.get();
                return a.valid() && b.valid() ? core::length(a.worldPosition() - b.worldPosition()) : 0.0f;
            },
            "destroy", [this](LuaEntity& e) { if (e.valid()) pending_destroy.push_back(e.handle); },
            "addForce", [this](LuaEntity& e, const Vec3& f, sol::optional<std::string> mode) {
                ecs::Entity x = e.get();
                if (!x.valid() || physics == nullptr) return;
                physics::ForceMode m = physics::ForceMode::Force;
                const std::string name = lower(mode.value_or("force"));
                if (name == "impulse") m = physics::ForceMode::Impulse;
                if (name == "acceleration") m = physics::ForceMode::Acceleration;
                if (name == "velocity" || name == "velocitychange") m = physics::ForceMode::VelocityChange;
                physics->addForce(x, f, m);
            },
            "addImpulse", [this](LuaEntity& e, const Vec3& f) { if (auto x = e.get(); x.valid() && physics != nullptr) physics->addImpulse(x, f); },
            "addTorque", [this](LuaEntity& e, const Vec3& t) { if (auto x = e.get(); x.valid() && physics != nullptr) physics->addTorque(x, t, physics::ForceMode::Force); },
            // Vehiculos (Vehicle + WheelCollider): acelerador -1..1, direccion -1..1, frenos 0..1.
            "setVehicleInput", [this](LuaEntity& e, float throttle, float steering, sol::optional<float> brake, sol::optional<float> handbrake) {
                if (auto x = e.get(); x.valid() && physics != nullptr) {
                    physics->setVehicleInput(x, throttle, steering, brake.value_or(0.0f), handbrake.value_or(0.0f));
                }
            },
            "speed", sol::property([this](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                return x.valid() && physics != nullptr ? physics->vehicleState(x).speed_kmh : 0.0f;
            }),
            "rpm", sol::property([this](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                return x.valid() && physics != nullptr ? physics->vehicleState(x).rpm : 0.0f;
            }),
            "gear", sol::property([this](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                return x.valid() && physics != nullptr ? physics->vehicleState(x).gear : 0;
            }),
            "playSound", [this](LuaEntity& e) { if (auto x = e.get(); x.valid() && audio != nullptr) audio->play(x); },
            "stopSound", [this](LuaEntity& e) { if (auto x = e.get(); x.valid() && audio != nullptr) audio->stop(x); },
            "isPlayingSound", [this](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() && audio != nullptr && audio->isPlaying(x); },
            // Efectos del AudioSource: e:setSoundEffect("lowpass"|"highpass"|"echo"|"reverb"|"occlusion", activo, valor)
            "setSoundEffect", [](LuaEntity& e, const std::string& name, bool enabled, sol::optional<float> value) {
                ecs::Entity x = e.get();
                audio::AudioSource* a = x.valid() ? x.tryGet<audio::AudioSource>() : nullptr;
                if (a == nullptr) return false;
                const std::string n = lower(name);
                if (n == "lowpass") {
                    a->low_pass = enabled;
                    if (value) a->low_pass_cutoff = *value;
                } else if (n == "highpass") {
                    a->high_pass = enabled;
                    if (value) a->high_pass_cutoff = *value;
                } else if (n == "echo") {
                    a->echo = enabled;
                    if (value) a->echo_delay = *value;
                } else if (n == "reverb") {
                    a->reverb_send = enabled ? value.value_or(1.0f) : 0.0f;
                } else if (n == "occlusion") {
                    a->occlusion = enabled;
                } else {
                    return false;
                }
                return true;
            },
            // Paredes que tapan ahora este sonido (-1 si no suena).
            "soundOcclusion", sol::property([this](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                return x.valid() && audio != nullptr ? audio->occlusionOf(x) : -1.0f;
            }),
            // La oclusion del AudioListener de esta entidad (camara): on/off.
            "audioOcclusion", sol::property(
                [](const LuaEntity& e) {
                    const ecs::Entity x = e.get();
                    const audio::AudioListener* l = x.valid() ? x.tryGet<audio::AudioListener>() : nullptr;
                    return l != nullptr && l->occlusion;
                },
                [](LuaEntity& e, bool on) {
                    ecs::Entity x = e.get();
                    if (audio::AudioListener* l = x.valid() ? x.tryGet<audio::AudioListener>() : nullptr) l->occlusion = on;
                }),
            "playAnimation", [](LuaEntity& e, const std::string& clip, sol::optional<bool> loop) {
                ecs::Entity x = e.get();
                if (!x.valid()) return;
                if (ecs::Animator* a = x.tryGet<ecs::Animator>()) {
                    a->clip_name = clip;
                    a->loop = loop.value_or(true);
                    a->playing = true;
                    a->time = 0.0f;
                }
            },
            "setAnimatorFloat", [](LuaEntity& e, const std::string& n, float v) { if (auto x = e.get(); x.valid()) if (auto* a = x.tryGet<ecs::Animator>()) a->setFloat(n, v); },
            "setAnimatorBool", [](LuaEntity& e, const std::string& n, bool v) { if (auto x = e.get(); x.valid()) if (auto* a = x.tryGet<ecs::Animator>()) a->setBool(n, v); },
            "setAnimatorTrigger", [](LuaEntity& e, const std::string& n) { if (auto x = e.get(); x.valid()) if (auto* a = x.tryGet<ecs::Animator>()) a->setTrigger(n); },
            "hasComponent", [](const LuaEntity& e, const std::string& name) {
                const ecs::Entity x = e.get();
                const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(name);
                return x.valid() && type != nullptr && e.world != nullptr && type->has(*e.world, x.handle());
            },
            "getScript", [this](const LuaEntity& e) -> sol::object {
                const auto it = instances.find(e.handle);
                if (it == instances.end()) return sol::lua_nil;
                return it->second.self;
            },
            "find", [this](const LuaEntity& e, const std::string& name) -> sol::object {
                ecs::Entity x = e.get();
                if (!x.valid()) return sol::lua_nil;
                std::vector<ecs::Entity> stack{x};
                while (!stack.empty()) {
                    const ecs::Entity current = stack.back();
                    stack.pop_back();
                    for (const entt::entity child : current.children()) {
                        const ecs::Entity c = world->wrap(child);
                        if (c.name() == name) return sol::make_object(*lua, LuaEntity{child, world});
                        stack.push_back(c);
                    }
                }
                return sol::lua_nil;
            },
            sol::meta_function::equal_to, [](const LuaEntity& a, const LuaEntity& b) { return a.handle == b.handle; },
            sol::meta_function::to_string, [](const LuaEntity& e) { const ecs::Entity x = e.get(); return "Entity(" + (x.valid() ? x.name() : std::string("destruida")) + ")"; });

        // La malla creada por codigo del MeshRenderer (lo anade si no hay).
        entity["mesh"] = sol::property(
            [](const LuaEntity& e) -> std::shared_ptr<ecs::Mesh> {
                const ecs::Entity x = e.get();
                const ecs::MeshRenderer* r = x.valid() ? x.tryGet<ecs::MeshRenderer>() : nullptr;
                return r != nullptr ? r->mesh : nullptr;
            },
            [](LuaEntity& e, sol::optional<std::shared_ptr<ecs::Mesh>> m) {
                ecs::Entity x = e.get();
                if (!x.valid()) return;
                ecs::MeshRenderer* r = x.tryGet<ecs::MeshRenderer>();
                if (r == nullptr) {
                    if (!m || !*m) return;
                    r = &x.add<ecs::MeshRenderer>();
                }
                r->mesh = m ? *m : nullptr;
            });
        // Material .crmat de un hueco del MeshRenderer (submalla), o nil para el suyo.
        entity["setMaterial"] = [this](LuaEntity& e, int slot, sol::optional<std::string> path) {
            ecs::Entity x = e.get();
            ecs::MeshRenderer* r = x.valid() ? x.tryGet<ecs::MeshRenderer>() : nullptr;
            if (r == nullptr || slot < 0) return false;
            assets::AssetRef ref{{}, assets::AssetType::Material};
            if (path && !path->empty()) {
                assets::MaterialAsset m;
                std::filesystem::path file = root / fromUtf8(*path);
                if (file.extension() != ".crmat") file += ".crmat";
                if (!assets::loadMaterial(file, m)) {
                    write(2, "setMaterial: no se puede leer el material \"" + *path + "\"");
                    return false;
                }
                ref.uuid = m.uuid;
            }
            if (r->materials.size() <= static_cast<std::size_t>(slot)) r->materials.resize(static_cast<std::size_t>(slot) + 1);
            r->materials[static_cast<std::size_t>(slot)] = ref;
            return true;
        };
        entity["castShadows"] = sol::property(
            [](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                const ecs::MeshRenderer* r = x.valid() ? x.tryGet<ecs::MeshRenderer>() : nullptr;
                return r != nullptr && r->cast_shadows != ecs::ShadowCasting::Off;
            },
            [](LuaEntity& e, bool on) {
                ecs::Entity x = e.get();
                if (ecs::MeshRenderer* r = x.valid() ? x.tryGet<ecs::MeshRenderer>() : nullptr) {
                    r->cast_shadows = on ? ecs::ShadowCasting::On : ecs::ShadowCasting::Off;
                }
            });
        // Interfaz: la imagen de un UIImage, la transparencia y el rectangulo.
        entity["texture"] = sol::property(
            [](const LuaEntity& e) -> std::string {
                const ecs::Entity x = e.get();
                const ui::Image* i = x.valid() ? x.tryGet<ui::Image>() : nullptr;
                return i != nullptr ? i->texture : std::string();
            },
            [](LuaEntity& e, const std::string& path) {
                ecs::Entity x = e.get();
                if (ui::Image* i = x.valid() ? x.tryGet<ui::Image>() : nullptr) i->texture = path;
            });
        entity["alpha"] = sol::property(
            [](const LuaEntity& e) -> float {
                const ecs::Entity x = e.get();
                if (!x.valid()) return 1.0f;
                if (const auto* i = x.tryGet<ui::Image>()) return i->alpha;
                if (const auto* t = x.tryGet<ui::Text>()) return t->alpha;
                return 1.0f;
            },
            [](LuaEntity& e, float a) {
                ecs::Entity x = e.get();
                if (!x.valid()) return;
                if (auto* i = x.tryGet<ui::Image>()) i->alpha = a;
                if (auto* t = x.tryGet<ui::Text>()) t->alpha = a;
            });
        entity["uiPosition"] = sol::property(
            [](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                const ui::RectTransform* r = x.valid() ? x.tryGet<ui::RectTransform>() : nullptr;
                return r != nullptr ? Vec3{r->position.x, r->position.y, 0.0f} : Vec3{};
            },
            [](LuaEntity& e, const Vec3& p) {
                ecs::Entity x = e.get();
                if (ui::RectTransform* r = x.valid() ? x.tryGet<ui::RectTransform>() : nullptr) r->position = core::Vec2{p.x, p.y};
            });
        entity["uiSize"] = sol::property(
            [](const LuaEntity& e) {
                const ecs::Entity x = e.get();
                const ui::RectTransform* r = x.valid() ? x.tryGet<ui::RectTransform>() : nullptr;
                return r != nullptr ? Vec3{r->size.x, r->size.y, 0.0f} : Vec3{};
            },
            [](LuaEntity& e, const Vec3& s) {
                ecs::Entity x = e.get();
                if (ui::RectTransform* r = x.valid() ? x.tryGet<ui::RectTransform>() : nullptr) r->size = core::Vec2{s.x, s.y};
            });
        // Componentes por su nombre ("MeshCollider", "Rigidbody", "Light"...).
        entity["addComponent"] = [](LuaEntity& e, const std::string& name) {
            ecs::Entity x = e.get();
            const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(name);
            if (!x.valid() || type == nullptr || e.world == nullptr) return false;
            if (!type->has(*e.world, x.handle())) type->add(*e.world, x.handle());
            return true;
        };
        entity["removeComponent"] = [](LuaEntity& e, const std::string& name) {
            ecs::Entity x = e.get();
            const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(name);
            if (!x.valid() || type == nullptr || e.world == nullptr || !type->has(*e.world, x.handle())) return false;
            type->remove(*e.world, x.handle());
            return true;
        };

        // Navegacion (NavAgent): como el MoveTo del AIController de Unreal.
        entity["moveTo"] = [this](LuaEntity& e, const Vec3& target) {
            const ecs::Entity x = e.get();
            return x.valid() && navigation != nullptr && navigation->moveTo(x, target);
        };
        entity["stopMoving"] = [this](LuaEntity& e) {
            if (auto x = e.get(); x.valid() && navigation != nullptr) navigation->stop(x);
        };
        entity["isMoving"] = sol::property([this](const LuaEntity& e) {
            const ecs::Entity x = e.get();
            return x.valid() && navigation != nullptr && navigation->isMoving(x);
        });
        entity["remainingDistance"] = sol::property([this](const LuaEntity& e) {
            const ecs::Entity x = e.get();
            return x.valid() && navigation != nullptr ? navigation->remainingDistance(x) : 0.0f;
        });
        entity["navVelocity"] = sol::property([this](const LuaEntity& e) {
            const ecs::Entity x = e.get();
            return x.valid() && navigation != nullptr ? navigation->agentVelocity(x) : Vec3{};
        });

        // Scene
        sol::table scene = L.create_named_table("Scene");
        scene["find"] = [this](const std::string& name) -> sol::object {
            if (world == nullptr) return sol::lua_nil;
            const ecs::Entity e = world->findByName(name);
            return e.valid() ? sol::make_object(*lua, LuaEntity{e.handle(), world}) : sol::object(sol::lua_nil);
        };
        scene["findWithTag"] = [this](const std::string& tag) -> sol::object {
            if (world == nullptr) return sol::lua_nil;
            const ecs::Entity e = world->findWithTag(tag);
            return e.valid() ? sol::make_object(*lua, LuaEntity{e.handle(), world}) : sol::object(sol::lua_nil);
        };
        scene["findAllWithTag"] = [this](const std::string& tag) {
            sol::table out = lua->create_table();
            if (world != nullptr) {
                int i = 1;
                for (const ecs::Entity& e : world->findAllWithTag(tag)) out[i++] = LuaEntity{e.handle(), world};
            }
            return out;
        };
        scene["create"] = [this](const std::string& name, sol::optional<Vec3> position) -> sol::object {
            if (world == nullptr) return sol::lua_nil;
            ecs::Entity e = world->create(name);
            if (position) e.setWorldPosition(*position);
            return sol::make_object(*lua, LuaEntity{e.handle(), world});
        };
        // Copia de una entidad, o una instancia de un prefab por su ruta en
        // Assets ("Prefabs/Enemigo" o "Prefabs/Enemigo.crprefab").
        scene["instantiate"] = [this](sol::object original, sol::optional<Vec3> position,
                                      sol::optional<Vec3> rotation) -> sol::object {
            if (world == nullptr) return sol::lua_nil;
            ecs::Entity copy;
            if (original.get_type() == sol::type::string) {
                std::filesystem::path file = root / fromUtf8(original.as<std::string>());
                if (file.extension() != ecs::kPrefabExtension) file += ecs::kPrefabExtension;
                const std::string text = prefab_cache(file);
                if (text.empty()) {
                    write(2, "Scene.instantiate: no existe el prefab \"" + original.as<std::string>() + "\"");
                    return sol::lua_nil;
                }
                copy = ecs::instantiatePrefab(*world, text);
            } else if (original.is<LuaEntity>()) {
                const ecs::Entity source = original.as<LuaEntity>().get();
                if (!source.valid()) return sol::lua_nil;
                copy = world->duplicate(source);
                ecs::detachCopiedLinks(*world, copy);
            }
            if (!copy.valid()) return sol::lua_nil;
            if (position) copy.setWorldPosition(*position);
            if (rotation) copy.setLocalEulerDegrees(*rotation);
            return sol::make_object(*lua, LuaEntity{copy.handle(), world});
        };
        scene["destroy"] = [this](const LuaEntity& e) { if (e.valid()) pending_destroy.push_back(e.handle); };
        // Cambiar de escena al terminar el frame (como SceneManager.LoadScene).
        scene["load"] = [this](const std::string& name) {
            const std::filesystem::path path = findScene(name);
            if (path.empty()) {
                write(2, "Scene.load: no existe la escena \"" + name + "\"");
                return false;
            }
            scene_request = path;
            return true;
        };
        scene["name"] = [this]() { return scene_name; };
        // Origen flotante: la posicion real (doble precision; los numeros de
        // Lua son double) del (0,0,0) del mundo, y conversiones. Para guardar
        // posiciones en una partida: Scene.toAbsolute al guardar y
        // Scene.toLocal al cargar (el origen puede ser otro).
        scene["origin"] = [this]() -> std::tuple<double, double, double> {
            if (world == nullptr) return {0.0, 0.0, 0.0};
            const ecs::DVec3& o = world->origin();
            return {o.x, o.y, o.z};
        };
        scene["toAbsolute"] = [this](const Vec3& local) -> std::tuple<double, double, double> {
            if (world == nullptr) return {local.x, local.y, local.z};
            const ecs::DVec3 a = world->absolute(local);
            return {a.x, a.y, a.z};
        };
        scene["toLocal"] = [this](double x, double y, double z) -> Vec3 {
            if (world == nullptr) return Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
            return world->local(ecs::DVec3{x, y, z});
        };

        // Prefs (PlayerPrefs): numeros y textos que sobreviven al cambiar de
        // escena y se guardan en disco.
        sol::table prefs_table = L.create_named_table("Prefs");
        prefs_table["setInt"] = [this](const std::string& k, int v) { prefs[k] = std::to_string(v); savePrefs(); };
        prefs_table["getInt"] = [this](const std::string& k, sol::optional<int> fallback) {
            const auto it = prefs.find(k);
            return it != prefs.end() ? std::atoi(it->second.c_str()) : fallback.value_or(0);
        };
        prefs_table["setFloat"] = [this](const std::string& k, float v) { prefs[k] = std::to_string(v); savePrefs(); };
        prefs_table["getFloat"] = [this](const std::string& k, sol::optional<float> fallback) {
            const auto it = prefs.find(k);
            return it != prefs.end() ? std::strtof(it->second.c_str(), nullptr) : fallback.value_or(0.0f);
        };
        prefs_table["setString"] = [this](const std::string& k, const std::string& v) { prefs[k] = v; savePrefs(); };
        prefs_table["getString"] = [this](const std::string& k, sol::optional<std::string> fallback) {
            const auto it = prefs.find(k);
            return it != prefs.end() ? it->second : fallback.value_or(std::string());
        };
        prefs_table["hasKey"] = [this](const std::string& k) { return prefs.count(k) != 0; };
        prefs_table["deleteKey"] = [this](const std::string& k) { prefs.erase(k); savePrefs(); };
        prefs_table["deleteAll"] = [this]() { prefs.clear(); savePrefs(); };

        sol::table game = L.create_named_table("Game");
        game["quit"] = [this]() { quit_request = true; };

        // Input
        sol::table in = L.create_named_table("Input");
        in["getKey"] = [this](const std::string& k) { return keyDown(k); };
        in["getKeyDown"] = [this](const std::string& k) {
            const dm::Key key_code = key(k);
            return input != nullptr && key_code != dm::Key::Unknown && input->isKeyPressed(key_code);
        };
        in["getKeyUp"] = [this](const std::string& k) {
            const dm::Key key_code = key(k);
            return input != nullptr && key_code != dm::Key::Unknown && input->isKeyReleased(key_code);
        };
        const auto button = [](int b) { return static_cast<dm::MouseButton>(std::clamp(b, 0, 4)); };
        in["getMouseButton"] = [this, button](int b) { return input != nullptr && input->isMouseButtonDown(button(b)); };
        in["getMouseButtonDown"] = [this, button](int b) { return input != nullptr && input->isMouseButtonPressed(button(b)); };
        in["getMouseButtonUp"] = [this, button](int b) { return input != nullptr && input->isMouseButtonReleased(button(b)); };
        in["mousePosition"] = [this]() { return input != nullptr ? Vec3{input->mouseX(), input->mouseY(), 0.0f} : Vec3{}; };
        in["mouseDelta"] = [this]() { return input != nullptr ? Vec3{input->mouseDeltaX(), input->mouseDeltaY(), 0.0f} : Vec3{}; };
        in["getAxis"] = [this](const std::string& name) { return axis(name); };
        // Raton capturado (primera persona): oculto, sin salir de la ventana y
        // mouseDelta sin tope en los bordes.
        in["lockCursor"] = [this](sol::optional<bool> on) { lockCursor(on.value_or(true)); };
        in["isCursorLocked"] = [this]() { return cursor_locked; };

        // Time (se actualiza cada frame)
        sol::table t = L.create_named_table("Time");
        t["deltaTime"] = 0.0f;
        t["time"] = 0.0f;
        t["frameCount"] = 0;
        t["fixedDeltaTime"] = 1.0f / 60.0f;

        // Physics
        sol::table ph = L.create_named_table("Physics");
        ph["raycast"] = [this](const Vec3& origin, const Vec3& direction, sol::optional<float> distance) -> sol::object {
            if (physics == nullptr) return sol::lua_nil;
            physics::RaycastHit hit;
            if (!physics->raycast(origin, direction, distance.value_or(1000.0f), hit)) return sol::lua_nil;
            sol::table result = lua->create_table();
            result["entity"] = LuaEntity{hit.entity.handle(), world};
            result["point"] = hit.point;
            result["normal"] = hit.normal;
            result["distance"] = hit.distance;
            return result;
        };

        // Navigation: consultas de la malla.
        sol::table nav = L.create_named_table("Navigation");
        nav["isReady"] = [this]() { return navigation != nullptr && navigation->ready(); };
        nav["findPath"] = [this](const Vec3& from, const Vec3& to) -> sol::object {
            std::vector<Vec3> path;
            if (navigation == nullptr || !navigation->findPath(from, to, path)) return sol::lua_nil;
            sol::table out = lua->create_table();
            for (std::size_t i = 0; i < path.size(); ++i) out[i + 1] = path[i];
            return out;
        };
        nav["projectPoint"] = [this](const Vec3& point, sol::optional<float> extent) -> sol::object {
            Vec3 result{};
            if (navigation == nullptr || !navigation->projectPoint(point, result, extent.value_or(2.0f))) return sol::lua_nil;
            return sol::make_object(*lua, result);
        };
        nav["randomPoint"] = [this](const Vec3& center, float radius) -> sol::object {
            Vec3 result{};
            if (navigation == nullptr || !navigation->randomPoint(center, radius, result)) return sol::lua_nil;
            return sol::make_object(*lua, result);
        };
        // true si la linea recta por la malla llega; si no, false y el punto del choque.
        nav["raycast"] = [this](const Vec3& from, const Vec3& to) {
            Vec3 hit = to;
            const bool clear = navigation != nullptr && navigation->raycast(from, to, &hit);
            return std::make_tuple(clear, hit);
        };

        bindVoxel(L);
        bindMesh(L);
        bindGraphics(L);

        // Audio
        sol::table au = L.create_named_table("Audio");
        au["playOneShot"] = [this](const std::string& clip, sol::optional<Vec3> position, sol::optional<float> volume) {
            if (audio != nullptr) audio->playOneShot(clip, position.value_or(Vec3{}), volume.value_or(1.0f), position.has_value());
        };
        // Oclusion y paso bajo general de los AudioListener de la escena.
        au["setOcclusion"] = [this](bool on) {
            if (world == nullptr) return;
            for (const entt::entity h : world->registry().view<audio::AudioListener>()) {
                world->registry().get<audio::AudioListener>(h).occlusion = on;
            }
        };
        au["occlusion"] = [this]() {
            if (world == nullptr) return false;
            for (const entt::entity h : world->registry().view<audio::AudioListener>()) {
                return world->registry().get<audio::AudioListener>(h).occlusion;
            }
            return false;
        };
        au["setLowPass"] = [this](bool on, sol::optional<float> cutoff) {
            if (world == nullptr) return;
            for (const entt::entity h : world->registry().view<audio::AudioListener>()) {
                audio::AudioListener& l = world->registry().get<audio::AudioListener>(h);
                l.low_pass = on;
                if (cutoff) l.low_pass_cutoff = *cutoff;
            }
        };
        au["reverbLevel"] = [this]() { return audio != nullptr ? audio->reverbLevel() : 0.0f; };

        // Debug / print
        const auto joined = [](sol::variadic_args args, sol::this_state s) {
            sol::state_view view(s);
            sol::protected_function tostring = view["tostring"];
            std::string text;
            for (auto arg : args) {
                if (!text.empty()) text += " ";
                sol::protected_function_result r = tostring(arg.get<sol::object>());
                text += r.valid() ? r.get<std::string>() : std::string("?");
            }
            return text;
        };
        sol::table dbg = L.create_named_table("Debug");
        dbg["log"] = [this, joined](sol::variadic_args a, sol::this_state s) { write(0, joined(a, s)); };
        dbg["warn"] = [this, joined](sol::variadic_args a, sol::this_state s) { write(1, joined(a, s)); };
        dbg["error"] = [this, joined](sol::variadic_args a, sol::this_state s) { write(2, joined(a, s)); };
        L["print"] = [this, joined](sol::variadic_args a, sol::this_state s) { write(0, joined(a, s)); };
    }

    void lockCursor(bool on) {
        if (on == cursor_locked) return;
        cursor_locked = on;
        if (cursor_lock) cursor_lock(on);
    }

    // --- Graphics: la configuracion grafica (QualitySettings + Screen de Unity) ---
    //   Graphics.setQuality("Baja")          calidades rapidas
    //   Graphics.vsync = true                 cualquier opcion por su clave
    //   Graphics.set{ textures = 2048, shadows = true }
    //   Graphics.post.bloom = false           post-procesado global de la escena
    //   Graphics.save()                       el juego la recupera al abrirse
    // Las opciones las da el host (setGraphics); sin host las funciones
    // avisan y no hacen nada.

    // Volumen de post-procesado global que manda (el de mayor prioridad).
    // Si la escena no tiene, se crea uno al cambiar algo.
    ecs::Entity globalPostVolume(bool create) {
        if (world == nullptr) return {};
        ecs::Entity best;
        int best_priority = 0;
        for (const entt::entity h : world->registry().view<ecs::PostProcessing>()) {
            const ecs::PostProcessing& p = world->registry().get<ecs::PostProcessing>(h);
            if (!p.isGlobal()) continue;
            if (!best.valid() || p.priority > best_priority) {
                best = world->wrap(h);
                best_priority = p.priority;
            }
        }
        if (!best.valid() && create) {
            best = world->create("Post-procesado global");
            best.add<ecs::PostProcessing>();
        }
        return best;
    }

    bool reflectPost(ecs::Entity e, PostFieldVisitor& visitor) {
        const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find("PostProcessing");
        if (type == nullptr || !e.valid() || world == nullptr) return false;
        return type->reflect(*world, e.handle(), visitor);
    }

    // 2048 y no 2048.0 al imprimirlo (Lua 5.4 distingue enteros).
    static sol::object numberToLua(sol::state_view L, double n) {
        if (std::floor(n) == n && std::abs(n) < 9.0e15) return sol::make_object(L, static_cast<long long>(n));
        return sol::make_object(L, n);
    }

    static sol::object postToLua(sol::state_view L, const PostValue& v) {
        switch (v.type) {
            case PostValue::Type::Bool: return sol::make_object(L, v.flag);
            case PostValue::Type::Number: return numberToLua(L, v.number);
            case PostValue::Type::Text: return sol::make_object(L, v.text);
            case PostValue::Type::Vector: return sol::make_object(L, v.vector);
            case PostValue::Type::None: break;
        }
        return sol::lua_nil;
    }

    static bool postFromLua(const sol::object& o, PostValue& out) {
        switch (o.get_type()) {
            case sol::type::boolean: out.type = PostValue::Type::Bool; out.flag = o.as<bool>(); return true;
            case sol::type::number: out.type = PostValue::Type::Number; out.number = o.as<double>(); return true;
            case sol::type::string: out.type = PostValue::Type::Text; out.text = o.as<std::string>(); return true;
            default:
                if (o.is<Vec3>()) {
                    out.type = PostValue::Type::Vector;
                    out.vector = o.as<Vec3>();
                    return true;
                }
                return false;
        }
    }

    static sol::object graphicsToLua(sol::state_view L, const GraphicsValue& v) {
        if (const bool* b = std::get_if<bool>(&v)) return sol::make_object(L, *b);
        if (const double* d = std::get_if<double>(&v)) return numberToLua(L, *d);
        return sol::make_object(L, std::get<std::string>(v));
    }

    static bool graphicsFromLua(const sol::object& o, GraphicsValue& out) {
        switch (o.get_type()) {
            case sol::type::boolean: out = o.as<bool>(); return true;
            case sol::type::number: out = o.as<double>(); return true;
            case sol::type::string: out = o.as<std::string>(); return true;
            default: return false;
        }
    }

    // Graphics.set / Graphics.<clave> = valor. Devuelve si se aplico.
    bool setGraphicsOption(const std::string& key, const sol::object& value) {
        if (graphics == nullptr) {
            write(1, "Graphics: este programa no permite cambiar la configuracion grafica");
            return false;
        }
        GraphicsValue v;
        if (!graphicsFromLua(value, v)) {
            write(1, "Graphics." + key + ": el valor debe ser true/false, un numero o un texto");
            return false;
        }
        std::string error;
        if (!graphics->set(key, v, error)) {
            write(1, "Graphics." + key + ": " + error);
            return false;
        }
        return true;
    }

    sol::object getGraphicsOption(sol::state_view L, const std::string& key) {
        if (graphics == nullptr) return sol::lua_nil;
        for (const GraphicsOption& o : graphics->options()) {
            if (o.key == key) return graphicsToLua(L, o.value);
        }
        return sol::lua_nil;
    }

    bool setPostField(const std::string& key, const sol::object& value) {
        PostValue v;
        if (!postFromLua(value, v)) {
            write(1, "Graphics.post." + key + ": valor no valido");
            return false;
        }
        ecs::Entity volume = globalPostVolume(true);
        if (!volume.valid()) return false;
        PostFieldVisitor visitor(PostFieldVisitor::Mode::Set, key, v);
        reflectPost(volume, visitor);
        if (!visitor.found()) {
            write(1, "Graphics.post: no existe '" + key + "' (mira Graphics.postKeys())");
            return false;
        }
        if (!visitor.error().empty()) {
            write(1, "Graphics.post." + key + ": " + visitor.error());
            return false;
        }
        return true;
    }

    sol::object getPostField(sol::state_view L, const std::string& key) {
        ecs::Entity volume = globalPostVolume(false);
        if (!volume.valid()) {
            // Sin volumen: los valores por defecto del motor.
            ecs::World scratch;
            ecs::Entity temp = scratch.create("temp");
            temp.add<ecs::PostProcessing>();
            PostFieldVisitor visitor(PostFieldVisitor::Mode::Get, key);
            const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find("PostProcessing");
            if (type != nullptr) type->reflect(scratch, temp.handle(), visitor);
            return visitor.found() ? postToLua(L, visitor.value()) : sol::object(sol::lua_nil);
        }
        PostFieldVisitor visitor(PostFieldVisitor::Mode::Get, key);
        reflectPost(volume, visitor);
        return visitor.found() ? postToLua(L, visitor.value()) : sol::object(sol::lua_nil);
    }

    void bindGraphics(sol::state& L) {
        sol::table g = L.create_named_table("Graphics");
        sol::state* S = &L;

        g["get"] = [this, S](const std::string& key) { return getGraphicsOption(*S, key); };
        // Graphics.set("vsync", true) o Graphics.set{ vsync = true, textures = 2048 }.
        g["set"] = [this](sol::object first, sol::optional<sol::object> second) {
            if (first.get_type() == sol::type::table) {
                bool all = true;
                for (const auto& [k, v] : first.as<sol::table>()) {
                    if (k.get_type() == sol::type::string) all = setGraphicsOption(k.as<std::string>(), v) && all;
                }
                return all;
            }
            if (first.get_type() != sol::type::string || !second) {
                write(1, "Graphics.set: usa Graphics.set(\"clave\", valor) o Graphics.set{ clave = valor }");
                return false;
            }
            return setGraphicsOption(first.as<std::string>(), *second);
        };
        g["getAll"] = [this, S]() {
            sol::table t = S->create_table();
            if (graphics != nullptr) {
                for (const GraphicsOption& o : graphics->options()) t[o.key] = graphicsToLua(*S, o.value);
            }
            return t;
        };
        // Lista para montar un menu de opciones: clave, valor, si se puede
        // cambiar, descripcion y valores posibles.
        g["options"] = [this, S]() {
            sol::table list = S->create_table();
            if (graphics == nullptr) return list;
            int i = 1;
            for (const GraphicsOption& o : graphics->options()) {
                sol::table item = S->create_table();
                item["key"] = o.key;
                item["value"] = graphicsToLua(*S, o.value);
                item["writable"] = o.writable;
                item["description"] = o.description;
                if (!o.choices.empty()) {
                    sol::table choices = S->create_table();
                    for (std::size_t c = 0; c < o.choices.size(); ++c) choices[c + 1] = o.choices[c];
                    item["choices"] = choices;
                }
                list[i++] = item;
            }
            return list;
        };
        g["setQuality"] = [this](sol::object level) {
            if (graphics == nullptr) {
                write(1, "Graphics: este programa no permite cambiar la configuracion grafica");
                return false;
            }
            std::string name;
            if (level.get_type() == sol::type::number) {
                // 0..3 como el QualitySettings.SetQualityLevel de Unity.
                const std::vector<std::string> levels = graphics->qualityLevels();
                const int index = level.as<int>();
                if (index >= 0 && index < static_cast<int>(levels.size())) name = levels[static_cast<std::size_t>(index)];
            } else if (level.get_type() == sol::type::string) {
                name = level.as<std::string>();
            }
            std::string error;
            if (name.empty() || !graphics->setQuality(name, error)) {
                write(1, "Graphics.setQuality: " + (error.empty() ? std::string("calidad desconocida") : error));
                return false;
            }
            return true;
        };
        g["getQuality"] = [this]() { return graphics != nullptr ? graphics->quality() : std::string(); };
        g["qualityLevels"] = [this, S]() {
            sol::table t = S->create_table();
            if (graphics != nullptr) {
                int i = 1;
                for (const std::string& level : graphics->qualityLevels()) t[i++] = level;
            }
            return t;
        };
        g["resolutions"] = [this, S]() {
            sol::table t = S->create_table();
            if (graphics != nullptr) {
                int i = 1;
                for (const auto& [w, h] : graphics->resolutions()) {
                    sol::table r = S->create_table();
                    r["width"] = w;
                    r["height"] = h;
                    t[i++] = r;
                }
            }
            return t;
        };
        g["save"] = [this]() {
            if (graphics == nullptr) return false;
            std::string error;
            if (!graphics->save(error)) {
                if (!error.empty()) write(1, "Graphics.save: " + error);
                return false;
            }
            return true;
        };

        // Post-procesado global: Graphics.post.bloom = false,
        // Graphics.getPost("exposure_compensation"), Graphics.postKeys().
        g["getPost"] = [this, S](const std::string& key) { return getPostField(*S, key); };
        g["setPost"] = [this](const std::string& key, sol::object value) { return setPostField(key, value); };
        g["postKeys"] = [S]() {
            // Las claves son las mismas en todos los volumenes: las de uno nuevo.
            ecs::World scratch;
            ecs::Entity temp = scratch.create("temp");
            temp.add<ecs::PostProcessing>();
            PostFieldVisitor visitor(PostFieldVisitor::Mode::Collect);
            if (const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find("PostProcessing")) {
                type->reflect(scratch, temp.handle(), visitor);
            }
            sol::table t = S->create_table();
            int i = 1;
            for (const auto& field : visitor.fields()) t[i++] = field.first;
            return t;
        };
        sol::table post = S->create_table();
        sol::table post_meta = S->create_table();
        post_meta[sol::meta_function::index] = [this, S](sol::table, const std::string& key) {
            return getPostField(*S, key);
        };
        post_meta[sol::meta_function::new_index] = [this](sol::table, const std::string& key, sol::object value) {
            setPostField(key, value);
        };
        post[sol::metatable_key] = post_meta;
        g["post"] = post;

        // Graphics.vsync, Graphics.textures = 2048...: las claves que no son
        // funciones van a las opciones.
        sol::table meta = S->create_table();
        meta[sol::meta_function::index] = [this, S](sol::table, const std::string& key) {
            return getGraphicsOption(*S, key);
        };
        meta[sol::meta_function::new_index] = [this](sol::table, const std::string& key, sol::object value) {
            setGraphicsOption(key, value);
        };
        g[sol::metatable_key] = meta;
    }

    // Voxel: el mundo de bloques. Los bloques se nombran por su id (numero) o
    // por su nombre ("stone", "Piedra").
    void bindVoxel(sol::state& L) {
        sol::table vx = L.create_named_table("Voxel");
        sol::state* S = &L;  // las funciones viven en este estado
        const auto idOf = [](const sol::object& block) -> int {
            if (block.get_type() == sol::type::number) return block.as<int>();
            if (block.get_type() == sol::type::string) return voxel::blockId(block.as<std::string>());
            return voxel::block::Air;
        };
        // Coordenadas de bloque: cualquier numero (las de un Vec3 son flotantes), hacia abajo.
        const auto cell = [](double v) { return static_cast<int>(std::floor(v)); };
        vx["isActive"] = [this]() { return voxels != nullptr && voxels->active(); };
        vx["getBlock"] = [this, cell](double x, double y, double z) {
            return voxels != nullptr ? static_cast<int>(voxels->getBlock(cell(x), cell(y), cell(z))) : 0;
        };
        vx["getBlockAt"] = [this, cell](const Vec3& p) {
            return voxels != nullptr ? static_cast<int>(voxels->getBlock(cell(p.x), cell(p.y), cell(p.z))) : 0;
        };
        vx["setBlock"] = [this, idOf, cell](double x, double y, double z, sol::object block) {
            const int id = idOf(block);
            return voxels != nullptr && id >= 0 && id < voxel::block::Count && voxels->setBlock(cell(x), cell(y), cell(z), static_cast<voxel::BlockId>(id));
        };
        vx["blockId"] = [](const std::string& name) { return static_cast<int>(voxel::blockId(name)); };
        vx["blockCount"] = []() { return static_cast<int>(voxel::block::Count); };
        // Datos de un bloque: name, label, solid, placeable, hardness, drop, light.
        vx["blockInfo"] = [S, idOf](sol::object block) -> sol::object {
            const int id = idOf(block);
            if (id < 0 || id >= voxel::block::Count) return sol::lua_nil;
            const voxel::BlockDef& def = voxel::blockDef(static_cast<voxel::BlockId>(id));
            sol::table t = S->create_table();
            t["id"] = id;
            t["name"] = def.name;
            t["label"] = def.label;
            t["solid"] = def.solid;
            t["placeable"] = def.placeable;
            t["replaceable"] = def.replaceable;
            t["hardness"] = def.hardness;
            t["drop"] = def.drop != 0 ? static_cast<int>(def.drop) : id;
            t["light"] = static_cast<int>(def.light);
            return t;
        };
        vx["blockColor"] = [this, idOf](sol::object block) {
            const int id = idOf(block);
            return voxels != nullptr && id >= 0 && id < voxel::block::Count ? voxels->blockColor(static_cast<voxel::BlockId>(id))
                                                                             : Vec3{1.0f, 1.0f, 1.0f};
        };
        vx["surfaceHeight"] = [this, cell](double x, double z) { return voxels != nullptr ? voxels->surfaceHeight(cell(x), cell(z)) : 0; };
        vx["isReady"] = [this, cell](const Vec3& p) { return voxels != nullptr && voxels->isReady(cell(p.x), cell(p.z)); };
        vx["inWater"] = [this](const Vec3& p) { return voxels != nullptr && voxels->inWater(p); };
        vx["skyLight"] = [this, cell](double x, double y, double z) { return voxels != nullptr ? voxels->skyLight(cell(x), cell(y), cell(z)) : 15; };
        vx["blockLight"] = [this, cell](double x, double y, double z) { return voxels != nullptr ? voxels->blockLight(cell(x), cell(y), cell(z)) : 0; };
        // El primer bloque que toca: {block = Vec3, normal = Vec3, id, point, distance}.
        vx["raycast"] = [this, S](const Vec3& origin, const Vec3& direction, sol::optional<float> distance) -> sol::object {
            if (voxels == nullptr) return sol::lua_nil;
            const voxel::VoxelHit hit = voxels->raycast(origin, direction, distance.value_or(8.0f));
            if (!hit.hit) return sol::lua_nil;
            sol::table t = S->create_table();
            t["block"] = Vec3{static_cast<float>(hit.block.x), static_cast<float>(hit.block.y), static_cast<float>(hit.block.z)};
            t["normal"] = Vec3{static_cast<float>(hit.normal.x), static_cast<float>(hit.normal.y), static_cast<float>(hit.normal.z)};
            t["id"] = static_cast<int>(hit.id);
            t["point"] = hit.point;
            t["distance"] = hit.distance;
            return t;
        };
        // Mueve una caja (centro, semiejes) contra los bloques: posicion, enSuelo, techo, pared.
        vx["moveBox"] = [this](const Vec3& center, const Vec3& half, const Vec3& delta) {
            if (voxels == nullptr) return std::make_tuple(center + delta, false, false, false);
            const voxel::VoxelSystem::MoveResult r = voxels->moveBox(center, half, delta);
            return std::make_tuple(r.position, r.on_ground, r.hit_ceiling, r.hit_wall);
        };
        vx["boxCollides"] = [this](const Vec3& center, const Vec3& half) { return voxels != nullptr && voxels->boxCollides(center, half); };
        // Mundos guardados (partidas).
        vx["newWorld"] = [this](const std::string& name, sol::optional<int> seed) {
            return voxels != nullptr && voxels->newWorld(name, seed.value_or(static_cast<int>(std::rand())));
        };
        vx["loadWorld"] = [this](const std::string& name) { return voxels != nullptr && voxels->loadWorld(name); };
        vx["saveWorld"] = [this]() { return voxels != nullptr && voxels->saveWorld(); };
        vx["deleteWorld"] = [this](const std::string& name) { return voxels != nullptr && voxels->deleteWorld(name); };
        vx["listWorlds"] = [this, S]() {
            sol::table out = S->create_table();
            if (voxels == nullptr) return out;
            int i = 1;
            for (const voxel::WorldInfo& w : voxels->listWorlds()) {
                sol::table t = S->create_table();
                t["name"] = w.name;
                t["seed"] = w.seed;
                t["lastPlayed"] = static_cast<double>(w.last_played);
                t["mode"] = w.mode;
                out[i++] = t;
            }
            return out;
        };
        vx["worldName"] = [this]() { return voxels != nullptr ? voxels->worldName() : std::string(); };
        vx["seed"] = [this]() { return voxels != nullptr ? voxels->seed() : 0; };
        vx["setMeta"] = [this](const std::string& k, const std::string& v) { if (voxels != nullptr) voxels->setMeta(k, v); };
        vx["getMeta"] = [this](const std::string& k, sol::optional<std::string> fallback) {
            return voxels != nullptr ? voxels->meta(k, fallback.value_or(std::string())) : fallback.value_or(std::string());
        };
    }

    // Mesh: mallas creadas por codigo, como el Mesh de Unity. Los triangulos
    // usan indices de vertice desde 0 (como Unity): el vertice 0 es
    // mesh.vertices[1] en Lua. Las UV van en Vec3 (x, y). Cambiar una lista
    // entera (mesh.vertices = {...}) o llamar a mesh:apply() la sube a la GPU
    // en el siguiente frame.
    void bindMesh(sol::state& L) {
        using MeshPtr = std::shared_ptr<ecs::Mesh>;
        sol::state* S = &L;
        const auto vec3List = [S](const std::vector<Vec3>& in) {
            sol::table t = S->create_table(static_cast<int>(in.size()), 0);
            for (std::size_t i = 0; i < in.size(); ++i) t[i + 1] = in[i];
            return t;
        };
        const auto readVec3 = [](const sol::table& t) {
            std::vector<Vec3> out;
            out.reserve(t.size());
            for (std::size_t i = 1; i <= t.size(); ++i) out.push_back(t.get<Vec3>(i));
            return out;
        };
        const auto readIndices = [](const sol::table& t) {
            std::vector<std::uint32_t> out;
            out.reserve(t.size());
            for (std::size_t i = 1; i <= t.size(); ++i) {
                const double v = t.get<double>(i);
                out.push_back(v < 0.0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v));
            }
            return out;
        };
        auto mesh = L.new_usertype<ecs::Mesh>(
            "Mesh", sol::no_constructor,
            "name", &ecs::Mesh::name,
            "vertexCount", sol::property([](const ecs::Mesh& m) { return static_cast<int>(m.vertices.size()); }),
            "triangleCount", sol::property([](const ecs::Mesh& m) { return static_cast<int>(m.triangleCount()); }),
            "subMeshCount", sol::property(&ecs::Mesh::subMeshCount, &ecs::Mesh::setSubMeshCount),
            "vertices", sol::property([vec3List](const ecs::Mesh& m) { return vec3List(m.vertices); },
                                      [readVec3](ecs::Mesh& m, const sol::table& t) {
                                          m.vertices = readVec3(t);
                                          m.markModified();
                                      }),
            "normals", sol::property([vec3List](const ecs::Mesh& m) { return vec3List(m.normals); },
                                     [readVec3](ecs::Mesh& m, const sol::table& t) {
                                         m.normals = readVec3(t);
                                         m.markModified();
                                     }),
            "uv", sol::property(
                      [S](const ecs::Mesh& m) {
                          sol::table t = S->create_table(static_cast<int>(m.uv.size()), 0);
                          for (std::size_t i = 0; i < m.uv.size(); ++i) t[i + 1] = Vec3{m.uv[i].x, m.uv[i].y, 0.0f};
                          return t;
                      },
                      [readVec3](ecs::Mesh& m, const sol::table& t) {
                          m.uv.clear();
                          for (const Vec3& v : readVec3(t)) m.uv.push_back(core::Vec2{v.x, v.y});
                          m.markModified();
                      }),
            // Tangentes: Vec3 (+U); el signo de la bitangente, +1.
            "tangents", sol::property(
                            [S](const ecs::Mesh& m) {
                                sol::table t = S->create_table(static_cast<int>(m.tangents.size()), 0);
                                for (std::size_t i = 0; i < m.tangents.size(); ++i) {
                                    t[i + 1] = Vec3{m.tangents[i].x, m.tangents[i].y, m.tangents[i].z};
                                }
                                return t;
                            },
                            [readVec3](ecs::Mesh& m, const sol::table& t) {
                                m.tangents.clear();
                                for (const Vec3& v : readVec3(t)) m.tangents.push_back(core::Vec4{v.x, v.y, v.z, 1.0f});
                                m.markModified();
                            }),
            // Los de la submalla 0 (como mesh.triangles de Unity).
            "triangles", sol::property(
                             [S](const ecs::Mesh& m) {
                                 const std::vector<std::uint32_t>& tris = m.triangles(0);
                                 sol::table t = S->create_table(static_cast<int>(tris.size()), 0);
                                 for (std::size_t i = 0; i < tris.size(); ++i) t[i + 1] = tris[i];
                                 return t;
                             },
                             [readIndices](ecs::Mesh& m, const sol::table& t) {
                                 m.setTriangles(readIndices(t), 0);
                                 m.markModified();
                             }),
            "setTriangles", [readIndices](ecs::Mesh& m, const sol::table& t, sol::optional<int> submesh) {
                m.setTriangles(readIndices(t), submesh.value_or(0));
                m.markModified();
            },
            "getTriangles", [S](const ecs::Mesh& m, sol::optional<int> submesh) {
                const std::vector<std::uint32_t>& tris = m.triangles(submesh.value_or(0));
                sol::table t = S->create_table(static_cast<int>(tris.size()), 0);
                for (std::size_t i = 0; i < tris.size(); ++i) t[i + 1] = tris[i];
                return t;
            },
            // Un vertice suelto (indice desde 0): despues, mesh:apply().
            "setVertex", [](ecs::Mesh& m, int index, const Vec3& p) {
                if (index >= 0 && static_cast<std::size_t>(index) < m.vertices.size()) m.vertices[static_cast<std::size_t>(index)] = p;
            },
            "getVertex", [](const ecs::Mesh& m, int index) {
                return index >= 0 && static_cast<std::size_t>(index) < m.vertices.size() ? m.vertices[static_cast<std::size_t>(index)] : Vec3{};
            },
            // Material de una submalla (sin .crmat): {color, alpha, metallic,
            // roughness, emission, emissionIntensity}.
            "setMaterial", [](ecs::Mesh& m, int submesh, const sol::table& t) {
                if (submesh < 0) return;
                bool layout = false;  // texturas o huecos nuevos: hay que volver a subirla
                if (m.materials.size() <= static_cast<std::size_t>(submesh)) {
                    m.materials.resize(static_cast<std::size_t>(submesh) + 1);
                    layout = true;
                }
                ecs::MeshMaterial& mat = m.materials[static_cast<std::size_t>(submesh)];
                if (sol::optional<Vec3> c = t["color"]) mat.color = core::Vec4{c->x, c->y, c->z, mat.color.w};
                if (sol::optional<float> a = t["alpha"]) mat.color.w = *a;
                if (sol::optional<float> v = t["metallic"]) mat.metallic = *v;
                if (sol::optional<float> v = t["roughness"]) mat.roughness = *v;
                if (sol::optional<Vec3> e = t["emission"]) mat.emission = *e;
                if (sol::optional<float> v = t["emissionIntensity"]) mat.emission_intensity = *v;
                if (sol::optional<float> v = t["normalStrength"]) mat.normal_strength = *v;
                const auto text = [&](const char* key, std::string& field) {
                    sol::object o = t[key];
                    if (o.get_type() == sol::type::string && o.as<std::string>() != field) {
                        field = o.as<std::string>();
                        layout = true;
                    } else if (o.get_type() == sol::type::boolean && !o.as<bool>() && !field.empty()) {
                        field.clear();  // false = quitarla
                        layout = true;
                    }
                };
                text("texture", mat.texture);
                text("normalMap", mat.normal_map);
                text("emissionMap", mat.emission_map);
                if (sol::optional<Vec3> v = t["tiling"]) {
                    mat.tiling = core::Vec2{v->x, v->y};
                    layout = true;
                }
                if (sol::optional<Vec3> v = t["offset"]) {
                    mat.offset = core::Vec2{v->x, v->y};
                    layout = true;
                }
                if (layout) m.markModified();
                else m.markMaterialsModified();  // efectos por frame: sin volver a subir la malla
            },
            "getMaterial", [S](const ecs::Mesh& m, int submesh) -> sol::object {
                if (submesh < 0) return sol::lua_nil;
                const ecs::MeshMaterial mat = static_cast<std::size_t>(submesh) < m.materials.size() ? m.materials[static_cast<std::size_t>(submesh)]
                                                                                                   : ecs::MeshMaterial{};
                sol::table t = S->create_table();
                t["color"] = Vec3{mat.color.x, mat.color.y, mat.color.z};
                t["alpha"] = mat.color.w;
                t["metallic"] = mat.metallic;
                t["roughness"] = mat.roughness;
                t["emission"] = mat.emission;
                t["emissionIntensity"] = mat.emission_intensity;
                t["normalStrength"] = mat.normal_strength;
                t["texture"] = mat.texture;
                t["normalMap"] = mat.normal_map;
                t["emissionMap"] = mat.emission_map;
                t["tiling"] = Vec3{mat.tiling.x, mat.tiling.y, 0.0f};
                t["offset"] = Vec3{mat.offset.x, mat.offset.y, 0.0f};
                return t;
            },
            "recalculateNormals", [](ecs::Mesh& m) { m.recalculateNormals(); m.markModified(); },
            "recalculateTangents", [](ecs::Mesh& m) { m.recalculateTangents(); m.markModified(); },
            "recalculateBounds", [](ecs::Mesh& m) { m.recalculateBounds(); },
            "boundsMin", sol::property([](const ecs::Mesh& m) { return m.boundsMin(); }),
            "boundsMax", sol::property([](const ecs::Mesh& m) { return m.boundsMax(); }),
            "apply", [](ecs::Mesh& m) { m.markModified(); },
            "clear", [](ecs::Mesh& m) { m.clear(); },
            // "" si se puede dibujar; si no, que le pasa.
            "validate", [](const ecs::Mesh& m) { return m.validate(); },
            "clone", [](const ecs::Mesh& m) {
                auto copy = std::make_shared<ecs::Mesh>(m);
                copy->markModified();
                return copy;
            },
            sol::meta_function::to_string, [](const ecs::Mesh& m) {
                return "Mesh(" + m.name + ", " + std::to_string(m.vertices.size()) + " vertices, " +
                       std::to_string(m.triangleCount()) + " triangulos)";
            });
        mesh["new"] = [](sol::optional<std::string> name) {
            auto m = std::make_shared<ecs::Mesh>();
            if (name) m->name = *name;
            return m;
        };
        mesh["cube"] = [](sol::object size) {
            Vec3 s{1.0f, 1.0f, 1.0f};
            if (size.is<Vec3>()) s = size.as<Vec3>();
            else if (size.get_type() == sol::type::number) s = Vec3{1.0f, 1.0f, 1.0f} * size.as<float>();
            return ecs::Mesh::cube(s);
        };
        mesh["quad"] = [](sol::optional<float> w, sol::optional<float> h) { return ecs::Mesh::quad(w.value_or(1.0f), h.value_or(w.value_or(1.0f))); };
        mesh["plane"] = [](sol::optional<float> w, sol::optional<float> d, sol::optional<int> sx, sol::optional<int> sz) {
            return ecs::Mesh::plane(w.value_or(10.0f), d.value_or(w.value_or(10.0f)), sx.value_or(10), sz.value_or(sx.value_or(10)));
        };
        mesh["sphere"] = [](sol::optional<float> r, sol::optional<int> seg, sol::optional<int> rings) {
            return ecs::Mesh::sphere(r.value_or(0.5f), seg.value_or(32), rings.value_or(16));
        };
        mesh["cylinder"] = [](sol::optional<float> r, sol::optional<float> h, sol::optional<int> seg) {
            return ecs::Mesh::cylinder(r.value_or(0.5f), h.value_or(2.0f), seg.value_or(32));
        };
        mesh["wireCube"] = [](sol::object size, sol::optional<float> thickness) {
            Vec3 s{1.0f, 1.0f, 1.0f};
            if (size.is<Vec3>()) s = size.as<Vec3>();
            else if (size.get_type() == sol::type::number) s = Vec3{1.0f, 1.0f, 1.0f} * size.as<float>();
            return ecs::Mesh::wireCube(s, thickness.value_or(0.02f));
        };
        mesh["capsule"] = [](sol::optional<float> r, sol::optional<float> h, sol::optional<int> seg) {
            return ecs::Mesh::capsule(r.value_or(0.5f), h.value_or(2.0f), seg.value_or(24));
        };
        (void)S;
        (void)sizeof(MeshPtr);
    }

    // --- Clases e instancias ---
    sol::object loadClass(sol::state& L, const std::string& file, std::string* error) {
        std::string code;
        if (!readText(root / fromUtf8(file), code)) {
            if (error) *error = "No se encuentra " + file;
            return sol::lua_nil;
        }
        sol::load_result chunk = L.load(code, "@" + file);
        if (!chunk.valid()) {
            sol::error e = chunk;
            if (error) *error = e.what();
            return sol::lua_nil;
        }
        sol::protected_function f = chunk;
        sol::protected_function_result result = f();
        if (!result.valid()) {
            sol::error e = result;
            if (error) *error = e.what();
            return sol::lua_nil;
        }
        sol::object value = result;
        if (!value.is<sol::table>()) {
            if (error) *error = file + ": el script debe terminar con 'return <su tabla>'";
            return sol::lua_nil;
        }
        return value;
    }

    sol::table* classFor(const std::string& file) {
        if (const auto it = classes.find(file); it != classes.end()) return &it->second;
        if (failed_classes.contains(file)) return nullptr;
        std::string error;
        sol::object cls = loadClass(*lua, file, &error);
        if (!cls.is<sol::table>()) {
            failed_classes[file] = error;
            fail(file, error);
            return nullptr;
        }
        return &(classes[file] = cls.as<sol::table>());
    }

    sol::object propertyValue(const ScriptProperty& p) {
        switch (p.type) {
            case PropertyType::Number: return sol::make_object(*lua, std::strtod(p.value.c_str(), nullptr));
            case PropertyType::Bool: return sol::make_object(*lua, p.value == "true" || p.value == "1");
            case PropertyType::Text: return sol::make_object(*lua, p.value);
            case PropertyType::Vector: {
                Vec3 v{};
                std::istringstream ss(p.value);
                ss >> v.x >> v.y >> v.z;
                return sol::make_object(*lua, v);
            }
        }
        return sol::lua_nil;
    }

    void createInstance(ecs::Entity e, const Script& script) {
        if (script.file.empty()) return;
        sol::table* cls = classFor(script.file);
        if (cls == nullptr) return;
        Instance inst;
        inst.file = script.file;
        inst.self = lua->create_table();
        sol::table meta = lua->create_table();
        meta["__index"] = *cls;
        inst.self[sol::metatable_key] = meta;
        inst.self["entity"] = LuaEntity{e.handle(), world};
        // Propiedades: las de la clase y encima las del Inspector.
        sol::optional<sol::table> defaults = (*cls)["properties"];
        if (defaults) {
            for (const auto& [k, v] : *defaults) {
                if (v.is<Vec3>()) {
                    inst.self[k] = v.as<Vec3>();
                } else {
                    inst.self[k] = v;
                }
            }
        }
        for (const ScriptProperty& p : script.properties) {
            if (!p.name.empty()) inst.self[p.name] = propertyValue(p);
        }
        auto& stored = instances[e.handle()] = std::move(inst);
        call(stored, "Awake");
    }

    template <typename... Args>
    void call(Instance& inst, const char* method, Args&&... args) {
        if (inst.failed) return;
        sol::object fn = inst.self[method];
        if (fn.get_type() != sol::type::function) return;
        sol::protected_function pf = fn;
        sol::protected_function_result r = pf(inst.self, std::forward<Args>(args)...);
        if (!r.valid()) {
            sol::error e = r;
            inst.failed = true;  // hasta que se recargue el script
            fail(inst.file, e.what());
        }
    }

    void flushDestroys() {
        for (const entt::entity handle : pending_destroy) {
            if (world == nullptr || !world->registry().valid(handle)) continue;
            // Los scripts del objeto y de sus hijos: OnDestroy.
            std::vector<entt::entity> stack{handle};
            while (!stack.empty()) {
                const entt::entity h = stack.back();
                stack.pop_back();
                if (const auto it = instances.find(h); it != instances.end()) {
                    call(it->second, "OnDestroy");
                    instances.erase(it);
                }
                for (const entt::entity c : world->wrap(h).children()) stack.push_back(c);
            }
            world->destroy(world->wrap(handle));
        }
        pending_destroy.clear();
    }

    void dispatchEvents() {
        std::vector<physics::PhysicsEvent> queue;
        queue.swap(events);
        for (const physics::PhysicsEvent& event : queue) {
            const char* method = nullptr;
            switch (event.type) {
                case physics::PhysicsEventType::CollisionEnter: method = "OnCollisionEnter"; break;
                case physics::PhysicsEventType::CollisionStay: method = "OnCollisionStay"; break;
                case physics::PhysicsEventType::CollisionExit: method = "OnCollisionExit"; break;
                case physics::PhysicsEventType::TriggerEnter: method = "OnTriggerEnter"; break;
                case physics::PhysicsEventType::TriggerStay: method = "OnTriggerStay"; break;
                case physics::PhysicsEventType::TriggerExit: method = "OnTriggerExit"; break;
                default: break;
            }
            if (method == nullptr) continue;
            const physics::PhysicsEvent sides[2] = {event, event.flipped()};
            for (const physics::PhysicsEvent& side : sides) {
                const auto it = instances.find(side.a.handle());
                if (it == instances.end() || !side.b.valid()) continue;
                sol::table contact = lua->create_table();
                contact["point"] = side.point;
                contact["normal"] = side.normal;
                contact["relativeVelocity"] = side.relative_velocity;
                call(it->second, method, LuaEntity{side.b.handle(), world}, contact);
            }
        }
    }
};

ScriptSystem::ScriptSystem() : impl_(std::make_unique<Impl>()) { impl_->buildKeys(); }

ScriptSystem::~ScriptSystem() { stop(); }

void ScriptSystem::setAssetsRoot(const std::filesystem::path& root) { impl_->root = root; }
void ScriptSystem::setInput(const dm::Input* input) { impl_->input = input; }
void ScriptSystem::setPhysics(physics::PhysicsSystem* physics) { impl_->physics = physics; }
void ScriptSystem::setAudio(audio::AudioSystem* audio) { impl_->audio = audio; }
void ScriptSystem::setNavigation(navigation::NavigationSystem* navigation) { impl_->navigation = navigation; }
void ScriptSystem::setVoxels(voxel::VoxelSystem* voxels) { impl_->voxels = voxels; }
void ScriptSystem::setGraphics(GraphicsHost* graphics) { impl_->graphics = graphics; }
void ScriptSystem::setCursorLock(CursorLockCallback callback) { impl_->cursor_lock = std::move(callback); }
bool ScriptSystem::cursorLocked() const { return impl_->cursor_locked; }
void ScriptSystem::releaseCursor() { impl_->lockCursor(false); }
void ScriptSystem::setLog(LogCallback log) { impl_->log = std::move(log); }

std::filesystem::path ScriptSystem::takeSceneRequest() {
    std::filesystem::path path = std::move(impl_->scene_request);
    impl_->scene_request.clear();
    return path;
}

bool ScriptSystem::takeQuitRequest() {
    const bool quit = impl_->quit_request;
    impl_->quit_request = false;
    return quit;
}

void ScriptSystem::setSceneName(const std::string& name) { impl_->scene_name = name; }

void ScriptSystem::setPrefsFile(const std::filesystem::path& file) {
    Impl& d = *impl_;
    d.prefs_file = file;
    d.prefs.clear();
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) d.prefs[line.substr(0, eq)] = line.substr(eq + 1);
    }
}
bool ScriptSystem::running() const { return impl_->running; }
const std::vector<ScriptError>& ScriptSystem::errors() const { return impl_->errors; }
void ScriptSystem::clearErrors() { impl_->errors.clear(); }

void ScriptSystem::start(ecs::World& world) {
    stop();
    Impl& d = *impl_;
    d.world = &world;
    d.lua = std::make_unique<sol::state>();
    d.bind(*d.lua);
    d.running = true;
    d.time = 0.0f;
    d.frame = 0;
    if (d.physics != nullptr) {
        d.listener = d.physics->addListener([&d](const physics::PhysicsEvent& event) { d.events.push_back(event); });
    }
    for (const entt::entity handle : world.registry().view<Script>()) {
        const ecs::Entity e = world.wrap(handle);
        const Script& script = e.get<Script>();
        if (e.activeInHierarchy()) d.createInstance(e, script);
    }
}

void ScriptSystem::update(ecs::World& world, float delta_seconds) {
    Impl& d = *impl_;
    if (!d.running) return;
    d.world = &world;
    d.time += delta_seconds;
    ++d.frame;
    sol::table time = (*d.lua)["Time"];
    time["deltaTime"] = delta_seconds;
    time["time"] = d.time;
    time["frameCount"] = d.frame;

    // Objetos con Script nuevos (creados en Play) y los que ya no estan.
    for (const entt::entity handle : world.registry().view<Script>()) {
        if (d.instances.contains(handle)) continue;
        const ecs::Entity e = world.wrap(handle);
        if (e.activeInHierarchy()) d.createInstance(e, e.get<Script>());
    }
    for (auto it = d.instances.begin(); it != d.instances.end();) {
        if (!world.registry().valid(it->first) || !world.registry().all_of<Script>(it->first)) {
            it = d.instances.erase(it);
        } else {
            ++it;
        }
    }

    d.dispatchEvents();
    // Copias de las claves: un script puede crear objetos durante el bucle.
    std::vector<entt::entity> order;
    order.reserve(d.instances.size());
    for (const auto& [handle, inst] : d.instances) order.push_back(handle);
    for (const entt::entity handle : order) {
        const auto it = d.instances.find(handle);
        if (it == d.instances.end()) continue;
        const ecs::Entity e = world.wrap(handle);
        const Script& script = e.get<Script>();
        if (!script.enabled || !e.activeInHierarchy()) continue;
        if (!it->second.started) {
            it->second.started = true;
            d.call(it->second, "Start");
        }
        d.call(it->second, "Update", delta_seconds);
    }
    for (const entt::entity handle : order) {
        const auto it = d.instances.find(handle);
        if (it == d.instances.end()) continue;
        const ecs::Entity e = world.wrap(handle);
        if (!e.get<Script>().enabled || !e.activeInHierarchy()) continue;
        d.call(it->second, "LateUpdate", delta_seconds);
    }
    d.flushDestroys();
}

void ScriptSystem::fixedUpdate(ecs::World& world, float step, int steps) {
    Impl& d = *impl_;
    if (!d.running || steps <= 0) return;
    d.world = &world;
    (*d.lua)["Time"]["fixedDeltaTime"] = step;
    for (int s = 0; s < steps; ++s) {
        for (auto& [handle, inst] : d.instances) {
            if (!world.registry().valid(handle)) continue;
            const ecs::Entity e = world.wrap(handle);
            if (!e.get<Script>().enabled || !inst.started) continue;
            d.call(inst, "FixedUpdate", step);
        }
    }
}

void ScriptSystem::stop() {
    Impl& d = *impl_;
    if (d.running) {
        for (auto& [handle, inst] : d.instances) d.call(inst, "OnDestroy");
    }
    if (d.physics != nullptr && d.listener >= 0) d.physics->removeListener(d.listener);
    d.listener = -1;
    d.instances.clear();
    d.classes.clear();
    d.failed_classes.clear();
    d.events.clear();
    d.pending_destroy.clear();
    d.lua.reset();
    d.running = false;
    d.lockCursor(false);  // el raton vuelve al sistema
}

void ScriptSystem::reloadFile(const std::string& file) {
    Impl& d = *impl_;
    d.described.erase(file);
    if (!d.running) return;
    std::string error;
    sol::object cls = d.loadClass(*d.lua, file, &error);
    if (!cls.is<sol::table>()) {
        d.fail(file, error);
        return;
    }
    d.failed_classes.erase(file);
    sol::table table = cls.as<sol::table>();
    d.classes[file] = table;
    // Las instancias siguen con sus datos y usan las funciones nuevas.
    for (auto& [handle, inst] : d.instances) {
        if (inst.file != file) continue;
        sol::table meta = d.lua->create_table();
        meta["__index"] = table;
        inst.self[sol::metatable_key] = meta;
        inst.failed = false;
    }
    d.write(0, "Script recargado: " + file);
    // Objetos cuyo script no se pudo cargar antes: ahora si.
    if (d.world != nullptr) {
        for (const entt::entity handle : d.world->registry().view<Script>()) {
            const ecs::Entity e = d.world->wrap(handle);
            if (!d.instances.contains(handle) && e.get<Script>().file == file) d.createInstance(e, e.get<Script>());
        }
    }
}

std::vector<ScriptProperty> ScriptSystem::describe(const std::string& file, std::string* error) {
    Impl& d = *impl_;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(d.root / fromUtf8(file), ec);
    if (const auto it = d.described.find(file); it != d.described.end() && !ec && it->second.stamp == stamp) {
        if (error) *error = it->second.error;
        return it->second.properties;
    }
    Impl::DescribeCache cache;
    cache.stamp = stamp;
    sol::state temp;
    ecs::World* saved_world = d.world;
    d.world = nullptr;  // el script no toca la escena al describirse
    d.bind(temp);
    std::string message;
    sol::object cls = d.loadClass(temp, file, &message);
    d.world = saved_world;
    if (cls.is<sol::table>()) {
        sol::optional<sol::table> props = cls.as<sol::table>()["properties"];
        if (props) {
            for (const auto& [k, v] : *props) {
                if (!k.is<std::string>()) continue;
                ScriptProperty p;
                p.name = k.as<std::string>();
                if (v.get_type() == sol::type::number) {
                    p.type = PropertyType::Number;
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "%g", v.as<double>());
                    p.value = buffer;
                } else if (v.get_type() == sol::type::boolean) {
                    p.type = PropertyType::Bool;
                    p.value = v.as<bool>() ? "true" : "false";
                } else if (v.get_type() == sol::type::string) {
                    p.type = PropertyType::Text;
                    p.value = v.as<std::string>();
                } else if (v.is<Vec3>()) {
                    p.type = PropertyType::Vector;
                    const Vec3 vec = v.as<Vec3>();
                    char buffer[96];
                    std::snprintf(buffer, sizeof(buffer), "%g %g %g", vec.x, vec.y, vec.z);
                    p.value = buffer;
                } else {
                    continue;
                }
                cache.properties.push_back(p);
            }
            std::sort(cache.properties.begin(), cache.properties.end(),
                      [](const ScriptProperty& a, const ScriptProperty& b) { return a.name < b.name; });
        }
    } else {
        cache.error = message;
    }
    if (error) *error = cache.error;
    auto result = cache.properties;
    d.described[file] = std::move(cache);
    return result;
}

void ScriptSystem::shiftOrigin(const core::Vec3& offset) {
    Impl& d = *impl_;
    for (auto& [handle, inst] : d.instances) d.call(inst, "OnOriginShift", offset);
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, ecs::Entity source) {
    Impl& d = *impl_;
    const auto it = target.valid() ? d.instances.find(target.handle()) : d.instances.end();
    if (it != d.instances.end()) d.call(it->second, method.c_str(), LuaEntity{source.handle(), d.world});
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, float value) {
    Impl& d = *impl_;
    const auto it = target.valid() ? d.instances.find(target.handle()) : d.instances.end();
    if (it != d.instances.end()) d.call(it->second, method.c_str(), value);
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, const std::string& value) {
    Impl& d = *impl_;
    const auto it = target.valid() ? d.instances.find(target.handle()) : d.instances.end();
    if (it != d.instances.end()) d.call(it->second, method.c_str(), value);
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, bool value) {
    Impl& d = *impl_;
    const auto it = target.valid() ? d.instances.find(target.handle()) : d.instances.end();
    if (it != d.instances.end()) d.call(it->second, method.c_str(), value);
}

bool ScriptSystem::run(const std::string& code, std::string* output) {
    Impl& d = *impl_;
    sol::state* L = d.lua.get();
    std::unique_ptr<sol::state> temp;
    if (L == nullptr) {
        temp = std::make_unique<sol::state>();
        d.bind(*temp);
        L = temp.get();
    }
    sol::protected_function_result r = L->safe_script(code, sol::script_pass_on_error, "@consola");
    if (!r.valid()) {
        sol::error e = r;
        if (output) *output = e.what();
        return false;
    }
    if (output) {
        sol::object value = r;
        sol::protected_function tostring = (*L)["tostring"];
        sol::protected_function_result s = tostring(value);
        *output = s.valid() ? s.get<std::string>() : std::string{};
    }
    return true;
}

}  // namespace cramion::scripting
