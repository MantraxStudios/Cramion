// DataPack (como los AssetBundles de Unity): escenas y objetos con todo lo
// que usan, en un .datapack que se monta en marcha (project/DataPack.h).
//
//   DataPack.load("Nivel2")                 monta; {name, scenes, objects, files} o nil
//   DataPack.loadScene("Nivel2", "escena")  monta y carga una escena suya
//   DataPack.instantiate("Skins", "Coche")  monta y crea un objeto (prefab) suyo
//   DataPack.unload / list / isLoaded / info
//
// Los paquetes montados sobreviven a los cambios de escena (rt.data_packs).

#include "Modules.h"

#include "CramionCore/ecs/Prefab.h"

namespace cramion::scripting::native {
namespace {

api::Value textList(const std::vector<std::string>& items) {
    api::Value::Array list;
    list.reserve(items.size());
    for (const std::string& item : items) list.emplace_back(item);
    return api::Value(std::move(list));
}

// {name, version, scenes, objects, files}.
api::Value packValue(const std::string& name, const std::string& version, const std::vector<std::string>& scenes,
                     const std::vector<std::string>& objects, std::size_t files) {
    api::Value t = api::Value::object();
    t.set("name", name);
    t.set("version", version);
    t.set("scenes", textList(scenes));
    t.set("objects", textList(objects));
    t.set("files", static_cast<double>(files));
    return t;
}

// Una entrada del paquete por nombre (sin carpeta ni extension) o ruta.
std::string pickEntry(const std::vector<std::string>& entries, const std::string& wanted_name) {
    const std::string wanted = lowerText(wanted_name);
    for (const std::string& entry : entries) {
        const std::string stem = lowerText(pathFromUtf8(entry).stem().string());
        if (stem == wanted || lowerText(entry) == wanted) return entry;
    }
    return {};
}

}  // namespace

void registerDataPackApi(Runtime& rt) {
    rt.native.function(
        "DataPack.load",
        [&rt](api::Call& c) -> api::Value {
            const project::DataPackMount* m = rt.loadDataPack(c.string(0), "DataPack.load");
            if (m == nullptr) return {};
            return packValue(m->name, "", m->scenes, m->objects, m->written.size() + m->kept.size());
        },
        {"\"Nivel2\"", "monta un .datapack (junto al juego, en DataPacks/ o ruta); devuelve {name, scenes, objects, files} o nil",
         "objeto o nil"});
    // Monta y carga una escena del paquete (la primera si no se dice cual).
    rt.native.function(
        "DataPack.loadScene",
        [&rt](api::Call& c) -> api::Value {
            const project::DataPackMount* m = rt.loadDataPack(c.string(0), "DataPack.loadScene");
            if (m == nullptr) return false;
            if (m->scenes.empty()) {
                rt.write(2, "DataPack.loadScene: \"" + m->name + "\" no tiene escenas");
                return false;
            }
            std::string chosen = m->scenes.front();
            const std::string scene_name = c.string(1, "");
            if (!scene_name.empty()) {
                chosen = pickEntry(m->scenes, scene_name);
                if (chosen.empty()) {
                    rt.write(2, "DataPack.loadScene: \"" + m->name + "\" no tiene la escena \"" + scene_name + "\"");
                    return false;
                }
            }
            rt.scene_request = rt.root / pathFromUtf8(chosen);
            return true;
        },
        {"\"Nivel2\", \"escena\"", "monta el paquete y carga su escena (la primera si no se dice cual); true/false", "booleano"});
    // Monta y crea en la escena un objeto (prefab) del paquete: el primero si
    // no se dice cual. Devuelve la entidad (nil si no se pudo).
    rt.native.function(
        "DataPack.instantiate",
        [&rt](api::Call& c) -> api::Value {
            if (rt.world == nullptr) return {};
            const project::DataPackMount* m = rt.loadDataPack(c.string(0), "DataPack.instantiate");
            if (m == nullptr) return {};
            if (m->objects.empty()) {
                rt.write(2, "DataPack.instantiate: \"" + m->name + "\" no tiene objetos (prefabs)");
                return {};
            }
            std::string chosen = m->objects.front();
            const std::string object_name = c.string(1, "");
            if (!object_name.empty()) {
                chosen = pickEntry(m->objects, object_name);
                if (chosen.empty()) {
                    rt.write(2, "DataPack.instantiate: \"" + m->name + "\" no tiene el objeto \"" + object_name + "\"");
                    return {};
                }
            }
            const std::string text = rt.prefabText(rt.root / pathFromUtf8(chosen));
            ecs::Entity copy = text.empty() ? ecs::Entity{} : ecs::instantiatePrefab(*rt.world, text);
            if (!copy.valid()) {
                rt.write(2, "DataPack.instantiate: no se pudo crear \"" + chosen + "\"");
                return {};
            }
            if (c.has(2)) copy.setWorldPosition(c.vec3(2));
            if (c.has(3)) copy.setLocalEulerDegrees(c.vec3(3));
            return rt.entityValue(copy);
        },
        {"\"Skins\", \"Coche\", posicion, rotacion", "monta el paquete y crea un objeto (prefab) suyo; devuelve la Entity o nil",
         "Entity o nil"});
    rt.native.function(
        "DataPack.unload",
        [&rt](api::Call& c) -> api::Value {
            const std::string name = c.string(0);
            const std::filesystem::path file = rt.findDataPack(name);
            for (auto it = rt.data_packs.begin(); it != rt.data_packs.end(); ++it) {
                std::error_code e;
                if (it->name == name || (!file.empty() && std::filesystem::equivalent(it->file, file, e))) {
                    project::unmountDataPack(*it, rt.root, rt.data_pack_journal);
                    rt.data_packs.erase(it);
                    if (rt.assets_changed) rt.assets_changed();
                    return true;
                }
            }
            return false;
        },
        {"\"Nivel2\"", "desmonta el paquete (borra lo que extrajo)", "booleano"});
    rt.native.function(
        "DataPack.list",
        [&rt](api::Call&) -> api::Value {
            api::Value::Array list;
            for (const project::DataPackMount& m : rt.data_packs) list.emplace_back(m.name);
            return api::Value(std::move(list));
        },
        {"", "nombres de los paquetes montados", "lista de textos"});
    rt.native.function(
        "DataPack.isLoaded",
        [&rt](api::Call& c) -> api::Value {
            const std::string name = c.string(0);
            for (const project::DataPackMount& m : rt.data_packs) {
                if (m.name == name) return true;
            }
            const std::filesystem::path file = rt.findDataPack(name);
            return !file.empty() && rt.mountedPack(file) != nullptr;
        },
        {"\"Nivel2\"", "esta montado?", "booleano"});
    // El manifiesto sin montar nada: nombre, version, escenas, archivos.
    rt.native.function(
        "DataPack.info",
        [&rt](api::Call& c) -> api::Value {
            const std::filesystem::path file = rt.findDataPack(c.string(0));
            if (file.empty()) return {};
            project::DataPackManifest manifest;
            std::string error;
            if (!project::readDataPackManifest(file, manifest, &error)) {
                rt.write(2, "DataPack.info: " + error);
                return {};
            }
            return packValue(manifest.name, manifest.version, manifest.scenes, manifest.objects, manifest.files.size());
        },
        {"\"Nivel2\"", "{name, version, scenes, objects, files} sin montar nada (o nil)", "objeto o nil"});
}

}  // namespace cramion::scripting::native
