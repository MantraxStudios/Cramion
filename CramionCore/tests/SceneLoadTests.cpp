// Pruebas de la carga de escenas en segundo plano (Scene.loadAsync,
// ecs/SceneLoading.h), en consola y sin GPU: la escena actual sigue viva
// mientras se lee y se crea la nueva (oculta), el progreso nunca baja, con
// allowActivation(false) se queda en 0.9, al activar (Single) la vieja se
// descarga salvo lo DontDestroyOnLoad, el modo Additive suma y unloadAsync
// quita, recargar la misma escena no duplica nada y un archivo que no existe
// falla. Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneLoading.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;
namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

fs::path tempRoot(const char* name) {
    const fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// Una escena con `count` raices "<prefijo> i", cada una con un hijo, y una
// MeshRenderer con un modelo en la primera.
fs::path writeScene(const fs::path& folder, const std::string& name, const std::string& prefix, int count,
                    const Uuid& model = {}) {
    ecs::World w;
    w.setSceneName(name);
    for (int i = 0; i < count; ++i) {
        ecs::Entity root = w.create(prefix + " " + std::to_string(i));
        root.setLocalPosition(core::Vec3{static_cast<float>(i), 0.0f, 0.0f});
        w.create(prefix + " hijo " + std::to_string(i), root);
        if (i == 0 && model.valid()) root.add<ecs::MeshRenderer>().model.uuid = model;
    }
    const fs::path file = folder / (name + ".crscene");
    std::string error;
    if (!ecs::saveScene(w, file, &error)) std::printf("  no se pudo guardar %s: %s\n", file.string().c_str(), error.c_str());
    return file;
}

int countNamed(const ecs::World& w, const std::string& prefix) {
    int n = 0;
    w.forEachDepthFirst([&](ecs::Entity e) {
        if (e.name().rfind(prefix, 0) == 0) ++n;
    });
    return n;
}

int countActiveNamed(const ecs::World& w, const std::string& prefix) {
    int n = 0;
    w.forEachDepthFirst([&](ecs::Entity e) {
        if (e.name().rfind(prefix, 0) == 0 && e.activeInHierarchy()) ++n;
    });
    return n;
}

// Avanza hasta que la operacion llega a `until` (o termina), con un tope.
template <typename Fn>
bool pump(ecs::AsyncSceneLoader& loader, ecs::World& world, const ecs::AsyncSceneLoader::Host& host, int id,
          float until, double budget_ms, Fn&& each) {
    for (int frame = 0; frame < 20000; ++frame) {
        loader.update(world, host, budget_ms);
        const ecs::SceneLoadInfo info = loader.info(id);
        each(info);
        if (info.done || info.progress >= until) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

void testSingle(const fs::path& folder) {
    std::printf("Single con allowActivation y DontDestroyOnLoad\n");
    const Uuid model = Uuid::generate();
    const fs::path a = writeScene(folder, "Viejo", "Viejo", 3);
    const fs::path b = writeScene(folder, "Nuevo", "Nuevo", 200, model);

    ecs::World world;
    std::string error;
    check(ecs::loadScene(world, a, &error), "la escena vieja se abre");
    ecs::Entity keep = world.create("Persistente");
    ecs::Entity keep_child = world.create("Persistente hijo", keep);
    ecs::setDontDestroyOnLoad(keep_child);  // se marca su raiz
    check(ecs::isDontDestroyOnLoad(keep) && keep.get<ecs::EntityInfo>().dont_destroy_on_load,
          "dontDestroyOnLoad de un hijo marca su raiz");

    ecs::AsyncSceneLoader loader;
    loader.setActiveSceneName("Viejo");
    int model_asks = 0;
    int before = 0;
    int after = 0;
    bool old_alive_at_before = false;
    ecs::AsyncSceneLoader::Host host;
    // Un modelo que tarda 5 frames en "leerse".
    host.model_ready = [&](const Uuid& id) { return id == model && ++model_asks >= 5; };
    host.before_single = [&](const fs::path&) {
        ++before;
        old_alive_at_before = countNamed(world, "Viejo") == 6;
    };
    host.after_single = [&](const fs::path&) { ++after; };

    const int id = loader.load(b, ecs::SceneLoadMode::Single);
    check(id > 0, "loadAsync devuelve un id");
    check(loader.setAllowActivation(id, false), "allowActivation(false)");
    float last = -1.0f;
    bool monotonic = true;
    bool saw_models = false;
    bool saw_building = false;
    bool old_alive = true;
    const bool ready = pump(loader, world, host, id, 0.9f, 0.05, [&](const ecs::SceneLoadInfo& info) {
        if (info.progress + 1e-6f < last) monotonic = false;
        last = info.progress;
        if (info.progress >= 0.1f && info.progress < 0.6f) saw_models = true;
        if (info.progress > 0.6f && info.progress < 0.9f) saw_building = true;
        if (countActiveNamed(world, "Viejo") != 6) old_alive = false;
    });
    check(ready, "llega a 0.9");
    check(monotonic, "el progreso nunca baja");
    check(saw_models, "etapa de modelos (0.1-0.6) mientras el modelo se lee");
    check(saw_building, "creacion por tandas (0.6-0.9) en varios frames");
    check(model_asks >= 5, "los modelos se piden al AssetManager (Host::model_ready)");
    check(old_alive, "la escena vieja sigue viva y activa mientras carga");
    check(countNamed(world, "Nuevo") == 400 && countActiveNamed(world, "Nuevo") == 0,
          "la nueva ya esta creada, pero oculta");

    // Sin permiso se queda en 0.9 sin activarse.
    for (int i = 0; i < 30; ++i) loader.update(world, host, 0.05);
    ecs::SceneLoadInfo info = loader.info(id);
    check(!info.done && info.progress == 0.9f && before == 0, "allowActivation(false): se queda en 0.9");
    loader.takeEvents();

    loader.setAllowActivation(id, true);
    loader.update(world, host, 0.05);
    info = loader.info(id);
    check(info.done && !info.failed && info.progress == 1.0f, "al permitirlo se activa en un frame (progreso 1)");
    check(before == 1 && after == 1 && old_alive_at_before, "before_single (con la vieja aun viva) y after_single, una vez");
    check(countNamed(world, "Viejo") == 0, "la escena vieja se descargo");
    check(countActiveNamed(world, "Nuevo") == 400, "la nueva, activa");
    check(keep.valid() && keep_child.valid() && countNamed(world, "Persistente") == 2,
          "lo DontDestroyOnLoad sobrevive (con sus hijos)");
    check(world.sceneName() == "Nuevo", "el mundo lleva el nombre de la escena nueva");
    const std::vector<ecs::AsyncSceneLoader::Event> events = loader.takeEvents();
    bool activated = false;
    for (const auto& e : events) activated = activated || (e.kind == ecs::AsyncSceneLoader::Event::Kind::Activated && e.id == id);
    check(activated, "evento Activated (OnSceneActivated)");
    check(!loader.busy(), "sin operaciones pendientes");
    const std::vector<std::string> scenes = loader.loadedScenes();
    check(scenes.size() == 1 && scenes[0] == "Nuevo", "loadedScenes: la nueva");

    // Recargar la misma escena: nada duplicado, la de antes se va.
    const int again = loader.load(b, ecs::SceneLoadMode::Single);
    check(pump(loader, world, {}, again, 1.0f, 1.0, [](const ecs::SceneLoadInfo&) {}), "recargar la misma escena termina");
    check(countNamed(world, "Nuevo") == 400 && countActiveNamed(world, "Nuevo") == 400,
          "recargar: una sola copia (UUID que chocan -> nuevos)");
    check(countNamed(world, "Persistente") == 2, "recargar: lo persistente sigue");
}

void testAdditive(const fs::path& folder) {
    std::printf("Additive y unloadAsync\n");
    const fs::path base = writeScene(folder, "Base", "Base", 2);
    const fs::path extra = writeScene(folder, "Extra", "Extra", 5);
    ecs::World world;
    std::string error;
    check(ecs::loadScene(world, base, &error), "la escena base se abre");
    ecs::AsyncSceneLoader loader;
    loader.setActiveSceneName("Base");
    const int id = loader.load(extra, ecs::SceneLoadMode::Additive);
    check(pump(loader, world, {}, id, 1.0f, 1.0, [](const ecs::SceneLoadInfo&) {}) && loader.info(id).done,
          "la aditiva termina");
    check(countActiveNamed(world, "Base") == 4 && countActiveNamed(world, "Extra") == 10, "las dos escenas a la vez");
    const std::vector<std::string> scenes = loader.loadedScenes();
    check(scenes.size() == 2 && scenes[1] == "Extra", "loadedScenes: la activa y la aditiva");
    check(loader.unload("NoCargada") == 0, "unloadAsync de una escena no cargada: 0");
    const int un = loader.unload("Extra");
    check(un > 0, "unloadAsync devuelve un id");
    loader.update(world, {}, 1.0);
    check(loader.info(un).done && loader.info(un).progress == 1.0f, "la descarga termina");
    check(countNamed(world, "Extra") == 0 && countActiveNamed(world, "Base") == 4, "fuera la aditiva; la base se queda");
    bool unloaded = false;
    for (const auto& e : loader.takeEvents()) unloaded = unloaded || e.kind == ecs::AsyncSceneLoader::Event::Kind::Unloaded;
    check(unloaded, "evento Unloaded (OnSceneUnloaded)");
}

void testFailure(const fs::path& folder) {
    std::printf("Escena que no existe\n");
    ecs::World world;
    ecs::AsyncSceneLoader loader;
    const int id = loader.load(folder / "NoExiste.crscene", ecs::SceneLoadMode::Single);
    check(pump(loader, world, {}, id, 2.0f, 1.0, [](const ecs::SceneLoadInfo&) {}), "termina");
    const ecs::SceneLoadInfo info = loader.info(id);
    check(info.done && info.failed && info.progress < 0.0f && !info.error.empty(), "falla con progreso -1 y su error");
    check(!loader.info(9999).exists, "un id desconocido no existe");
}

void testBuilder(const fs::path& folder) {
    std::printf("SceneEntityBuilder (por tandas, oculto, desplazado)\n");
    const fs::path file = writeScene(folder, "Tandas", "Tanda", 50);
    std::string text;
    {
        std::FILE* f = std::fopen(file.string().c_str(), "rb");
        char buf[4096];
        std::size_t n;
        while (f != nullptr && (n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        if (f != nullptr) std::fclose(f);
    }
    ecs::SceneHeader header;
    const auto parsed = ecs::parseSceneText(text, &header);
    check(parsed != nullptr && header.name == "Tandas" && ecs::parsedEntityCount(*parsed) == 100, "parseSceneText");
    check(ecs::parseSceneText("{\"format\":\"Otro\"}", nullptr) == nullptr, "un JSON que no es una escena: nullptr");
    ecs::World world;
    ecs::SceneEntityBuilder builder(parsed, true, core::Vec3{100.0f, 0.0f, 0.0f});
    int steps = 0;
    while (!builder.step(world, 0.0)) ++steps;  // presupuesto 0: un registro por paso
    check(steps >= 99, "presupuesto 0: un registro por llamada");
    check(builder.roots().size() == 50 && builder.created() == 100, "todas las raices y los hijos");
    check(countActiveNamed(world, "Tanda") == 0, "ocultas hasta activate");
    builder.activate(world);
    check(countActiveNamed(world, "Tanda") == 100, "activate");
    const ecs::Entity first = world.wrap(builder.roots().front());
    check(first.localPosition().x == 100.0f, "las raices, desplazadas por el origen");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path folder = tempRoot("cramion_scene_load_tests");
    testBuilder(folder);
    testSingle(folder);
    testAdditive(folder);
    testFailure(folder);
    std::error_code ec;
    fs::remove_all(folder, ec);
    std::printf("\n%d/%d comprobaciones correctas\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
