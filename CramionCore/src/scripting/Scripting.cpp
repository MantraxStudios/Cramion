#include "CramionCore/scripting/Scripting.h"

#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/ui/UI.h"

#include <CramionDM/Input.h>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

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

        // Vec3
        auto vec = L.new_usertype<Vec3>(
            "Vec3", sol::call_constructor,
            sol::factories([]() { return Vec3{}; }, [](float x, float y, float z) { return Vec3{x, y, z}; }),
            "x", &Vec3::x, "y", &Vec3::y, "z", &Vec3::z,
            sol::meta_function::addition, [](const Vec3& a, const Vec3& b) { return a + b; },
            sol::meta_function::subtraction, [](const Vec3& a, const Vec3& b) { return a - b; },
            sol::meta_function::multiplication,
            sol::overload([](const Vec3& a, float s) { return a * s; }, [](float s, const Vec3& a) { return a * s; },
                          [](const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }),
            sol::meta_function::division, [](const Vec3& a, float s) { return a * (1.0f / s); },
            sol::meta_function::unary_minus, [](const Vec3& a) { return a * -1.0f; },
            sol::meta_function::equal_to, [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; },
            sol::meta_function::to_string, [](const Vec3& v) { return vecString(v); },
            "length", [](const Vec3& v) { return core::length(v); },
            "normalized", [](const Vec3& v) { return core::length(v) > 1e-6f ? core::normalize(v) : Vec3{}; },
            "dot", [](const Vec3& a, const Vec3& b) { return core::dot(a, b); },
            "cross", [](const Vec3& a, const Vec3& b) { return core::cross(a, b); },
            "distance", [](const Vec3& a, const Vec3& b) { return core::length(a - b); },
            "lerp", [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; });
        vec["zero"] = sol::var(Vec3{0.0f, 0.0f, 0.0f});
        vec["one"] = sol::var(Vec3{1.0f, 1.0f, 1.0f});
        vec["up"] = sol::var(Vec3{0.0f, 1.0f, 0.0f});
        vec["down"] = sol::var(Vec3{0.0f, -1.0f, 0.0f});
        vec["right"] = sol::var(Vec3{1.0f, 0.0f, 0.0f});
        vec["left"] = sol::var(Vec3{-1.0f, 0.0f, 0.0f});
        vec["forward"] = sol::var(Vec3{0.0f, 0.0f, -1.0f});
        vec["back"] = sol::var(Vec3{0.0f, 0.0f, 1.0f});

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
            "translate", [](LuaEntity& e, const Vec3& d) { if (auto x = e.get(); x.valid()) x.setWorldPosition(x.worldPosition() + d); },
            "translateLocal", [](LuaEntity& e, const Vec3& d) {
                if (auto x = e.get(); x.valid()) x.setWorldPosition(x.worldPosition() + x.right() * d.x + x.up() * d.y - x.forward() * d.z);
            },
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
            "playSound", [this](LuaEntity& e) { if (auto x = e.get(); x.valid() && audio != nullptr) audio->play(x); },
            "stopSound", [this](LuaEntity& e) { if (auto x = e.get(); x.valid() && audio != nullptr) audio->stop(x); },
            "isPlayingSound", [this](const LuaEntity& e) { const ecs::Entity x = e.get(); return x.valid() && audio != nullptr && audio->isPlaying(x); },
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
        (void)entity;

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
        scene["instantiate"] = [this](const LuaEntity& original, sol::optional<Vec3> position) -> sol::object {
            const ecs::Entity source = original.get();
            if (world == nullptr || !source.valid()) return sol::lua_nil;
            ecs::Entity copy = world->duplicate(source);
            if (position) copy.setWorldPosition(*position);
            return sol::make_object(*lua, LuaEntity{copy.handle(), world});
        };
        scene["destroy"] = [this](const LuaEntity& e) { if (e.valid()) pending_destroy.push_back(e.handle); };

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

        // Audio
        sol::table au = L.create_named_table("Audio");
        au["playOneShot"] = [this](const std::string& clip, sol::optional<Vec3> position, sol::optional<float> volume) {
            if (audio != nullptr) audio->playOneShot(clip, position.value_or(Vec3{}), volume.value_or(1.0f), position.has_value());
        };

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

        // Mathf
        sol::table m = L.create_named_table("Mathf");
        m["pi"] = 3.14159265358979;
        m["deg2rad"] = 3.14159265358979 / 180.0;
        m["rad2deg"] = 180.0 / 3.14159265358979;
        m["lerp"] = [](float a, float b, float t) { return a + (b - a) * t; };
        m["clamp"] = [](float v, float lo, float hi) { return std::clamp(v, lo, hi); };
        m["clamp01"] = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
        m["smoothstep"] = [](float a, float b, float v) {
            const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        m["moveTowards"] = [](float current, float target, float step) {
            return std::abs(target - current) <= step ? target : current + (target > current ? step : -step);
        };
        m["random"] = [](float lo, float hi) {
            return lo + (hi - lo) * (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
        };
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
void ScriptSystem::setLog(LogCallback log) { impl_->log = std::move(log); }
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
