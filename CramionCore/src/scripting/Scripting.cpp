// ScriptSystem: el estado del juego en marcha (ScriptRuntime.h), las
// fases del frame y el puente con los scripts de C++. La API la ponen los
// modulos de native/ (Modules.h).

#include "CramionCore/scripting/Scripting.h"

#include "ScriptRuntime.h"
#include "native/Modules.h"

#include "CramionCore/ai/BehaviorTree.h"
#include "CramionCore/ai/StateMachine.h"
#include "CramionCore/anim/MotionMatching.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/lighting/ProbeBaker.h"
#include "CramionCore/net/NetworkObject.h"
#include "CramionCore/profiling/Profiler.h"
#include "CramionCore/scripting/CppScripts.h"
#include "CramionCore/scripting/VisualScript.h"
#include "CramionCore/twod/System2D.h"
#include "CramionCore/vfx/VisualEffect.h"

#include <CramionDM/Input.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cramion::scripting {

using core::Vec3;

namespace {

// Presupuesto de los scripts por frame: si un frame se pasa, un aviso (una
// vez por Play) con la fase que mas tardo. 0 = sin aviso.
cvar::CVar<float> g_scripts_budget("scripts.BudgetMs", 2.0f,
                                   "Tiempo maximo de los scripts por frame (ms) antes de avisar (0 = sin aviso)",
                                   cvar::Saved, 0.0f, 100.0f);

// Modo del cursor al empezar el juego (sin codigo: un juego de estrategia
// Confined, uno en primera persona Locked). Los scripts lo cambian despues.
cvar::CVar<std::string> g_cursor_lock_mode("input.CursorLockMode", "none",
                                           "Modo del cursor al empezar el juego: none, confined (no sale de la ventana) o "
                                           "locked (oculto y en el centro)",
                                           cvar::Saved);
// En el juego exportado Escape no suelta el cursor (como en Unity): el juego
// decide (menu de pausa...). Con esto, Escape lo suelta hasta el proximo clic.
cvar::CVar<bool> g_escape_releases_cursor("input.EscapeReleasesCursor", false,
                                          "Juego exportado: Escape suelta el cursor confinado o capturado hasta el proximo clic "
                                          "(en el editor siempre)",
                                          cvar::Saved);

// Zonas del perfilador (dentro de "Scripts"), una por fase del frame.
constexpr std::array<const char*, kPhaseCount> kPhaseZones = {
    "Red y HTTP", "Gameplay", "Entrada", "Eventos", "Visual Scripts", "IA", "LateUpdate", "Envio de red",
};

}  // namespace

std::string lowerText(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool readTextFile(const std::filesystem::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string vec3Text(const Vec3& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return buffer;
}

// -----------------------------------------------------------------------------
// Componentes
// -----------------------------------------------------------------------------

std::string scriptTemplate(const std::string& class_name) { return cppScriptTemplate(class_name); }

void registerScriptComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    net::registerNetworkComponents();
    ai::registerStateMachineComponents();
    ai::registerBehaviorTreeComponents();
    registerCppScriptComponents();
    vscript::registerVisualScriptComponents();
    gameplay::registerSaveComponents();
    gameplay::registerDialogueComponents();
    anim::registerMotionMatchingComponents();  // Motion Matching (anim/MotionMatching.h)
    vfx::registerVfxComponents();              // VFX Graph (vfx/VisualEffect.h)
    twod::register2DComponents();              // sprites, tilemaps y fisica 2D (twod/System2D.h)
    lighting::registerLightingComponents();    // Light Probe Volume (lighting/ProbeBaker.h)
}

// -----------------------------------------------------------------------------
// Estado (ScriptRuntime.h)
// -----------------------------------------------------------------------------

ScriptSystem::Impl::Impl() {
    buildKeys();
    actions.setSettings(input::defaultInputActions());
    // Las acciones se leen antes que cualquier modulo de la fase Input.
    onFrame(Phase::Input, [this](float dt) { actions.update(input, dt); });
    native::registerAll(*this);
}

ScriptSystem::Impl::~Impl() = default;

void ScriptSystem::Impl::write(int level, const std::string& message) {
    if (log) {
        log(level, message);
    } else if (level == 0) {
        std::cout << "[Scripts] " << message << std::endl;
    } else {
        std::cerr << "[Scripts] " << message << std::endl;
    }
}

void ScriptSystem::Impl::fail(const std::string& where, const std::string& message) {
    ScriptError error;
    error.file = where;
    error.message = message;
    // "Scripts/Jugador.h:12: mensaje" -> linea 12.
    const std::string key = where + ":";
    const std::size_t at = message.find(key);
    if (at != std::string::npos) error.line = std::atoi(message.c_str() + at + key.size());
    if (errors.size() > 64) errors.erase(errors.begin());
    errors.push_back(error);
    write(2, message.rfind(where, 0) == 0 ? message : where + ": " + message);
}

ecs::Entity ScriptSystem::Impl::entity(entt::entity handle) const {
    if (world == nullptr || handle == entt::null || !world->registry().valid(handle)) return {};
    return world->wrap(handle);
}

ecs::Entity ScriptSystem::Impl::entityArg(const api::Call& c, std::size_t i) const {
    const entt::entity handle = c.entity(i);
    if (handle == entt::null) return {};
    const ecs::Entity e = entity(handle);
    if (!e.valid()) throw api::Error("argumento " + std::to_string(i + 1) + ": la entidad ya no existe");
    return e;
}

ecs::Entity ScriptSystem::Impl::selfEntity(const api::Call& c) const {
    const ecs::Entity e = entity(c.selfEntity());
    if (!e.valid()) throw api::Error("la entidad ya no existe");
    return e;
}

ecs::World& ScriptSystem::Impl::requireWorld() const {
    if (world == nullptr) throw api::Error("no hay escena");
    return *world;
}

// Un .datapack por ruta: tal cual, junto al ejecutable (y en su carpeta
// DataPacks/), junto a la carpeta del juego o dentro de Assets; con o sin la
// extension.
std::filesystem::path ScriptSystem::Impl::findDataPack(const std::string& name) const {
    namespace fs = std::filesystem;
    const fs::path wanted = pathFromUtf8(name);
    std::vector<fs::path> bases = {fs::path{}};
#if defined(_WIN32)
    wchar_t buffer[1024] = {};
    GetModuleFileNameW(nullptr, buffer, 1024);
    const fs::path exe = fs::path(buffer).parent_path();
    bases.push_back(exe);
    bases.push_back(exe / "DataPacks");
#endif
    if (!root.empty()) {
        bases.push_back(root.parent_path());
        bases.push_back(root.parent_path() / "DataPacks");
        bases.push_back(root);
    }
    std::error_code e;
    for (const fs::path& base : bases) {
        fs::path candidate = wanted.is_absolute() || base.empty() ? wanted : base / wanted;
        if (fs::is_regular_file(candidate, e)) return candidate;
        candidate += project::kDataPackExtension;
        if (fs::is_regular_file(candidate, e)) return candidate;
    }
    return {};
}

const project::DataPackMount* ScriptSystem::Impl::mountedPack(const std::filesystem::path& file) const {
    std::error_code e;
    for (const project::DataPackMount& m : data_packs) {
        if (std::filesystem::equivalent(m.file, file, e)) return &m;
    }
    return nullptr;
}

const project::DataPackMount* ScriptSystem::Impl::loadDataPack(const std::string& name, const char* who) {
    const std::filesystem::path file = findDataPack(name);
    if (file.empty()) {
        write(2, std::string(who) + ": no existe el DataPack \"" + name + "\"");
        return nullptr;
    }
    if (const project::DataPackMount* already = mountedPack(file)) return already;
    if (root.empty()) {
        write(2, std::string(who) + ": no hay carpeta de assets");
        return nullptr;
    }
    project::DataPackMount mount;
    std::string error;
    if (!project::mountDataPack(file, root, mount, &error, data_pack_journal)) {
        write(2, std::string(who) + ": " + error);
        return nullptr;
    }
    if (!mount.kept.empty()) {
        write(1, "DataPack \"" + mount.name + "\": " + std::to_string(mount.kept.size()) +
                     " archivos ya existian y no se sobrescribieron");
    }
    write(0, "DataPack \"" + mount.name + "\" montado (" + std::to_string(mount.written.size()) + " archivos)");
    data_packs.push_back(std::move(mount));
    if (assets_changed) assets_changed();
    return &data_packs.back();
}

void ScriptSystem::Impl::savePrefs() const {
    if (prefs_file.empty()) return;
    std::error_code e;
    std::filesystem::create_directories(prefs_file.parent_path(), e);
    std::ofstream out(prefs_file, std::ios::trunc);
    for (const auto& [key, value] : prefs) {
        if (key.find_first_of("=\n") != std::string::npos || value.find('\n') != std::string::npos) continue;
        out << key << '=' << value << '\n';
    }
}

std::filesystem::path ScriptSystem::Impl::findScene(const std::string& name) const {
    if (name.empty() || root.empty()) return {};
    std::filesystem::path direct = root / pathFromUtf8(name);
    if (direct.extension() != ".crscene") direct += ".crscene";
    std::error_code e;
    if (std::filesystem::is_regular_file(direct, e)) return direct;
    const std::string wanted = lowerText(pathFromUtf8(name).stem().string());
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, e);
         !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
        if (it->path().extension() != ".crscene") continue;
        if (lowerText(it->path().stem().string()) == wanted) return it->path();
    }
    return {};
}

std::string ScriptSystem::Impl::prefabText(const std::filesystem::path& file) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(file, ec);
    if (ec) return {};
    // Tope de la cache de texto de prefabs (LRU por bytes).
    constexpr std::size_t kPrefabCacheBytes = 48u << 20;
    const std::string key = file.string();
    PrefabFile& entry = prefabs_[key];
    entry.last_use = ++prefab_clock_;
    if (entry.text.empty() || entry.stamp != stamp) {
        prefab_bytes_ -= std::min(prefab_bytes_, entry.text.size());
        entry.text = ecs::readPrefabFile(file);
        entry.stamp = stamp;
        prefab_bytes_ += entry.text.size();
    }
    std::string text = entry.text;  // (entry puede salir abajo)
    // Fuera los menos usados hasta caber (nunca el que se acaba de pedir).
    while (prefab_bytes_ > kPrefabCacheBytes && prefabs_.size() > 1) {
        auto oldest = prefabs_.end();
        for (auto it = prefabs_.begin(); it != prefabs_.end(); ++it) {
            if (it->first == key) continue;
            if (oldest == prefabs_.end() || it->second.last_use < oldest->second.last_use) oldest = it;
        }
        if (oldest == prefabs_.end()) break;
        prefab_bytes_ -= std::min(prefab_bytes_, oldest->second.text.size());
        prefabs_.erase(oldest);
    }
    return text;
}

void ScriptSystem::Impl::buildKeys() {
    for (int k = 1; k < static_cast<int>(dm::Key::Count); ++k) {
        const char* name = dm::keyName(static_cast<dm::Key>(k));
        if (name != nullptr && name[0] != '\0') keys[lowerText(name)] = static_cast<dm::Key>(k);
    }
    const std::pair<const char*, dm::Key> aliases[] = {
        {"space", dm::Key::Space},        {"enter", dm::Key::Enter},         {"return", dm::Key::Enter},
        {"escape", dm::Key::Escape},      {"esc", dm::Key::Escape},          {"tab", dm::Key::Tab},
        {"shift", dm::Key::LeftShift},    {"leftshift", dm::Key::LeftShift}, {"rightshift", dm::Key::RightShift},
        {"ctrl", dm::Key::LeftControl},   {"control", dm::Key::LeftControl}, {"alt", dm::Key::LeftAlt},
        {"left", dm::Key::Left},          {"right", dm::Key::Right},         {"up", dm::Key::Up},
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

dm::Key ScriptSystem::Impl::key(const std::string& name) const {
    const auto it = keys.find(lowerText(name));
    return it != keys.end() ? it->second : dm::Key::Unknown;
}

bool ScriptSystem::Impl::keyDown(const std::string& name) const {
    const dm::Key k = key(name);
    return input != nullptr && k != dm::Key::Unknown && input->isKeyDown(k);
}

float ScriptSystem::Impl::axis(const std::string& name) const {
    if (input == nullptr) return 0.0f;
    const std::string n = lowerText(name);
    // Teclado + joystick virtual (tactil) + stick izquierdo / cruceta del mando.
    const auto pad = [&](dm::GamepadButton b) { return input->isGamepadButtonDown(b) ? 1.0f : 0.0f; };
    if (n == "horizontal") {
        const float k = (keyDown("d") || keyDown("right") ? 1.0f : 0.0f) - (keyDown("a") || keyDown("left") ? 1.0f : 0.0f);
        const float v = k + input->virtualStickX() + input->gamepadAxis(dm::GamepadAxis::LeftX) +
                        pad(dm::GamepadButton::DpadRight) - pad(dm::GamepadButton::DpadLeft);
        return std::clamp(v, -1.0f, 1.0f);
    }
    if (n == "vertical") {
        const float k = (keyDown("w") || keyDown("up") ? 1.0f : 0.0f) - (keyDown("s") || keyDown("down") ? 1.0f : 0.0f);
        const float v = k + input->virtualStickY() + input->gamepadAxis(dm::GamepadAxis::LeftY) +
                        pad(dm::GamepadButton::DpadUp) - pad(dm::GamepadButton::DpadDown);
        return std::clamp(v, -1.0f, 1.0f);
    }
    // Mirar: raton (o arrastrar el dedo) + stick derecho del mando.
    if (n == "mouse x") return input->mouseDeltaX() * 0.1f + input->gamepadAxis(dm::GamepadAxis::RightX) * 0.6f;
    if (n == "mouse y") return -input->mouseDeltaY() * 0.1f + input->gamepadAxis(dm::GamepadAxis::RightY) * 0.6f;
    if (n == "mouse scrollwheel") return input->scrollY();
    // Una accion del proyecto (Axis1D o el x de las demas).
    if (const input::ActionState* s = actions.state(name)) return s->value.x;
    return 0.0f;
}

bool ScriptSystem::Impl::gamepadButton(const std::string& name, dm::GamepadButton& out) {
    const std::string n = lowerText(name);
    for (int i = 0; i < static_cast<int>(dm::GamepadButton::Count); ++i) {
        const auto b = static_cast<dm::GamepadButton>(i);
        if (n == dm::gamepadButtonName(b)) {
            out = b;
            return true;
        }
    }
    return false;
}

void ScriptSystem::Impl::lockCursor(bool on) {
    // lockCursor(false) con Confined pedido tambien lo suelta (vuelve a None).
    if (!on && cursor.requested() == CursorLockMode::None) return;
    cursor.request(on ? CursorLockMode::Locked : CursorLockMode::None);
    refreshCursor();
}

void ScriptSystem::Impl::refreshCursor() {
    const CursorLockMode mode = cursor.effective();
    const bool hidden = cursor.hidden();
    const bool locked = mode == CursorLockMode::Locked;
    if (locked != cursor_locked) {
        cursor_locked = locked;
        if (cursor_lock) cursor_lock(locked);
    }
    if (mode != cursor_applied || hidden != cursor_applied_hidden) {
        cursor_applied = mode;
        cursor_applied_hidden = hidden;
        if (cursor_mode) cursor_mode(mode, hidden);
    }
}

void ScriptSystem::Impl::flushDestroys() {
    std::vector<entt::entity> queue;
    queue.swap(pending_destroy);
    for (const entt::entity handle : queue) {
        if (world == nullptr || !world->registry().valid(handle)) continue;
        // El objeto y sus hijos: los modulos sueltan lo suyo (OnDestroy de
        // los Visual Scripts...).
        std::vector<entt::entity> stack{handle};
        while (!stack.empty()) {
            const entt::entity h = stack.back();
            stack.pop_back();
            for (const auto& fn : hooks.destroying) fn(h);
            if (destroy_listener && world->registry().valid(h)) destroy_listener(world->wrap(h));
            if (!world->registry().valid(h)) continue;
            for (const entt::entity c : world->wrap(h).children()) stack.push_back(c);
        }
        world->destroy(world->wrap(handle));
    }
}

// -----------------------------------------------------------------------------
// ScriptSystem
// -----------------------------------------------------------------------------

ScriptSystem::ScriptSystem() : impl_(std::make_unique<Impl>()) {}

ScriptSystem::~ScriptSystem() { stop(); }

void ScriptSystem::setInputActions(const input::InputActionSettings& settings) {
    Impl& d = *impl_;
    const std::vector<std::string> active = d.actions.activeContexts();
    d.actions.setSettings(settings);
    if (d.running) {
        // En Play: los contextos que estaban activos siguen (con su prioridad nueva).
        d.actions.clearContexts();
        for (auto it = active.rbegin(); it != active.rend(); ++it) d.actions.addContext(*it);
    }
}

const input::InputMapper& ScriptSystem::inputMapper() const { return impl_->actions; }

void ScriptSystem::setAssetsRoot(const std::filesystem::path& root) { impl_->root = root; }

void ScriptSystem::setAssetsChangedCallback(std::function<void()> callback) {
    impl_->assets_changed = std::move(callback);
}

void ScriptSystem::setDataPackJournal(const std::filesystem::path& journal) { impl_->data_pack_journal = journal; }

void ScriptSystem::unmountDataPacks() {
    Impl& d = *impl_;
    if (d.data_packs.empty()) return;
    for (const project::DataPackMount& m : d.data_packs) project::unmountDataPack(m, d.root, d.data_pack_journal);
    d.data_packs.clear();
    if (d.assets_changed) d.assets_changed();
}

std::vector<std::string> ScriptSystem::mountedDataPacks() const {
    std::vector<std::string> names;
    for (const project::DataPackMount& m : impl_->data_packs) names.push_back(m.name);
    return names;
}

void ScriptSystem::setInput(const dm::Input* input) { impl_->input = input; }
void ScriptSystem::setPhysics(physics::PhysicsSystem* physics) { impl_->physics = physics; }
void ScriptSystem::setAudio(audio::AudioSystem* audio) { impl_->audio = audio; }
void ScriptSystem::setVideo(video::VideoSystem* video) { impl_->video = video; }
void ScriptSystem::setNavigation(navigation::NavigationSystem* navigation) { impl_->navigation = navigation; }
void ScriptSystem::setVoxels(voxel::VoxelSystem* voxels) { impl_->voxels = voxels; }
void ScriptSystem::setGraphics(GraphicsHost* graphics) { impl_->graphics = graphics; }
void ScriptSystem::setSkeletonHost(SkeletonHost host) { impl_->skeleton_host = std::move(host); }
void ScriptSystem::setCursorLock(CursorLockCallback callback) { impl_->cursor_lock = std::move(callback); }
void ScriptSystem::setVibrate(VibrateCallback callback) { impl_->vibrate = std::move(callback); }
void ScriptSystem::setTouchControls(dm::TouchControls* controls) { impl_->touch_controls = controls; }
void ScriptSystem::setScreen(ScreenHost host) { impl_->screen = std::move(host); }
void ScriptSystem::setXr(xr::XrSystem* system, const xr::XrRig* rig) {
    impl_->xr_system = system;
    impl_->xr_rig = rig;
}
void ScriptSystem::setUi(ui::UiSystem* ui) { impl_->ui = ui; }
void ScriptSystem::setCursorMode(CursorModeCallback callback) { impl_->cursor_mode = std::move(callback); }
bool ScriptSystem::cursorLocked() const { return impl_->cursor_locked; }
void ScriptSystem::releaseCursor() {
    impl_->cursor.release();
    impl_->refreshCursor();
}
CursorLockMode ScriptSystem::cursorLockMode() const { return impl_->cursor.requested(); }
CursorLockMode ScriptSystem::effectiveCursorLockMode() const { return impl_->cursor.effective(); }
bool ScriptSystem::cursorHidden() const { return impl_->cursor.hidden(); }
void ScriptSystem::cursorFocus(bool focused) {
    impl_->cursor.focus(focused);
    impl_->refreshCursor();
}
void ScriptSystem::setCursorResumeOnFocus(bool on) { impl_->cursor.setResumeOnFocus(on); }
void ScriptSystem::cursorViewActive(bool active) {
    impl_->cursor.setViewActive(active);
    impl_->refreshCursor();
}
void ScriptSystem::cursorClick() {
    impl_->cursor.click();
    impl_->refreshCursor();
}
void ScriptSystem::carryCursorToNextStart() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.cursor_carry = true;
    d.cursor_carry_mode = d.cursor.requested();
    d.cursor_carry_visible = d.cursor.visible();
}
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
        if (!line.empty() && line.back() == '\r') line.pop_back();  // escrito en Windows (CRLF), leido en Android
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) d.prefs[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void ScriptSystem::setSaveFolder(const std::filesystem::path& folder) { impl_->saves.setFolder(folder); }
gameplay::SaveSystem& ScriptSystem::saveSystem() { return impl_->saves; }
gameplay::DialogueSystem& ScriptSystem::dialogueSystem() { return impl_->dialogue; }
bool ScriptSystem::running() const { return impl_->running; }
const std::vector<ScriptError>& ScriptSystem::errors() const { return impl_->errors; }
void ScriptSystem::clearErrors() { impl_->errors.clear(); }

void ScriptSystem::start(ecs::World& world) {
    stop();
    Impl& d = *impl_;
    d.world = &world;
    d.running = true;
    d.time = 0.0f;
    d.delta = 0.0f;
    d.frame = 0;
    d.warned_budget = false;
    // El modo del cursor del proyecto (input.CursorLockMode); los scripts lo
    // cambian en Awake/Start si quieren.
    // Tras Scene.load sigue el modo que tenia el juego (como en Unity).
    if (d.cursor_carry) {
        d.cursor_carry = false;
        d.cursor.request(d.cursor_carry_mode);
        d.cursor.setVisible(d.cursor_carry_visible);
        d.refreshCursor();
    } else {
        CursorLockMode mode = CursorLockMode::None;
        if (!parseCursorLockMode(g_cursor_lock_mode.get(), mode)) {
            std::cerr << "[Input] input.CursorLockMode: modo desconocido \"" << g_cursor_lock_mode.get()
                      << "\" (none, confined o locked)" << std::endl;
        }
        d.cursor.request(mode);
        d.refreshCursor();
    }
    // Contextos del principio y las teclas que el jugador guardo.
    if (const auto saved = d.prefs.find(Impl::kBindingsPref); saved != d.prefs.end()) {
        d.actions.applyOverridesJson(saved->second);
    }
    d.actions.resetContexts();
    if (d.physics != nullptr) {
        d.physics_listener = d.physics->addListener([&d](const physics::PhysicsEvent& event) { d.events.push_back(event); });
    }
    for (const auto& fn : d.hooks.start) fn();
}

void ScriptSystem::update(ecs::World& world, float delta_seconds) {
    Impl& d = *impl_;
    if (!d.running) return;
    d.world = &world;
    d.time += delta_seconds;
    d.delta = delta_seconds;
    ++d.frame;
    d.frame_events.clear();
    d.frame_events.swap(d.events);
    using Clock = std::chrono::steady_clock;
    const Clock::time_point begin = Clock::now();
    double slowest_ms = 0.0;
    std::size_t slowest = 0;
    {
        prof::Scope zone("Scripts");
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            const auto& hooks = d.hooks.frame[phase];
            if (hooks.empty()) continue;
            const Clock::time_point phase_begin = Clock::now();
            prof::Scope phase_zone(kPhaseZones[phase]);
            for (const auto& fn : hooks) fn(delta_seconds);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - phase_begin).count();
            if (ms > slowest_ms) {
                slowest_ms = ms;
                slowest = phase;
            }
        }
        d.flushDestroys();
    }
    const double total_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    CR_PROFILE_COUNTER("Scripts (ms)", total_ms);
    const float budget = g_scripts_budget.get();
    if (budget > 0.0f && total_ms > budget && !d.warned_budget) {
        d.warned_budget = true;
        char text[256];
        std::snprintf(text, sizeof(text),
                      "Los scripts van lentos: %.2f ms en un frame (scripts.BudgetMs = %.2f). La fase que mas tardo: %s (%.2f ms).",
                      total_ms, budget, kPhaseZones[slowest], slowest_ms);
        d.write(1, text);
    }
}

void ScriptSystem::fixedUpdate(ecs::World& world, float step, int steps) {
    Impl& d = *impl_;
    if (!d.running || steps <= 0) return;
    d.world = &world;
    d.fixed_delta = step;
    for (int s = 0; s < steps; ++s) {
        for (const auto& fn : d.hooks.fixed) fn(step);
    }
}

void ScriptSystem::shiftOrigin(const core::Vec3& offset) {
    for (const auto& fn : impl_->hooks.origin_shift) fn(offset);
}

void ScriptSystem::stop() {
    Impl& d = *impl_;
    const bool was_running = d.running;
    for (const auto& fn : d.hooks.stop) fn(was_running);
    if (d.physics != nullptr && d.physics_listener >= 0) d.physics->removeListener(d.physics_listener);
    d.physics_listener = -1;
    d.native.clearHandles();
    d.events.clear();
    d.frame_events.clear();
    d.pending_destroy.clear();
    d.net_entities.clear();
    if (d.http) d.http->cancelAll();
    d.running = false;
    d.cursor.reset();  // el raton vuelve al sistema (libre y visible)
    d.refreshCursor();
}

void ScriptSystem::shutdownNetwork() {
    Impl& d = *impl_;
    if (d.network) d.network->close();
    d.network.reset();
    d.net_entities.clear();
    if (d.websockets) d.websockets->closeAll(1001, "Fin del juego");
    d.websockets.reset();
}

std::string ScriptSystem::networkStatus() const {
    const Impl& d = *impl_;
    if (!d.network || d.network->role() == net::NetRole::None) return {};
    if (d.network->isServer()) return "Servidor: " + std::to_string(d.network->players().size()) + " jugador(es)";
    if (d.network->connecting()) return "Cliente: conectando...";
    return "Cliente " + std::to_string(d.network->localId()) + " (ping " + std::to_string(d.network->ping(net::kServerId)) + " ms)";
}

void ScriptSystem::reloadFile(const std::string& file) {
    Impl& d = *impl_;
    if (!d.running) return;
    for (const auto& fn : d.hooks.reload) {
        if (fn(file)) return;
    }
}

void ScriptSystem::resetBridgeCallbacks() {
    for (const auto& fn : impl_->hooks.bridge_reset) fn();
}

void ScriptSystem::setNetVarListener(std::function<void(ecs::Entity, const std::string&, const std::string&)> listener) {
    impl_->net_var_listener = std::move(listener);
}

void ScriptSystem::setMessageListener(std::function<void(ecs::Entity, const std::string&, const std::string&)> listener) {
    impl_->message_listener = std::move(listener);
}

void ScriptSystem::setDestroyListener(std::function<void(ecs::Entity)> listener) { impl_->destroy_listener = std::move(listener); }

void ScriptSystem::destroyLater(ecs::Entity e) {
    if (e.valid()) impl_->destroyLater(e.handle());
}

namespace {

void sendEvent(ScriptSystem::Impl& d, ecs::Entity target, const std::string& method, const api::Value& arg) {
    if (!target.valid()) return;
    for (const auto& fn : d.hooks.event) fn(target, method, arg);
    // Y a los scripts de C++ del objeto (Script::onMessage).
    if (d.message_listener) d.message_listener(target, method, d.native.toJson(arg).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

}  // namespace

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, ecs::Entity source) {
    sendEvent(*impl_, target, method, impl_->entityValue(source));
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, float value) {
    sendEvent(*impl_, target, method, api::Value(value));
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, const std::string& value) {
    sendEvent(*impl_, target, method, api::Value(value));
}

void ScriptSystem::callMethod(ecs::Entity target, const std::string& method, bool value) {
    sendEvent(*impl_, target, method, api::Value(value));
}

const api::NativeApi& ScriptSystem::apiRegistry() {
    static const ScriptSystem registry;
    return registry.impl_->native;
}

api::NativeApi& ScriptSystem::nativeApi() { return impl_->native; }

std::map<std::string, std::vector<ScriptSystem::ApiMember>> ScriptSystem::apiReference() {
    std::map<std::string, std::vector<ApiMember>> out;
    for (const api::Entry& e : apiRegistry().entries()) {
        // Las funciones globales (print) van en ""; las tablas como miembros de "".
        const std::string owner = e.member() ? e.owner + ":" : e.owner;
        out[owner].push_back(ApiMember{e.name, e.kind != api::Entry::Kind::Property});
        if (!e.member() && !e.owner.empty()) {
            std::vector<ApiMember>& globals = out[""];
            const bool listed = std::any_of(globals.begin(), globals.end(),
                                            [&](const ApiMember& m) { return m.name == e.owner; });
            if (!listed) globals.push_back(ApiMember{e.owner, false});
        }
    }
    for (auto& [owner, members] : out) {
        std::sort(members.begin(), members.end(), [](const ApiMember& a, const ApiMember& b) { return a.name < b.name; });
        members.erase(std::unique(members.begin(), members.end(),
                                  [](const ApiMember& a, const ApiMember& b) { return a.name == b.name; }),
                      members.end());
    }
    return out;
}

bool ScriptSystem::run(const std::string& code, std::string* output, ecs::World* world) {
    Impl& d = *impl_;
    if (!d.console) {
        if (output != nullptr) *output = "La consola no esta disponible";
        return false;
    }
    // Fuera de Play, sobre la escena que se pase (la del editor).
    ecs::World* previous = d.world;
    if (!d.running && world != nullptr) d.world = world;
    const bool ok = d.console(code, output);
    if (!d.running) {
        // Fuera de Play no hay "final del frame" del juego: Scene.destroy y
        // Entity:destroy se hacen al terminar el comando (tambien si fallo a
        // medias: lo de antes del error ya se hizo), con el mundo del editor.
        if (world != nullptr) {
            d.flushDestroys();
        } else {
            d.pending_destroy.clear();  // sin escena no hay nada que borrar
        }
        d.world = previous;
    }
    return ok;
}

std::string ScriptSystem::bridgeCall(const std::string& request_text) {
    using json = nlohmann::json;
    Impl& d = *impl_;
    if (!d.running) return json{{"ok", false}, {"error", "el juego no esta en marcha"}}.dump();
    const json request = json::parse(request_text, nullptr, false);
    if (!request.is_object()) return json{{"ok", false}, {"error", "peticion no valida"}}.dump();
    return d.native.bridgeCall(request);
}

void ScriptSystem::setBridgeCallbackSink(std::function<void(std::uint64_t id, const std::string& args_json)> sink) {
    impl_->native.setCallbackSink(std::move(sink));
}

bool ScriptSystem::visualScriptDebug(ecs::Entity entity, VisualScriptDebug& out) {
    return impl_->vs_debug != nullptr && impl_->vs_debug->debug(entity, out);
}

void ScriptSystem::setVisualScriptDebugging(bool on) {
    if (impl_->vs_debug) impl_->vs_debug->setDebugging(on);
}

void ScriptSystem::setVisualScriptBreakpoints(const std::string& graph, const std::vector<int>& nodes) {
    if (impl_->vs_debug) impl_->vs_debug->setBreakpoints(graph, nodes);
}

bool ScriptSystem::takeVisualScriptBreak(std::string& graph, int& node, entt::entity& entity) {
    return impl_->vs_debug != nullptr && impl_->vs_debug->takeBreak(graph, node, entity);
}

std::vector<entt::entity> ScriptSystem::visualScriptObjects(const std::string& graph) const {
    return impl_->vs_debug != nullptr ? impl_->vs_debug->objects(graph) : std::vector<entt::entity>{};
}

void ScriptSystem::setVisualScriptLiveDebug(bool on) {
    if (impl_->vs_debug) impl_->vs_debug->setLive(on);
}

bool ScriptSystem::hasConstructionScript(const std::string& graph) {
    return impl_->vs_debug != nullptr && impl_->vs_debug->hasConstruction(graph);
}

bool ScriptSystem::runConstructionScript(ecs::World& world, ecs::Entity entity, std::string* error) {
    Impl& d = *impl_;
    if (d.running || d.vs_debug == nullptr) {
        if (error != nullptr) *error = d.running ? "el juego esta en marcha" : "Visual Scripting no esta disponible";
        return false;
    }
    // Como la consola fuera de Play: sobre la escena del editor, y lo que se
    // destruya (Entity:destroy) se borra al terminar.
    ecs::World* previous = d.world;
    d.world = &world;
    std::string problem;
    const bool ok = d.vs_debug->construct(entity, problem);
    d.flushDestroys();
    d.world = previous;
    if (!ok && error != nullptr) *error = problem;
    return ok;
}

bool ScriptSystem::loadTestFile(const std::string& file, std::string* error) {
    if (impl_->tests) return impl_->tests->load(file, error);
    if (error != nullptr) *error = "Las pruebas automaticas no estan disponibles";
    return false;
}

std::string ScriptSystem::testCall(const std::string& function, const std::string& arg) {
    return impl_->tests ? impl_->tests->call(function, arg) : std::string{};
}

std::string ScriptSystem::testStep(float delta_seconds) {
    return impl_->tests ? impl_->tests->step(delta_seconds) : std::string{};
}

}  // namespace cramion::scripting
