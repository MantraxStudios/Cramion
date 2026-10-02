#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/ecs/World.h"  // la definicion de ComponentRegistry::registerComponent<T>

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/project/SaveFile.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace cramion::gameplay {

using json = nlohmann::json;
using core::Quat;
using core::Vec3;

namespace {

json vec3Json(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
json quatJson(const Quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

bool readVec3(const json& j, Vec3& out) {
    if (!j.is_array() || j.size() < 3) return false;
    out = Vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    return true;
}

bool readQuat(const json& j, Quat& out) {
    if (!j.is_array() || j.size() < 4) return false;
    out = Quat{j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
    return true;
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitComponents(const std::string& list) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : list) {
        if (c == ',' || c == ';') {
            if (!trim(current).empty()) out.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!trim(current).empty()) out.push_back(trim(current));
    return out;
}

std::string dateText(std::int64_t unix_time) {
    const std::time_t t = static_cast<std::time_t>(unix_time);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
    return buffer;
}

// Componentes que nunca se copian enteros (los lleva el propio sistema).
bool skipComponent(const std::string& name) {
    return name == "Transform" || name == "Saveable" || name == "Script" || name == "CppScript" || name == "PrefabLink" ||
           name == "PrefabInstance";
}

// Objetos Saveable con su clave (las repetidas pasan a usar el UUID).
std::vector<std::pair<ecs::Entity, std::string>> saveableKeys(ecs::World& world) {
    std::vector<std::pair<ecs::Entity, std::string>> out;
    std::set<std::string> used;
    // En el orden de la jerarquia: estable entre ejecuciones.
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (!e.has<Saveable>()) return;
        std::string key = SaveSystem::keyOf(e);
        if (!used.insert(key).second) {
            key = e.uuid().toString();
            used.insert(key);
        }
        out.emplace_back(e, key);
    });
    return out;
}

SaveSlotInfo infoFromJson(const json& j) {
    SaveSlotInfo info;
    info.slot = j.value("slot", std::string());
    info.label = j.value("label", std::string());
    info.scene = j.value("scene", std::string());
    info.unix_time = j.value("unix_time", static_cast<std::int64_t>(0));
    info.date = j.value("date", dateText(info.unix_time));
    info.playtime = j.value("playtime", 0.0);
    return info;
}

}  // namespace

// --- Componente ---

void Saveable::reflect(ecs::PropertyVisitor& v) {
    v.field({"save_id", "ID de guardado", "Vacio = el UUID del objeto. Ponlo a mano para que una partida vieja lo encuentre aunque recrees el objeto"},
            save_id);
    v.field({"save_transform", "Posicion, rotacion y escala"}, save_transform);
    v.field({"save_active", "Activo/inactivo"}, save_active);
    v.field({"save_physics", "Velocidad (Rigidbody)"}, save_physics);
    v.field({"save_script", "Estado del script", "Sus propiedades y lo que devuelva OnSave() (al cargar: OnLoad(datos))"}, save_script);
    v.field({"save_children", "Transform de los hijos"}, save_children);
    v.field({"components", "Componentes", "Nombres separados por comas (Light, AudioSource...) o * para todos"}, components);
}

void registerSaveComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("Saveable") != nullptr) return;
    registry.registerComponent<Saveable>("Saveable", "Guardable (partidas)", "Scripting");
}

// --- Sistema ---

SaveSystem::SaveSystem() = default;

SaveSystem::~SaveSystem() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
}

void SaveSystem::setFolder(const std::filesystem::path& folder) {
    flush();
    folder_ = folder;
}

std::string SaveSystem::keyOf(ecs::Entity entity) {
    if (const Saveable* s = entity.tryGet<Saveable>(); s != nullptr && !trim(s->save_id).empty()) return trim(s->save_id);
    return entity.uuid().toString();
}

std::string SaveSystem::sanitizeSlot(const std::string& slot) {
    std::string out;
    for (const char c : trim(slot)) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) {
            out += '_';
        } else {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();  // Windows no los admite al final
    return out.empty() ? std::string("slot") : out;
}

std::filesystem::path SaveSystem::defaultFolder(const std::string& game, const std::filesystem::path& fallback) {
#if defined(_WIN32) && !defined(__ANDROID__)
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return std::filesystem::path(appdata) / fromUtf8(sanitizeSlot(game)) / "saves";
    }
#endif
    (void)game;
    return fallback;
}

std::filesystem::path SaveSystem::slotFile(const std::string& slot) const {
    std::filesystem::path file = folder_ / fromUtf8(sanitizeSlot(slot));
    file += kSaveExtension;
    return file;
}

void SaveSystem::beginScene(ecs::World& world, const std::string& scene_name, const std::string& scene_file) {
    scene_name_ = scene_name;
    scene_file_ = scene_file;
    baseline_.clear();
    for (const auto& [e, key] : saveableKeys(world)) baseline_.insert(key);
    autosave_timer_ = 0.0f;
}

void SaveSystem::update(ecs::World& world, float dt) {
    playtime_ += dt;
    if (autosave_interval_ <= 0.0f || folder_.empty()) return;
    autosave_timer_ += dt;
    if (autosave_timer_ >= autosave_interval_) {
        autosave_timer_ = 0.0f;
        std::string error;
        if (!save(world, autosave_slot_, "Autoguardado", &error)) std::cerr << "[Save] Autoguardado: " << error << "\n";
    }
}

void SaveSystem::setAutosave(float seconds, const std::string& slot) {
    autosave_interval_ = std::max(0.0f, seconds);
    autosave_timer_ = 0.0f;
    if (!slot.empty()) autosave_slot_ = slot;
}

json SaveSystem::captureEntity(ecs::World& world, ecs::Entity e, const Saveable& s) {
    json state = json::object();
    if (s.save_active) state["active"] = e.activeSelf();
    if (s.save_transform) {
        state["t"] = {{"p", vec3Json(e.localPosition())}, {"r", quatJson(e.localRotation())}, {"s", vec3Json(e.localScale())}};
        if (s.save_children) {
            // Hijos por indice (la jerarquia de un objeto no cambia en el juego).
            json children = json::array();
            std::vector<ecs::Entity> stack;
            for (auto it = e.children().rbegin(); it != e.children().rend(); ++it) stack.push_back(world.wrap(*it));
            while (!stack.empty()) {
                const ecs::Entity c = stack.back();
                stack.pop_back();
                children.push_back({{"p", vec3Json(c.localPosition())}, {"r", quatJson(c.localRotation())},
                                    {"s", vec3Json(c.localScale())}, {"active", c.activeSelf()}});
                for (auto it = c.children().rbegin(); it != c.children().rend(); ++it) stack.push_back(world.wrap(*it));
            }
            state["children"] = std::move(children);
        }
    }
    if (s.save_physics && physics_ != nullptr && physics_->hasBody(e)) {
        state["v"] = vec3Json(physics_->linearVelocity(e));
        state["w"] = vec3Json(physics_->angularVelocity(e));
    }
    const std::string list = trim(s.components);
    if (!list.empty()) {
        json comps = json::object();
        std::vector<std::string> names;
        if (list == "*") {
            for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
                if (!skipComponent(type.name) && type.has && type.has(world, e.handle())) names.push_back(type.name);
            }
        } else {
            names = splitComponents(list);
        }
        for (const std::string& name : names) {
            if (skipComponent(name)) continue;
            const std::string text = ecs::componentToJson(world, e, name);
            if (text.empty()) continue;
            const json c = json::parse(text, nullptr, false);
            if (!c.is_discarded()) comps[name] = c;
        }
        if (!comps.empty()) state["c"] = std::move(comps);
    }
    if (s.save_script && hooks_.capture) {
        json script = hooks_.capture(e);
        if (!script.is_null() && !(script.is_object() && script.empty())) state["script"] = std::move(script);
    }
    return state;
}

json SaveSystem::capture(ecs::World& world) {
    const auto now = std::chrono::system_clock::now();
    const std::int64_t unix_time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    json snapshot;
    snapshot["format"] = "CramionSave";
    snapshot["version"] = kSaveFormatVersion;
    snapshot["scene"] = scene_name_;
    snapshot["scene_file"] = scene_file_;
    snapshot["unix_time"] = unix_time;
    snapshot["date"] = dateText(unix_time);
    snapshot["playtime"] = playtime_;
    snapshot["values"] = values_;
    if (extra_capture) snapshot["extra"] = extra_capture();

    const auto keys = saveableKeys(world);
    std::set<std::string> present;
    std::set<entt::entity> spawned_roots;
    json entities = json::array();
    json spawned = json::array();
    for (const auto& [e, key] : keys) {
        present.insert(key);
        const Saveable& s = e.get<Saveable>();
        json state = captureEntity(world, e, s);
        state["id"] = key;
        if (baseline_.contains(key)) {
            entities.push_back(std::move(state));
            continue;
        }
        // Creado en el juego: dentro de otro objeto creado ya va en su copia.
        bool inside_spawned = false;
        for (ecs::Entity p = e.parent(); p.valid() && !inside_spawned; p = p.parent()) inside_spawned = spawned_roots.contains(p.handle());
        if (inside_spawned) continue;
        spawned_roots.insert(e.handle());
        const ecs::Entity parent = e.parent();
        state["parent"] = parent.valid() ? json(parent.has<Saveable>() ? keyOf(parent) : parent.uuid().toString()) : json(nullptr);
        state["entity"] = ecs::serializeEntity(world, e);
        spawned.push_back(std::move(state));
    }
    json destroyed = json::array();
    for (const std::string& key : baseline_) {
        if (!present.contains(key)) destroyed.push_back(key);
    }
    snapshot["entities"] = std::move(entities);
    snapshot["spawned"] = std::move(spawned);
    snapshot["destroyed"] = std::move(destroyed);
    return snapshot;
}

void SaveSystem::applyEntity(ecs::World& world, ecs::Entity e, const json& state) {
    if (const auto it = state.find("t"); it != state.end() && it->is_object()) {
        Vec3 p = e.localPosition();
        Quat r = e.localRotation();
        Vec3 s = e.localScale();
        if (it->contains("p")) readVec3((*it)["p"], p);
        if (it->contains("r")) readQuat((*it)["r"], r);
        if (it->contains("s")) readVec3((*it)["s"], s);
        e.setLocalTrs(p, r, s);
    }
    if (const auto it = state.find("children"); it != state.end() && it->is_array()) {
        std::vector<ecs::Entity> stack;
        for (auto c = e.children().rbegin(); c != e.children().rend(); ++c) stack.push_back(world.wrap(*c));
        std::size_t index = 0;
        while (!stack.empty() && index < it->size()) {
            ecs::Entity c = stack.back();
            stack.pop_back();
            const json& cs = (*it)[index++];
            Vec3 p = c.localPosition();
            Quat r = c.localRotation();
            Vec3 s = c.localScale();
            if (cs.contains("p")) readVec3(cs["p"], p);
            if (cs.contains("r")) readQuat(cs["r"], r);
            if (cs.contains("s")) readVec3(cs["s"], s);
            c.setLocalTrs(p, r, s);
            if (cs.contains("active") && cs["active"].is_boolean()) c.setActive(cs["active"].get<bool>());
            for (auto k = c.children().rbegin(); k != c.children().rend(); ++k) stack.push_back(world.wrap(*k));
        }
    }
    if (const auto it = state.find("active"); it != state.end() && it->is_boolean()) e.setActive(it->get<bool>());
    if (const auto it = state.find("c"); it != state.end() && it->is_object()) {
        for (const auto& [name, fields] : it->items()) {
            std::string error;
            if (!ecs::componentFromJson(world, e, name, fields.dump(), &error)) {
                std::cerr << "[Save] " << e.name() << "." << name << ": " << error << "\n";
            }
        }
    }
    if (physics_ != nullptr && physics_->hasBody(e)) {
        Vec3 v{};
        if (const auto it = state.find("v"); it != state.end() && readVec3(*it, v)) physics_->setLinearVelocity(e, v);
        Vec3 w{};
        if (const auto it = state.find("w"); it != state.end() && readVec3(*it, w)) physics_->setAngularVelocity(e, w);
    }
}

bool SaveSystem::apply(ecs::World& world, const json& snapshot, std::string* error) {
    if (!snapshot.is_object() || snapshot.value("format", std::string()) != "CramionSave") {
        if (error) *error = "no es una partida de Cramion";
        return false;
    }
    if (snapshot.value("version", 0) > kSaveFormatVersion) {
        if (error) *error = "la partida es de una version mas nueva del juego";
        return false;
    }
    values_ = snapshot.contains("values") && snapshot["values"].is_object() ? snapshot["values"] : json::object();
    playtime_ = snapshot.value("playtime", playtime_);
    if (extra_restore && snapshot.contains("extra")) extra_restore(snapshot["extra"]);

    std::map<std::string, ecs::Entity> by_key;
    for (const auto& [e, key] : saveableKeys(world)) by_key.emplace(key, e);

    // 1. Los que la partida ya no tenia.
    if (const auto it = snapshot.find("destroyed"); it != snapshot.end() && it->is_array()) {
        for (const json& k : *it) {
            if (!k.is_string()) continue;
            const auto found = by_key.find(k.get<std::string>());
            if (found == by_key.end() || !found->second.valid()) continue;
            if (destroy) {
                destroy(found->second);
            } else {
                world.destroy(found->second);
            }
            by_key.erase(found);
        }
    }
    // Los creados en el juego que hay AHORA (no estaban en la escena): fuera,
    // la partida trae los suyos.
    for (auto it = by_key.begin(); it != by_key.end();) {
        if (!baseline_.contains(it->first) && it->second.valid()) {
            if (destroy) {
                destroy(it->second);
            } else {
                world.destroy(it->second);
            }
            it = by_key.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Los de la escena.
    std::vector<std::pair<ecs::Entity, const json*>> restored;
    if (const auto it = snapshot.find("entities"); it != snapshot.end() && it->is_array()) {
        for (const json& state : *it) {
            const auto found = by_key.find(state.value("id", std::string()));
            if (found == by_key.end() || !found->second.valid()) continue;
            applyEntity(world, found->second, state);
            restored.emplace_back(found->second, &state);
        }
    }

    // 3. Los creados en el juego.
    if (const auto it = snapshot.find("spawned"); it != snapshot.end() && it->is_array()) {
        for (const json& state : *it) {
            const std::string text = state.value("entity", std::string());
            if (text.empty()) continue;
            ecs::Entity parent;
            if (const auto p = state.find("parent"); p != state.end() && p->is_string()) {
                const std::string pk = p->get<std::string>();
                if (const auto f = by_key.find(pk); f != by_key.end()) {
                    parent = f->second;
                } else {
                    parent = world.find(Uuid::parse(pk));
                }
            }
            ecs::Entity copy = ecs::pasteEntities(world, text, parent);
            if (!copy.valid()) continue;
            applyEntity(world, copy, state);
            by_key[state.value("id", std::string())] = copy;
            if (spawned) spawned(copy);
            restored.emplace_back(copy, &state);
        }
    }

    // 4. Los scripts, al final (ya existen todos los objetos).
    if (hooks_.restore) {
        for (const auto& [e, state] : restored) {
            if (!e.valid()) continue;
            if (const auto s = state->find("script"); s != state->end()) hooks_.restore(e, *s);
        }
    }
    return true;
}

// --- Ranuras ---

void SaveSystem::enqueue(WriteJob job) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!writer_.joinable()) writer_ = std::thread([this] { writerLoop(); });
    ++pending_writes_;
    queue_.push_back(std::move(job));
    queue_cv_.notify_one();
}

void SaveSystem::writerLoop() {
    for (;;) {
        WriteJob job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // parando y sin nada pendiente
            job = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        std::string error;
        if (!project::writeSaveFile(job.file, job.text, job.compress, &error)) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            write_error_ = utf8(job.file.filename()) + ": " + error;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --pending_writes_;
        }
        idle_cv_.notify_all();
    }
}

void SaveSystem::flush() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    idle_cv_.wait(lock, [this] { return pending_writes_.load() == 0; });
}

std::string SaveSystem::takeWriteError() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::string out;
    out.swap(write_error_);
    return out;
}

bool SaveSystem::save(ecs::World& world, const std::string& slot, const std::string& label, std::string* error) {
    if (folder_.empty()) {
        if (error) *error = "no hay carpeta de partidas";
        return false;
    }
    json snapshot = capture(world);
    snapshot["slot"] = slot;
    snapshot["label"] = label;
    // Metadatos aparte (la lista de partidas no lee las partidas enteras).
    json meta{{"slot", slot},
              {"label", label},
              {"scene", snapshot["scene"]},
              {"unix_time", snapshot["unix_time"]},
              {"date", snapshot["date"]},
              {"playtime", snapshot["playtime"]}};
    const std::filesystem::path file = slotFile(slot);
    std::filesystem::path meta_file = file;
    meta_file += ".meta";
    enqueue(WriteJob{file, snapshot.dump(-1, ' ', false, json::error_handler_t::replace), compress_});
    enqueue(WriteJob{meta_file, meta.dump(2, ' ', false, json::error_handler_t::replace), false});
    return true;
}

bool SaveSystem::read(const std::string& slot, json& snapshot, std::string* error) const {
    std::string text;
    if (!project::readSaveFile(slotFile(slot), text, error)) return false;
    snapshot = json::parse(text, nullptr, false);
    if (snapshot.is_discarded()) {
        if (error) *error = "la partida esta danada (JSON no valido)";
        return false;
    }
    return true;
}

bool SaveSystem::exists(const std::string& slot) const {
    std::error_code e;
    return !folder_.empty() && std::filesystem::is_regular_file(slotFile(slot), e);
}

bool SaveSystem::remove(const std::string& slot) {
    flush();
    std::error_code e;
    const std::filesystem::path file = slotFile(slot);
    std::filesystem::path meta = file;
    meta += ".meta";
    std::filesystem::remove(meta, e);
    return std::filesystem::remove(file, e);
}

std::optional<SaveSlotInfo> SaveSystem::info(const std::string& slot) const {
    const std::filesystem::path file = slotFile(slot);
    std::error_code e;
    if (!std::filesystem::is_regular_file(file, e)) return std::nullopt;
    std::filesystem::path meta_file = file;
    meta_file += ".meta";
    SaveSlotInfo info;
    std::ifstream in(meta_file, std::ios::binary);
    json meta = in ? json::parse(in, nullptr, false) : json();
    if (meta.is_object()) {
        info = infoFromJson(meta);
    } else {
        json snapshot;
        if (!read(slot, snapshot)) return std::nullopt;
        info = infoFromJson(snapshot);
    }
    if (info.slot.empty()) info.slot = slot;
    info.file = file;
    info.size_bytes = std::filesystem::file_size(file, e);
    return info;
}

std::vector<SaveSlotInfo> SaveSystem::list() const {
    std::vector<SaveSlotInfo> out;
    std::error_code e;
    if (folder_.empty() || !std::filesystem::is_directory(folder_, e)) return out;
    for (std::filesystem::directory_iterator it(folder_, e); !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
        if (it->path().extension() != kSaveExtension) continue;
        const std::string slot = utf8(it->path().stem());
        if (auto i = info(slot)) out.push_back(std::move(*i));
    }
    // Las mas recientes primero.
    std::sort(out.begin(), out.end(), [](const SaveSlotInfo& a, const SaveSlotInfo& b) { return a.unix_time > b.unix_time; });
    return out;
}

}  // namespace cramion::gameplay
