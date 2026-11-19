// Pruebas de los prefabs (consola, sin GPU): crear un prefab desde una
// entidad, instanciarlo, cambios propios de cada instancia, aplicar una
// instancia al prefab y actualizar las demas, hijos anadidos y borrados,
// componentes anadidos, revertir, desempaquetar, guardar la escena y
// Scene.instantiate("Prefabs/...") desde la API de los scripts; prefabs
// anidados (3 niveles), variantes (y variantes de variantes), overrides que
// sobreviven a un cambio de la base, ciclos rechazados, paridad JSON /
// plantilla compilada y dependencias de los DataPacks. Devuelve 0 si todo va.

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Blueprint.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/PrefabSpawn.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/project/DataPack.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

ecs::Entity child(ecs::Entity e, const char* name) {
    for (const entt::entity c : e.children()) {
        const ecs::Entity x = e.world()->wrap(c);
        if (x.name() == name) return x;
    }
    return {};
}

// --- Anidados y variantes ---------------------------------------------------------

std::vector<ecs::Entity> subtreeOf(ecs::World& w, ecs::Entity root) {
    std::vector<ecs::Entity> out;
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty()) {
        const ecs::Entity e = stack.back();
        stack.pop_back();
        out.push_back(e);
        const auto& children = e.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(w.wrap(*it));
    }
    return out;
}

// El arbol como texto con los UUID de la copia cambiados por su orden.
std::string normalized(ecs::World& w, ecs::Entity root) {
    const std::vector<ecs::Entity> list = subtreeOf(w, root);
    std::string all;
    for (const ecs::Entity& e : list) all += ecs::serializeEntityRecord(w, e) + "\n";
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string id = list[i].uuid().toString();
        const std::string tag = "#" + std::to_string(i);
        for (std::size_t at = all.find(id); at != std::string::npos; at = all.find(id, at + tag.size())) all.replace(at, id.size(), tag);
    }
    return all;
}

std::vector<ecs::Entity> named(ecs::World& w, ecs::Entity root, const std::string& name) {
    std::vector<ecs::Entity> out;
    for (const ecs::Entity e : subtreeOf(w, root)) {
        if (e.name() == name) out.push_back(e);
    }
    return out;
}

void locate(const std::vector<std::filesystem::path>& files) {
    std::vector<std::pair<Uuid, std::filesystem::path>> list;
    for (const auto& f : files) list.emplace_back(ecs::prefabUuid(ecs::readPrefabFileRaw(f)), f);
    ecs::setPrefabLocations(std::move(list));
}

int jsonCount(const std::string& text, const std::string& needle) {
    int n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

void testNested(const std::filesystem::path& root) {
    std::printf("Prefabs anidados (3 niveles)\n");
    std::string error;
    cvar::Registry::instance().set("prefab.SaveNested", "true", &error, true);
    const std::filesystem::path rueda_file = root / "Prefabs" / "Rueda.crprefab";
    const std::filesystem::path eje_file = root / "Prefabs" / "Ejes" / "Eje.crprefab";
    const std::filesystem::path coche_file = root / "Prefabs" / "Coche.crprefab";

    ecs::World w;
    // Rueda > Tapa (luz); la rueda apunta a su tapa (referencia interna).
    ecs::Entity rueda = w.create("Rueda");
    rueda.add<physics::BoxCollider>().size = Vec3{1.0f, 1.0f, 0.2f};
    ecs::Entity tapa = w.create("Tapa", rueda);
    tapa.add<ecs::Light>().intensity = 1.0f;
    rueda.add<ecs::PhysBones>().colliders = {tapa.uuid()};
    check(ecs::createPrefab(w, rueda, rueda_file, &error), "crear Rueda");
    locate({rueda_file});

    // Eje con dos ruedas; la segunda con su propia intensidad (override anidado).
    ecs::Entity eje = w.create("Eje");
    ecs::Entity r1 = ecs::instantiatePrefab(w, rueda_file, eje);
    ecs::Entity r2 = ecs::instantiatePrefab(w, rueda_file, eje);
    r2.setLocalPosition(Vec3{2.0f, 0.0f, 0.0f});
    named(w, r2, "Tapa").front().get<ecs::Light>().intensity = 5.0f;
    check(ecs::createPrefab(w, eje, eje_file, &error), "crear Eje (con dos ruedas dentro)");
    const std::string eje_raw = ecs::readPrefabFileRaw(eje_file);
    check(jsonCount(eje_raw, "\"nestedPrefab\"") == 2 && eje_raw.find("\"Tapa\"") == std::string::npos &&
              eje_raw.find("\"path\": \"../Rueda.crprefab\"") != std::string::npos,
          "Eje guarda dos enlaces a Rueda (ruta relativa) y no copias");
    check(!r1.has<ecs::PrefabInstance>() && r1.get<ecs::PrefabLink>().source != r2.get<ecs::PrefabLink>().source,
          "las ruedas pasan a ser partes del Eje (ids derivados distintos)");
    locate({rueda_file, eje_file});

    // Coche con dos ejes (3 niveles).
    ecs::Entity coche = w.create("Coche");
    ecs::Entity e1 = ecs::instantiatePrefab(w, eje_file, coche);
    ecs::Entity e2 = ecs::instantiatePrefab(w, eje_file, coche);
    e2.setLocalPosition(Vec3{0.0f, 0.0f, 3.0f});
    check(e1.valid() && named(w, e1, "Tapa").size() == 2, "instanciar Eje resuelve sus ruedas");
    check(ecs::createPrefab(w, coche, coche_file, &error), "crear Coche (con dos ejes dentro)");
    locate({rueda_file, eje_file, coche_file});
    const std::string coche_raw = ecs::readPrefabFileRaw(coche_file);
    const std::string coche_text = ecs::readPrefabFile(coche_file);
    check(jsonCount(coche_raw, "\"nestedPrefab\"") == 2 && jsonCount(coche_text, "\"nestedPrefab\"") == 0 &&
              jsonCount(coche_text, "\"Tapa\"") == 4,
          "Coche: dos enlaces en disco; resuelto, cuatro ruedas planas");
    const auto deps = ecs::prefabDependencies(coche_file);
    check(deps.size() == 2, "prefabDependencies: Eje y Rueda");

    ecs::World scene;
    ecs::Entity car = ecs::instantiatePrefab(scene, coche_text);
    const auto tapas = named(scene, car, "Tapa");
    int boosted = 0;
    for (const ecs::Entity t : tapas) boosted += t.get<ecs::Light>().intensity == 5.0f ? 1 : 0;
    bool refs_ok = true;
    for (const ecs::Entity r : named(scene, car, "Rueda")) {
        const auto& colliders = r.get<ecs::PhysBones>().colliders;
        refs_ok = refs_ok && colliders.size() == 1 && colliders[0] == named(scene, r, "Tapa").front().uuid();
    }
    check(tapas.size() == 4 && boosted == 2 && refs_ok, "instancia del Coche: 4 ruedas, el override de cada eje y referencias a su propia tapa");
    const int car_revision = car.get<ecs::PrefabInstance>().revision;

    // Cambiar la Rueda se ve en Eje, en Coche y en la escena.
    ecs::World edit;
    ecs::Entity wheel = ecs::instantiatePrefab(edit, rueda_file);
    wheel.get<physics::BoxCollider>().size = Vec3{1.5f, 1.5f, 0.3f};
    named(edit, wheel, "Tapa").front().get<ecs::Light>().intensity = 2.0f;
    named(edit, wheel, "Tapa").front().get<ecs::Light>().range = 7.0f;
    check(!ecs::applyInstance(edit, wheel, rueda_file, &error).empty(), "aplicar un cambio a Rueda");
    const std::string coche_v2 = ecs::readPrefabFile(coche_file);
    check(ecs::prefabRevision(coche_v2) != car_revision, "la revision resuelta del Coche cambia (sin tocar su archivo)");
    const int synced = ecs::syncOutdatedInstances(scene, [&](const Uuid& id) { return id == ecs::prefabUuid(coche_v2) ? coche_v2 : std::string{}; });
    int big = 0;
    int kept = 0;
    int ranged = 0;
    for (const ecs::Entity r : named(scene, car, "Rueda")) big += r.get<physics::BoxCollider>().size.x == 1.5f ? 1 : 0;
    for (const ecs::Entity t : named(scene, car, "Tapa")) {
        const ecs::Light& l = t.get<ecs::Light>();
        kept += l.intensity == 5.0f ? 1 : 0;
        ranged += l.range == 7.0f ? 1 : 0;
        if (l.intensity != 5.0f && l.intensity != 2.0f) kept = -100;
    }
    check(synced == 1 && big == 4 && kept == 2 && ranged == 4,
          "la escena se pone al dia: las 4 ruedas cambian y el override del eje sobrevive");

    // Override en el Coche sobre una rueda anidada de 2 niveles, y guardar.
    ecs::World tab;
    ecs::Entity stage = ecs::instantiatePrefab(tab, coche_v2);
    const std::vector<ecs::Entity> stage_tapas = named(tab, stage, "Tapa");
    stage_tapas[0].get<ecs::Light>().color = Vec3{1.0f, 0.0f, 0.0f};
    ecs::Entity extra = tab.create("Faro", named(tab, stage, "Rueda")[0]);
    extra.add<ecs::Light>().intensity = 9.0f;
    const std::string coche_v3 = ecs::applyInstance(tab, stage, coche_file, &error);
    const std::string coche_raw3 = ecs::readPrefabFileRaw(coche_file);
    check(!coche_v3.empty() && jsonCount(coche_raw3, "\"nestedPrefab\"") == 2 && jsonCount(coche_raw3, "\"Tapa\"") == 0 &&
              coche_raw3.find("\"Faro\"") != std::string::npos && coche_raw3.find("\"patches\"") != std::string::npos,
          "guardar el Coche: siguen los enlaces, la diferencia y el hijo anadido como registro propio");
    const auto inherited = ecs::prefabInheritedOverrides(coche_file);
    const std::string tapa0 = stage_tapas[0].get<ecs::PrefabLink>().source;
    check(std::find(inherited.begin(), inherited.end(), tapa0 + "|Light|color") != inherited.end(),
          "prefabInheritedOverrides marca el color de esa tapa");
    const auto origin = ecs::prefabEntityOrigin(coche_file, tapa0);
    check(origin && !origin->variant_base && std::filesystem::equivalent(origin->file, eje_file), "prefabEntityOrigin: la tapa viene del Eje anidado");
    ecs::World again;
    ecs::Entity car3 = ecs::instantiatePrefab(again, coche_v3);
    check(named(again, car3, "Faro").size() == 1 && named(again, car3, "Tapa").size() == 4 &&
              normalized(again, car3) == normalized(tab, stage),
          "el Coche guardado vuelve a dar lo mismo");

    // Desvincular un eje: entidades propias, mismos ids.
    const std::string first_axle = named(again, car3, "Eje")[0].get<ecs::PrefabLink>().source;
    check(ecs::unlinkNestedPrefab(coche_file, first_axle, &error), "desvincular un eje anidado");
    ecs::World unlinked;
    ecs::Entity car4 = ecs::instantiatePrefab(unlinked, ecs::readPrefabFile(coche_file));
    car4.get<ecs::PrefabInstance>().revision = car3.get<ecs::PrefabInstance>().revision;
    check(jsonCount(ecs::readPrefabFileRaw(coche_file), "\"nestedPrefab\"") == 1 && normalized(unlinked, car4) == normalized(again, car3),
          "queda un enlace y el resultado no cambia");
    cvar::Registry::instance().set("prefab.SaveNested", "false", &error, true);
}

void testVariants(const std::filesystem::path& root) {
    std::printf("Variantes\n");
    std::string error;
    const std::filesystem::path base_file = root / "Prefabs" / "Orco.crprefab";
    const std::filesystem::path red_file = root / "Prefabs" / "OrcoRojo.crprefab";
    const std::filesystem::path boss_file = root / "Prefabs" / "Jefes" / "OrcoJefe.crprefab";
    ecs::World w;
    ecs::Entity orc = w.create("Orco");
    ecs::Light& light = orc.add<ecs::Light>();
    light.intensity = 10.0f;
    light.color = Vec3{1.0f, 1.0f, 1.0f};
    ecs::Entity weapon = w.create("Arma", orc);
    weapon.add<physics::BoxCollider>().size = Vec3{0.1f, 1.0f, 0.1f};
    check(ecs::createPrefab(w, orc, base_file, &error), "crear Orco");
    check(!ecs::createPrefabVariant(base_file, base_file, &error), "una variante de si mismo se rechaza");
    check(ecs::createPrefabVariant(base_file, red_file, &error) && ecs::prefabVariantBase(ecs::readPrefabFileRaw(red_file)) == ecs::prefabUuid(ecs::readPrefabFileRaw(base_file)),
          "crear la variante OrcoRojo");
    locate({base_file, red_file});
    ecs::Entity red = ecs::instantiatePrefab(w, red_file);
    check(red.valid() && red.get<ecs::Light>().intensity == 10.0f && named(w, red, "Arma").size() == 1, "una variante vacia es igual que su base");
    red.get<ecs::Light>().color = Vec3{1.0f, 0.0f, 0.0f};
    check(!ecs::applyInstance(w, red, red_file, &error).empty(), "aplicar el color rojo a la variante");
    const std::string red_raw = ecs::readPrefabFileRaw(red_file);
    check(red_raw.find("\"Arma\"") == std::string::npos && jsonCount(red_raw, "\"op\": \"set\"") == 1,
          "la variante guarda solo la diferencia (el color)");

    // Variante de variante con un hijo nuevo y su propia intensidad.
    std::filesystem::create_directories(boss_file.parent_path());
    check(ecs::createPrefabVariant(red_file, boss_file, &error), "crear OrcoJefe (variante de la variante)");
    locate({base_file, red_file, boss_file});
    ecs::Entity boss = ecs::instantiatePrefab(w, boss_file);
    boss.get<ecs::Light>().intensity = 20.0f;
    w.create("Corona", boss).add<physics::SphereCollider>();
    check(!ecs::applyInstance(w, boss, boss_file, &error).empty(), "aplicar al jefe");
    ecs::World scene;
    ecs::Entity boss_in_scene = ecs::instantiatePrefab(scene, ecs::readPrefabFile(boss_file));
    check(boss_in_scene.get<ecs::Light>().intensity == 20.0f && boss_in_scene.get<ecs::Light>().color.y == 0.0f &&
              named(scene, boss_in_scene, "Corona").size() == 1 && named(scene, boss_in_scene, "Arma").size() == 1,
          "el jefe: su intensidad, el rojo de su base y el arma de la base de su base");

    // La base cambia: lo no sobrescrito se propaga.
    ecs::Entity base_edit = ecs::instantiatePrefab(w, base_file);
    base_edit.get<ecs::Light>().intensity = 15.0f;
    base_edit.get<ecs::Light>().color = Vec3{0.0f, 1.0f, 0.0f};
    named(w, base_edit, "Arma").front().get<physics::BoxCollider>().size = Vec3{0.2f, 2.0f, 0.2f};
    check(!ecs::applyInstance(w, base_edit, base_file, &error).empty(), "cambiar Orco (intensidad, color y arma)");
    ecs::World check_world;
    ecs::Entity red2 = ecs::instantiatePrefab(check_world, ecs::readPrefabFile(red_file));
    check(red2.get<ecs::Light>().intensity == 15.0f && red2.get<ecs::Light>().color.x == 1.0f && red2.get<ecs::Light>().color.y == 0.0f,
          "OrcoRojo hereda la intensidad nueva y conserva su rojo (override que sobrevive)");
    const std::string boss_text = ecs::readPrefabFile(boss_file);
    const int synced = ecs::syncOutdatedInstances(scene, [&](const Uuid& id) { return id == ecs::prefabUuid(boss_text) ? boss_text : std::string{}; });
    check(synced == 1 && boss_in_scene.get<ecs::Light>().intensity == 20.0f && boss_in_scene.get<ecs::Light>().color.x == 1.0f &&
              boss_in_scene.get<ecs::Light>().color.y == 0.0f &&
              named(scene, boss_in_scene, "Arma").front().get<physics::BoxCollider>().size.y == 2.0f,
          "la instancia del jefe en la escena recibe el arma nueva y conserva lo suyo");

    // Aplicar / revertir un override suelto (a la variante y a la base).
    ecs::Entity red_inst = ecs::instantiatePrefab(scene, ecs::readPrefabFile(red_file));
    red_inst.get<ecs::Light>().range = 50.0f;
    red_inst.get<ecs::Light>().intensity = 3.0f;
    ecs::recordOverrides(scene, red_inst, ecs::readPrefabFile(red_file));
    const std::string src = red_inst.get<ecs::PrefabLink>().source;
    const auto& ov = red_inst.get<ecs::PrefabInstance>().overrides;
    check(ov.size() == 2, "dos cambios propios en la instancia de OrcoRojo");
    check(ecs::revertOverride(scene, red_inst, src + "|Light|intensity", ecs::readPrefabFile(red_file)) &&
              red_inst.get<ecs::Light>().intensity == 15.0f && red_inst.get<ecs::Light>().range == 50.0f,
          "revertir un override: vuelve al valor del prefab y el otro se queda");
    const std::string to_base = ecs::applyOverride(scene, red_inst, src + "|Light|range", red_file, /*to_base=*/true, &error);
    ecs::World fresh;
    ecs::Entity orc3 = ecs::instantiatePrefab(fresh, ecs::readPrefabFile(base_file));
    check(!to_base.empty() && orc3.get<ecs::Light>().range == 50.0f && red_inst.get<ecs::PrefabInstance>().overrides.empty(),
          "aplicar un override a la base: el Orco lo recibe");
    red_inst.get<ecs::Light>().color = Vec3{0.0f, 0.0f, 1.0f};
    const std::string to_variant = ecs::applyOverride(scene, red_inst, src + "|Light|color", red_file, false, &error);
    ecs::Entity red4 = ecs::instantiatePrefab(fresh, to_variant);
    check(!to_variant.empty() && red4.get<ecs::Light>().color.z == 1.0f && orc3.get<ecs::Light>().color.z == 0.0f,
          "aplicar un override a este prefab (la variante) no toca la base");

    // En la pestana del prefab: revertir la diferencia de la variante.
    ecs::World tab;
    ecs::Entity stage = ecs::instantiatePrefab(tab, ecs::readPrefabFile(red_file));
    const auto inherited = ecs::prefabInheritedOverrides(red_file);
    const std::string key = stage.get<ecs::PrefabLink>().source + "|Light|color";
    check(std::find(inherited.begin(), inherited.end(), key) != inherited.end() &&
              ecs::revertInheritedOverride(tab, stage, key, red_file) && stage.get<ecs::Light>().color.y == 1.0f,
          "pestana del prefab: el color sale como override y revertirlo pone el de la base");

    // Desvincular la variante: prefab normal con lo mismo.
    const auto shape = [&](ecs::Entity e) {
        e.get<ecs::PrefabInstance>().revision = 0;
        return normalized(fresh, e);
    };
    const std::string before = shape(ecs::instantiatePrefab(fresh, ecs::readPrefabFile(boss_file)));
    check(ecs::detachPrefabVariant(boss_file, &error) && ecs::prefabVariantBase(ecs::readPrefabFileRaw(boss_file)) == Uuid{},
          "desvincular OrcoJefe de su base");
    check(shape(ecs::instantiatePrefab(fresh, ecs::readPrefabFile(boss_file))) == before, "y da lo mismo que antes");

    // Variante creada desde una instancia con cambios.
    ecs::Entity green = ecs::instantiatePrefab(w, base_file);
    green.get<ecs::Light>().color = Vec3{0.0f, 0.5f, 0.0f};
    const std::filesystem::path green_file = root / "Prefabs" / "OrcoVerde.crprefab";
    const std::string green_text = ecs::createVariantFromInstance(w, green, base_file, green_file, &error);
    check(!green_text.empty() && ecs::prefabVariantBase(ecs::readPrefabFileRaw(green_file)).valid() &&
              green.get<ecs::PrefabInstance>().prefab.uuid == ecs::prefabUuid(green_text) && green.get<ecs::PrefabInstance>().overrides.empty(),
          "crear variante desde una instancia: la instancia pasa a la variante");
}

void testCycles(const std::filesystem::path& root) {
    std::printf("Ciclos\n");
    std::string error;
    // A variante de B y B variante de A (a mano).
    const std::filesystem::path a = root / "Prefabs" / "CicloA.crprefab";
    const std::filesystem::path b = root / "Prefabs" / "CicloB.crprefab";
    const std::string ua = Uuid::generate().toString();
    const std::string ub = Uuid::generate().toString();
    const auto write = [](const std::filesystem::path& f, const std::string& text) {
        std::ofstream out(f, std::ios::binary);
        out << text;
    };
    write(a, "{\"format\":\"CramionPrefab\",\"version\":1,\"uuid\":\"" + ua + "\",\"name\":\"A\",\"revision\":1,\"variantOf\":{\"uuid\":\"" + ub +
                 "\",\"path\":\"CicloB.crprefab\"},\"patches\":[],\"entities\":[]}");
    write(b, "{\"format\":\"CramionPrefab\",\"version\":1,\"uuid\":\"" + ub + "\",\"name\":\"B\",\"revision\":1,\"variantOf\":{\"uuid\":\"" + ua +
                 "\",\"path\":\"CicloA.crprefab\"},\"patches\":[],\"entities\":[]}");
    ecs::World w;
    std::string why;
    const std::string resolved = ecs::resolvePrefabText(ecs::readPrefabFileRaw(a), a, &why);
    check(resolved.empty() && why.find("ciclo") != std::string::npos && !ecs::instantiatePrefab(w, a).valid(),
          "variantes en circulo: se rechaza (texto vacio, sin instancia)");

    // Meter un Coche dentro de su propia Rueda y aplicar: ciclo.
    cvar::Registry::instance().set("prefab.SaveNested", "true", &error, true);
    const std::filesystem::path rueda_file = root / "Prefabs" / "Rueda.crprefab";
    const std::filesystem::path coche_file = root / "Prefabs" / "Coche.crprefab";
    locate({rueda_file, root / "Prefabs" / "Ejes" / "Eje.crprefab", coche_file});
    const std::string rueda_before = ecs::readPrefabFileRaw(rueda_file);
    ecs::Entity wheel = ecs::instantiatePrefab(w, rueda_file);
    ecs::instantiatePrefab(w, coche_file, wheel);
    error.clear();
    const std::string out = ecs::applyInstance(w, wheel, rueda_file, &error);
    check(out.empty() && error.find("ciclo") != std::string::npos && ecs::readPrefabFileRaw(rueda_file) == rueda_before,
          "un prefab dentro de si mismo (a traves de otros): se rechaza y el archivo no cambia");
    cvar::Registry::instance().set("prefab.SaveNested", "false", &error, true);
}

void testParity(const std::filesystem::path& root) {
    std::printf("Paridad JSON / plantilla compilada / por tandas\n");
    const std::filesystem::path pf = root / "Prefabs";
    locate({pf / "Rueda.crprefab", pf / "Ejes" / "Eje.crprefab", pf / "Coche.crprefab", pf / "Orco.crprefab", pf / "OrcoRojo.crprefab",
            pf / "Jefes" / "OrcoJefe.crprefab", pf / "OrcoVerde.crprefab"});
    for (const char* name : {"Coche.crprefab", "OrcoRojo.crprefab", "Jefes/OrcoJefe.crprefab", "OrcoVerde.crprefab"}) {
        const std::filesystem::path f = root / "Prefabs" / name;
        const std::string text = ecs::readPrefabFile(f);
        ecs::World w;
        const ecs::Entity by_json = ecs::instantiatePrefab(w, text);
        const auto prefab = ecs::compilePrefabTemplate(text);
        const ecs::Entity by_template = prefab ? ecs::instantiatePrefabTemplate(w, *prefab) : ecs::Entity{};
        // Con el texto sin resolver tambien (parsePrefabDocument lo resuelve).
        const auto raw_template = ecs::compilePrefabTemplate(ecs::readPrefabFileRaw(f));
        ecs::PrefabSpawn spawn(raw_template);
        while (raw_template && !spawn.step(w, 100.0)) {
        }
        const ecs::Entity by_raw = ecs::instantiatePrefab(w, ecs::readPrefabFileRaw(f));
        const std::string a = by_json.valid() ? normalized(w, by_json) : std::string{};
        const bool same = by_json.valid() && by_template.valid() && spawn.root().valid() && by_raw.valid() &&
                          a == normalized(w, by_template) && a == normalized(w, spawn.root()) && a == normalized(w, by_raw);
        std::string what = std::string("mismo resultado por JSON, plantilla, tandas y texto sin resolver: ") + name;
        check(same, what.c_str());
    }
}

void testDataPack(const std::filesystem::path& root) {
    std::printf("DataPacks\n");
    assets::AssetDatabase database;
    database.open(root);
    const auto collection = project::collectDependencies({root / "Prefabs" / "Coche.crprefab", root / "Prefabs" / "OrcoRojo.crprefab"}, root, database);
    bool eje = false;
    bool rueda = false;
    bool orco = false;
    for (const auto& f : collection.files) {
        eje = eje || f.filename() == "Eje.crprefab";
        rueda = rueda || f.filename() == "Rueda.crprefab";
        orco = orco || f.filename() == "Orco.crprefab";
    }
    check(eje && rueda && orco, "el DataPack de Coche y OrcoRojo incluye Eje, Rueda y Orco");
}

// Revision: lo que se arreglo despues de la primera entrega.
void testReview(const std::filesystem::path& root) {
    std::printf("Revision\n");
    std::string error;
    const std::filesystem::path pf = root / "Prefabs";
    locate({pf / "Rueda.crprefab", pf / "Ejes" / "Eje.crprefab", pf / "Coche.crprefab", pf / "Orco.crprefab", pf / "OrcoRojo.crprefab",
            pf / "OrcoVerde.crprefab"});

    // Un ciclo detras de otra instancia que si se podia enlazar: la instancia
    // se queda como estaba (antes, la primera ya habia perdido su enlace).
    cvar::Registry::instance().set("prefab.SaveNested", "true", &error, true);
    {
        ecs::World w;
        ecs::Entity wheel = ecs::instantiatePrefab(w, pf / "Rueda.crprefab");
        ecs::Entity orc = ecs::instantiatePrefab(w, pf / "Orco.crprefab", wheel);
        ecs::instantiatePrefab(w, pf / "Coche.crprefab", wheel);
        const std::string orc_source = orc.get<ecs::PrefabLink>().source;
        const std::string before = ecs::readPrefabFileRaw(pf / "Rueda.crprefab");
        error.clear();
        check(ecs::applyInstance(w, wheel, pf / "Rueda.crprefab", &error).empty() && error.find("ciclo") != std::string::npos &&
                  orc.has<ecs::PrefabInstance>() && orc.get<ecs::PrefabLink>().source == orc_source &&
                  ecs::readPrefabFileRaw(pf / "Rueda.crprefab") == before,
              "ciclo rechazado: las instancias de dentro no se tocan (ni la que iba antes)");
    }
    cvar::Registry::instance().set("prefab.SaveNested", "false", &error, true);

    // Base perdida: ni se instancia ni se sincroniza con el texto sin resolver.
    {
        const std::filesystem::path lost = pf / "SinBase.crprefab";
        std::ofstream(lost, std::ios::binary)
            << "{\"format\":\"CramionPrefab\",\"version\":1,\"uuid\":\"" << Uuid::generate().toString()
            << "\",\"name\":\"SinBase\",\"revision\":1,\"variantOf\":{\"uuid\":\"" << Uuid::generate().toString()
            << "\",\"path\":\"NoEsta.crprefab\"},\"patches\":[],\"entities\":[{\"uuid\":\"" << Uuid::generate().toString()
            << "\",\"name\":\"Suelto\",\"parent\":null,\"components\":{}}]}";
        const std::string raw = ecs::readPrefabFileRaw(lost);
        ecs::World w;
        check(ecs::readPrefabFile(lost).empty() && !ecs::instantiatePrefab(w, raw).valid() && ecs::compilePrefabTemplate(raw) == nullptr,
              "variante sin base: no se instancia (ni por JSON ni por plantilla)");
        ecs::Entity red = ecs::instantiatePrefab(w, pf / "OrcoRojo.crprefab");
        const std::size_t parts = subtreeOf(w, red).size();
        ecs::syncInstance(w, red, raw);
        check(red.valid() && subtreeOf(w, red).size() == parts && parts > 1, "sincronizar con un texto que no se resuelve no borra nada");
    }

    // Escribir los datos de Blueprint en una variante no la aplana.
    {
        ecs::BlueprintInfo info;
        info.description = "variante con datos de Blueprint";
        check(ecs::writeBlueprintInfo(pf / "OrcoRojo.crprefab", info, &error), "writeBlueprintInfo en una variante");
        const std::string raw = ecs::readPrefabFileRaw(pf / "OrcoRojo.crprefab");
        check(ecs::prefabVariantBase(raw).valid() && raw.find("\"Arma\"") == std::string::npos && !ecs::readPrefabFile(pf / "OrcoRojo.crprefab").empty(),
              "la variante sigue siendo variante (sin copiar las entidades de la base)");
    }

    // Consultas del editor sobre un prefab de siempre: nada (sin parsearlo).
    check(!ecs::prefabEntityOrigin(pf / "Farola.crprefab", "x") && ecs::prefabInheritedOverrides(pf / "Farola.crprefab").empty() &&
              ecs::prefabDependencies(pf / "Farola.crprefab").empty(),
          "origen, diferencias y dependencias de un prefab plano: vacios");
}

void testFlatUnchanged(const std::filesystem::path& root) {
    std::printf("Retrocompatibilidad\n");
    const std::filesystem::path f = root / "Prefabs" / "Farola.crprefab";
    const std::string raw = ecs::readPrefabFileRaw(f);
    check(!raw.empty() && ecs::readPrefabFile(f) == raw && ecs::resolvePrefabText(raw) == raw && !ecs::prefabNeedsResolve(raw),
          "un prefab de siempre se lee tal cual (sin parsear de mas)");
}

}  // namespace

int main() {
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    scripting::registerScriptComponents();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_prefab_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path file = root / "Prefabs" / "Farola.crprefab";

    std::printf("Crear e instanciar\n");
    ecs::World world;
    ecs::Entity lamp = world.create("Farola");
    lamp.setWorldPosition(Vec3{10.0f, 0.0f, 5.0f});
    lamp.add<physics::BoxCollider>().size = Vec3{0.3f, 3.0f, 0.3f};
    ecs::Entity bulb = world.create("Bombilla", lamp);
    bulb.setLocalPosition(Vec3{0.0f, 3.0f, 0.0f});
    ecs::Light& light = bulb.add<ecs::Light>();
    light.type = ecs::LightType::Point;
    light.intensity = 10.0f;
    light.color = Vec3{1.0f, 0.8f, 0.5f};
    std::string error;
    check(ecs::createPrefab(world, lamp, file, &error) && std::filesystem::exists(file), "crear el prefab desde una entidad");
    const std::string v1 = ecs::readPrefabFile(file);
    check(ecs::prefabRevision(v1) == 1 && ecs::prefabName(v1) == "Farola" && ecs::prefabUuid(v1).valid() &&
              lamp.has<ecs::PrefabInstance>() && bulb.has<ecs::PrefabLink>() &&
              lamp.get<ecs::PrefabInstance>().prefab.uuid == ecs::prefabUuid(v1),
          "el archivo tiene revision, nombre y UUID; la entidad pasa a ser su instancia");
    check(v1.find("PrefabLink") == std::string::npos && v1.find("\"position\": [\n") != std::string::npos,
          "el archivo no guarda los enlaces");
    ecs::Entity a = ecs::instantiatePrefab(world, file);
    ecs::Entity b = ecs::instantiatePrefab(world, file);
    a.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    b.setWorldPosition(Vec3{20.0f, 0.0f, 0.0f});
    check(a.valid() && b.valid() && a.uuid() != b.uuid() && child(a, "Bombilla").valid() &&
              child(a, "Bombilla").get<ecs::Light>().intensity == 10.0f && a.has<physics::BoxCollider>(),
          "instanciar dos copias con sus hijos y componentes");
    check(ecs::prefabInstances(world, ecs::prefabUuid(v1)).size() == 3 && ecs::prefabRoot(child(a, "Bombilla")) == a,
          "prefabInstances y prefabRoot");
    ecs::recordOverrides(world, a, v1);
    ecs::recordOverrides(world, lamp, v1);
    check(a.get<ecs::PrefabInstance>().overrides.empty() && lamp.get<ecs::PrefabInstance>().overrides.empty(),
          "una instancia recien creada no tiene cambios propios (ni por su posicion)");

    std::printf("Cambios propios y aplicar\n");
    // A: su propia intensidad y un componente mas.
    child(a, "Bombilla").get<ecs::Light>().intensity = 25.0f;
    a.add<physics::SphereCollider>();
    ecs::recordOverrides(world, a, v1);
    const auto& ov = a.get<ecs::PrefabInstance>().overrides;
    check(ov.size() == 2, "se apuntan los cambios propios de una instancia");
    // B se cambia y se aplica al prefab: color nuevo y un hijo nuevo.
    child(b, "Bombilla").get<ecs::Light>().color = Vec3{0.2f, 0.6f, 1.0f};
    child(b, "Bombilla").get<ecs::Light>().intensity = 12.0f;
    ecs::Entity sign = world.create("Cartel", b);
    sign.add<physics::BoxCollider>();
    const std::string v2 = ecs::applyInstance(world, b, file, &error);
    check(!v2.empty() && ecs::prefabRevision(v2) == 2 && sign.has<ecs::PrefabLink>() && b.get<ecs::PrefabInstance>().overrides.empty(),
          "aplicar una instancia crea la revision 2 (con el hijo nuevo)");
    {
        const nlohmann::json j = nlohmann::json::parse(v2, nullptr, false);
        const nlohmann::json& pos = j["entities"][0]["components"]["Transform"]["position"];
        check(b.worldPosition().x == 20.0f && pos.is_array() && pos[0].get<float>() == 0.0f,
              "la posicion de la instancia no pasa al prefab");
    }
    ecs::syncInstance(world, a, v2);
    ecs::syncInstance(world, lamp, v2);
    const ecs::Light& la = child(a, "Bombilla").get<ecs::Light>();
    check(std::abs(la.color.z - 1.0f) < 1e-5f && la.intensity == 25.0f,
          "la otra instancia recibe el color nuevo y conserva su intensidad propia");
    check(child(a, "Cartel").valid() && child(a, "Cartel").has<physics::BoxCollider>() && child(lamp, "Cartel").valid(),
          "y el hijo nuevo del prefab aparece en todas");
    check(a.has<physics::SphereCollider>() && a.worldPosition().x == 0.0f && a.get<ecs::PrefabInstance>().revision == 2,
          "conserva su componente anadido y su posicion; queda en la revision 2");
    check(std::abs(child(lamp, "Bombilla").get<ecs::Light>().intensity - 12.0f) < 1e-5f, "una instancia sin cambios lo recibe todo");

    std::printf("Borrar, revertir y desempaquetar\n");
    // A borra el cartel: al actualizar no vuelve.
    world.destroy(child(a, "Cartel"));
    ecs::recordOverrides(world, a, v2);
    ecs::syncInstance(world, a, v2);
    check(!child(a, "Cartel").valid(), "un hijo borrado en la instancia no vuelve al actualizar");
    // Revertir: todo como el prefab.
    ecs::revertInstance(world, a, v2);
    check(child(a, "Cartel").valid() && std::abs(child(a, "Bombilla").get<ecs::Light>().intensity - 12.0f) < 1e-5f &&
              !a.has<physics::SphereCollider>() && a.get<ecs::PrefabInstance>().overrides.empty() && a.worldPosition().x == 0.0f,
          "revertir: igual que el prefab (salvo su posicion)");
    // El prefab quita un hijo: desaparece de las instancias.
    world.destroy(child(b, "Cartel"));
    const std::string v3 = ecs::applyInstance(world, b, file, &error);
    ecs::syncInstance(world, a, v3);
    check(!child(a, "Cartel").valid() && ecs::prefabRevision(v3) == 3, "quitar un hijo del prefab lo quita de las instancias");
    ecs::unpackInstance(world, lamp);
    check(!lamp.has<ecs::PrefabInstance>() && !child(lamp, "Bombilla").has<ecs::PrefabLink>() &&
              ecs::prefabInstances(world, ecs::prefabUuid(v3)).size() == 2,
          "desempaquetar: entidades normales");

    std::printf("Escenas y la API de los scripts\n");
    child(a, "Bombilla").get<ecs::Light>().intensity = 40.0f;
    ecs::recordOverrides(world, a, v3);
    const std::string scene = ecs::serializeWorld(world);
    ecs::World loaded;
    check(ecs::deserializeWorld(loaded, scene), "guardar y cargar la escena");
    const auto instances = ecs::prefabInstances(loaded, ecs::prefabUuid(v3));
    ecs::Entity la2;
    for (ecs::Entity e : instances) {
        if (e.uuid() == a.uuid()) la2 = e;
    }
    check(instances.size() == 2 && la2.valid() && la2.get<ecs::PrefabInstance>().overrides.size() == 1 &&
              child(la2, "Bombilla").get<ecs::PrefabLink>().source == child(a, "Bombilla").get<ecs::PrefabLink>().source,
          "la escena guarda las instancias, sus enlaces y sus cambios propios");
    // Una revision nueva con la escena cerrada: al abrirla se actualiza (y respeta el cambio propio).
    child(b, "Bombilla").get<ecs::Light>().range = 33.0f;
    const std::string v4 = ecs::applyInstance(world, b, file, &error);
    const int synced = ecs::syncOutdatedInstances(loaded, [&](const Uuid& id) { return id == ecs::prefabUuid(v4) ? v4 : std::string{}; });
    check(synced == 2 && la2.get<ecs::PrefabInstance>().revision == 4 && child(la2, "Bombilla").get<ecs::Light>().range == 33.0f && child(la2, "Bombilla").get<ecs::Light>().intensity == 40.0f,
          "al abrir una escena con una revision vieja, se actualiza respetando lo propio");

    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    scripts.start(world);
    using scripting::api::Value;
    scripting::api::NativeApi& api = scripts.nativeApi();
    const Value f = api.call("Scene.instantiate", {Value("Prefabs/Farola"), Value(core::Vec3{1, 2, 3}), Value(core::Vec3{0, 90, 0})});
    const Value g = api.call("Scene.instantiate", {Value("Prefabs/Farola.crprefab")});
    const Value nada = api.call("Scene.instantiate", {Value("Prefabs/NoExiste")});
    const ecs::Entity farola = world.wrap(f.asEntity());
    const bool found = farola.valid() && api.call("Entity:find", {Value("Bombilla")}, f).asEntity() != entt::entity{entt::null};
    std::printf("  (%s %.1f)\n", farola.valid() ? farola.name().c_str() : "-", farola.valid() ? farola.worldPosition().y : 0.0f);
    check(farola.valid() && farola.name() == "Farola" && std::abs(farola.worldPosition().y - 2.0f) < 1e-4f && found &&
              world.wrap(g.asEntity()).valid() && nada.asEntity() == entt::entity{entt::null} &&
              ecs::prefabInstances(world, ecs::prefabUuid(v4)).size() == 4,
          "Scene.instantiate(\"Prefabs/Farola\", posicion, giro) desde la API");
    scripts.stop();

    testNested(root);
    testVariants(root);
    testCycles(root);
    testParity(root);
    testDataPack(root);
    testFlatUnchanged(root);
    testReview(root);

    std::filesystem::remove_all(root, ec);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
