// Pruebas de native/GameplayApi.cpp: Save (partidas, valores, carga en la
// misma escena y en otra), Text (localizacion y avisos del idioma) y
// Dialogue (eventos, opciones, variables y mensajes a los scripts de C++).

#include "ApiTest.h"

#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/gameplay/Localization.h"
#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/scripting/CppScripts.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <tuple>

using namespace cramion;
using namespace cramion::apitest;

namespace {

struct Recorder {
    std::vector<std::pair<std::uint64_t, json>> calls;                         // callbacks (id, argumentos)
    std::vector<std::tuple<entt::entity, std::string, std::string>> messages;  // a los scripts de C++

    void attach(scripting::ScriptSystem& scripts) {
        scripts.setBridgeCallbackSink([this](std::uint64_t id, const std::string& args) { calls.emplace_back(id, json::parse(args)); });
        scripts.setMessageListener([this](ecs::Entity e, const std::string& method, const std::string& value) {
            messages.emplace_back(e.handle(), method, value);
        });
    }
    // Los argumentos de la ultima llamada al callback `id` (null si no hubo).
    json last(std::uint64_t id) const {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
            if (it->first == id) return it->second;
        }
        return nullptr;
    }
    int count(std::uint64_t id) const {
        int n = 0;
        for (const auto& c : calls) n += c.first == id ? 1 : 0;
        return n;
    }
    int countMessages(const std::string& method) const {
        int n = 0;
        for (const auto& m : messages) n += std::get<1>(m) == method ? 1 : 0;
        return n;
    }
    bool message(entt::entity e, const std::string& method, const std::string& value) const {
        for (const auto& [h, m, v] : messages) {
            if (h == e && m == method && v == value) return true;
        }
        return false;
    }
};

bool near(const core::Vec3& a, const core::Vec3& b) {
    return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

Value fn(std::uint64_t id) { return Value::function(id); }

Value named(const char* key, Value v) {
    Value o = Value::object();
    o.set(key, std::move(v));
    return o;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_gameplay_api_test";
    std::filesystem::remove_all(root);
    const std::filesystem::path assets = root / "Assets";
    write(assets / "Escenas" / "Nivel1.crscene", "{\"entities\":[]}");
    write(assets / "Escenas" / "Nivel2.crscene", "{\"entities\":[]}");
    // El dialogo de ejemplo, con su evento dirigido al script del Mercader.
    gameplay::DialogueAsset dialog = gameplay::makeExampleDialogue("Mercader");
    for (gameplay::DialogueNode& node : dialog.nodes) {
        if (node.type == gameplay::DialogueNodeType::Event) {
            node.target = "Mercader";
            node.method = "OnComprar";
        }
    }
    gameplay::saveDialogue(assets / "Dialogos" / "Mercader.crdialog", dialog);

    ApiFixture t(false);
    Recorder rec;
    rec.attach(t.scripts);
    t.scripts.setAssetsRoot(assets);
    t.scripts.setSaveFolder(root / "saves");
    t.scripts.setSceneName("Nivel1");
    ecs::Entity caja = t.world.create("Caja");
    caja.add<gameplay::Saveable>();
    caja.add<scripting::CppScript>().class_name = "Caja";
    caja.setLocalPosition({1.0f, 2.0f, 3.0f});
    ecs::Entity piedra = t.world.create("Piedra");  // Saveable sin script
    piedra.add<gameplay::Saveable>();
    ecs::Entity mercader = t.world.create("Mercader");
    t.scripts.start(t.world);
    gameplay::SaveSystem& saves = t.scripts.saveSystem();

    // --- Save: valores sueltos ---
    std::printf("Save\n");
    t.call("Save.setValue", {Value("oro"), Value(120)});
    t.call("Save.setValue", {Value("pos"), Value(core::Vec3{1.0f, 2.0f, 3.0f})});
    t.call("Save.setValue", {Value("yo"), ApiFixture::entity(caja)});
    Value lista = Value(Value::Array{Value(1), Value("dos")});
    t.call("Save.setValue", {Value("lista"), lista});
    check(t.call("Save.getValue", {Value("oro")}).asNumber() == 120.0 && saves.values()["oro"].is_number_integer(),
          "Save.setValue/getValue de un numero (entero en la partida)");
    check(near(t.call("Save.getValue", {Value("pos")}).asVec3(), {1.0f, 2.0f, 3.0f}) &&
              saves.values()["pos"].contains("$v"),
          "Save.getValue de un Vec3 ($v)");
    check(t.call("Save.getValue", {Value("yo")}).asEntity() == caja.handle() && saves.values()["yo"].contains("$ent"),
          "Save.getValue de una entidad (por UUID)");
    const Value back = t.call("Save.getValue", {Value("lista")});
    check(back.isArray() && back.size() == 2 && back[1].asString() == "dos", "Save.getValue de una lista");
    check(t.call("Save.getValue", {Value("nada"), Value(5)}).asNumber() == 5.0, "Save.getValue: el valor por defecto");
    check(t.call("Save.hasValue", {Value("oro")}).truthy(), "Save.hasValue");
    t.call("Save.deleteValue", {Value("lista")});
    t.call("Save.setValue", {Value("pos"), Value()});
    check(!t.call("Save.hasValue", {Value("lista")}).truthy() && !t.call("Save.hasValue", {Value("pos")}).truthy(),
          "Save.deleteValue y setValue(nil) los borran");

    // --- Save: guardar y cargar en la misma escena ---
    t.call("Dialogue.setVar", {Value("oro"), Value(33)});
    t.call("Save.onLoaded", {fn(7)});
    check(t.call("Save.save", {Value("slot1"), Value("Antes del jefe")}).truthy(), "Save.save");
    check(rec.message(caja.handle(), "OnSave", "null") && rec.countMessages("OnSave") == 1,
          "OnSave al script de C++ del objeto (y no al que no tiene script)");
    saves.flush();
    check(t.call("Save.exists", {Value("slot1")}).truthy() && !t.call("Save.isWriting").truthy(), "Save.exists / isWriting");
    const Value slots = t.call("Save.list");
    check(slots.size() == 1 && slots[0]["slot"].asString() == "slot1" && slots[0]["label"].asString() == "Antes del jefe" &&
              slots[0]["scene"].asString() == "Nivel1" && slots[0]["size"].asNumber() > 0.0,
          "Save.list");
    check(t.call("Save.info", {Value("slot1")})["label"].asString() == "Antes del jefe" &&
              t.call("Save.info", {Value("nada")}).isNil(),
          "Save.info (nil si no existe)");

    caja.setLocalPosition({9.0f, 9.0f, 9.0f});
    t.call("Save.setValue", {Value("oro"), Value(1)});
    t.call("Dialogue.setVar", {Value("oro"), Value(0)});
    check(t.call("Save.load", {Value("slot1")}).truthy() && near(caja.localPosition(), {9.0f, 9.0f, 9.0f}),
          "Save.load de la misma escena: espera al siguiente frame");
    t.frame();
    check(near(caja.localPosition(), {1.0f, 2.0f, 3.0f}), "Save.load: la posicion vuelve");
    check(t.call("Save.getValue", {Value("oro")}).asNumber() == 120.0, "Save.load: los valores sueltos vuelven");
    check(t.call("Dialogue.getVar", {Value("oro")}).asNumber() == 33.0, "Save.load: las variables de los dialogos vuelven");
    check(rec.last(7) == json::array({"slot1"}), "Save.onLoaded(slot)");
    check(rec.message(caja.handle(), "OnLoad", "null"), "OnLoad al script de C++ del objeto");
    check(!t.call("Save.load", {Value("nada")}).truthy() && t.logged("Save.load(\"nada\")"), "Save.load de una ranura que no existe");

    // Por el puente de los scripts de C++.
    json r = t.bridge({{"fn", "Save.exists"}, {"args", {"slot1"}}});
    check(r["ok"] == true && r["result"] == true, "Save.exists por el puente");
    r = t.bridge({{"fn", "Save.info"}, {"args", {"slot1"}}});
    check(r["ok"] == true && r["result"]["slot"] == "slot1", "Save.info por el puente (objeto)");

    t.call("Save.setAutosave", {Value(30), Value("auto")});
    check(saves.autosaveInterval() == 30.0f, "Save.setAutosave");
    t.call("Save.setCompression", {Value(true)});
    check(saves.compression(), "Save.setCompression");
    t.call("Save.setCompression", {Value(false)});
    t.call("Save.setAutosave", {Value(0)});
    check(t.call("Save.playtime").asNumber() > 0.0, "Save.playtime");
    check(t.call("Save.folder").asString().find("saves") != std::string::npos, "Save.folder");

    // --- Save: cargar una partida de otra escena ---
    check(t.call("Save.save", {Value("otra")}).truthy(), "Save.save (para cargar desde otra escena)");
    saves.flush();
    t.scripts.stop();
    t.scripts.setSceneName("Nivel2");
    t.scripts.start(t.world);
    caja.setLocalPosition({5.0f, 5.0f, 5.0f});
    check(t.call("Save.load", {Value("otra")}).truthy() && saves.pending_load.has_value(), "Save.load de otra escena");
    const std::filesystem::path request = t.scripts.takeSceneRequest();
    check(request.filename() == "Nivel1.crscene", "Save.load pide cambiar a la escena de la partida");
    t.scripts.stop();
    t.scripts.setSceneName("Nivel1");
    t.scripts.start(t.world);
    check(near(caja.localPosition(), {1.0f, 2.0f, 3.0f}) && !saves.pending_load, "la partida se aplica al empezar la escena");
    t.call("Save.onLoaded", {fn(8)});
    t.frame();
    check(rec.last(8) == json::array({"otra"}) && rec.count(7) == 1, "Save.onLoaded tras cambiar de escena (los de antes se olvidan)");

    check(t.call("Save.delete", {Value("slot1")}).truthy() && !t.call("Save.exists", {Value("slot1")}).truthy(), "Save.delete");
    check(t.call("Save.remove", {Value("otra")}).truthy(), "Save.remove");

    // --- Text ---
    std::printf("Text\n");
    gameplay::Localization& loc = gameplay::localization();
    loc.addLanguage("es", "Espanol");
    loc.addLanguage("en", "English");
    loc.setLanguage("es");
    loc.setText("saludo", "es", "Hola {0}, soy {jugador}");
    loc.setText("saludo", "en", "Hello {0}, I am {jugador}");
    loc.setText("monedas#one", "es", "{n} moneda");
    loc.setText("monedas#other", "es", "{n} monedas");
    check(t.call("Text.get", {Value("saludo"), Value("Ana"), named("jugador", Value("Luis"))}).asString() == "Hola Ana, soy Luis",
          "Text.get con argumentos y con nombre");
    check(t.call("Text.plural", {Value("monedas"), Value(1)}).asString() == "1 moneda" &&
              t.call("Text.plural", {Value("monedas"), Value(3)}).asString() == "3 monedas",
          "Text.plural");
    check(t.call("Text.format", {Value("A {0} B {x}"), Value(5), named("x", Value("y"))}).asString() == "A 5 B y", "Text.format");
    check(t.call("Text.has", {Value("saludo")}).truthy() && !t.call("Text.has", {Value("nada")}).truthy(), "Text.has");
    check(t.call("Text.language").asString() == "es", "Text.language");
    const Value languages = t.call("Text.languages");
    bool has_en = false;
    for (const Value& l : languages.items()) has_en = has_en || (l["code"].asString() == "en" && l["name"].asString() == "English");
    check(has_en, "Text.languages ({code, name})");
    check(t.call("Text.systemLanguage").isString(), "Text.systemLanguage");
    const Value id = t.call("Text.onLanguageChanged", {fn(9)});
    check(id.isNumber() && t.call("Text.setLanguage", {Value("en")}).truthy() && rec.last(9) == json::array({"en"}),
          "Text.onLanguageChanged avisa al cambiar de idioma");
    check(t.call("Text.get", {Value("saludo"), Value("Ana"), named("jugador", Value("Luis"))}).asString() == "Hello Ana, I am Luis",
          "Text.get en el idioma nuevo");
    check(!t.call("Text.setLanguage", {Value("xx")}).truthy() && t.logged("no tiene el idioma \"xx\""),
          "Text.setLanguage de un idioma que no existe");
    t.call("Text.removeListener", {id});
    t.call("Text.setLanguage", {Value("es")});
    check(rec.count(9) == 1, "Text.removeListener");
    t.call("Text.onLanguageChanged", {fn(10)});
    t.scripts.stop();
    loc.setLanguage("en");
    check(rec.count(10) == 0, "los avisos del idioma se quitan al parar");
    loc.setLanguage("es");
    t.scripts.start(t.world);

    // --- Dialogue ---
    std::printf("Dialogue\n");
    check(gameplay::activeDialogue() == &t.scripts.dialogueSystem(), "el dialogo del juego queda activo en Play");
    t.call("Dialogue.setAutoAudio", {Value(false)});
    t.call("Dialogue.setVar", {Value("oro"), Value(10)});
    t.call("Dialogue.onStart", {fn(11)});
    t.call("Dialogue.onLine", {fn(12)});
    t.call("Dialogue.onChoices", {fn(13)});
    t.call("Dialogue.onEvent", {fn(14)});
    t.call("Dialogue.onEnd", {fn(15)});
    check(t.call("Dialogue.start", {Value("Mercader")}).truthy() && t.call("Dialogue.isActive").truthy() &&
              t.call("Dialogue.name").asString() == "Mercader",
          "Dialogue.start por nombre (busca el .crdialog en Assets)");
    const Value line = t.call("Dialogue.currentLine");
    check(line["speaker"].asString() == "Mercader" && line["node"].isNumber() && line["autoAdvance"].isNumber(),
          "Dialogue.currentLine");
    t.frame();
    check(rec.last(11) == json::array({"Mercader"}), "Dialogue.onStart(nombre)");
    const json first_line = rec.last(12);
    check(first_line.is_array() && first_line[0]["speaker"] == "Mercader" &&
              first_line[0]["text"].get<std::string>().find("Bienvenido") != std::string::npos,
          "Dialogue.onLine(linea)");
    t.call("Dialogue.next");
    t.frame();
    const json choices = rec.last(13);
    check(choices.is_array() && choices[0].size() == 2 && choices[0][0]["index"] == 1 && choices[0][1]["index"] == 2 &&
              choices[0][0]["enabled"] == true,
          "Dialogue.onChoices (indices desde 1)");
    check(t.call("Dialogue.isWaitingChoice").truthy() && t.call("Dialogue.choices").size() == 2, "Dialogue.choices");
    check(t.call("Dialogue.choose", {Value(1)}).truthy(), "Dialogue.choose(1)");
    t.frame();
    check(rec.last(14) == json::array({"comprar_pocion", "pocion"}), "Dialogue.onEvent(nombre, argumento)");
    check(rec.message(mercader.handle(), "OnComprar", "\"pocion\""), "el evento llama al metodo del script del objeto (mensaje)");
    check(t.call("Dialogue.getVar", {Value("oro")}).asNumber() == 5.0, "Dialogue.getVar (el nodo Variable resto 5)");
    check(rec.last(12)[0]["text"].get<std::string>().find('5') != std::string::npos, "la linea pone la variable ({$oro})");
    t.call("Dialogue.advance");
    t.frame();
    check(rec.last(15) == json::array({"Mercader"}) && !t.call("Dialogue.isActive").truthy() &&
              t.call("Dialogue.name").asString().empty() && t.call("Dialogue.currentLine").isNil(),
          "Dialogue.onEnd(nombre)");
    check(!t.call("Dialogue.start", {Value("NoExiste")}).truthy() && t.logged("Dialogue.start(\"NoExiste\")"),
          "Dialogue.start de un dialogo que no existe");
    t.call("Dialogue.setVar", {Value("nombre"), Value("Ana")});
    check(t.call("Dialogue.getVar", {Value("nombre")}).asString() == "Ana", "Dialogue.setVar / getVar de un texto");
    t.call("Dialogue.start", {Value("Dialogos/Mercader")});
    t.call("Dialogue.stop");
    check(!t.call("Dialogue.isActive").truthy(), "Dialogue.start por ruta y Dialogue.stop");

    // Al parar: los callbacks se olvidan y el dialogo deja de estar activo.
    t.scripts.stop();
    check(gameplay::activeDialogue() == nullptr, "al parar no queda un dialogo activo");
    t.scripts.start(t.world);
    const int before = rec.count(11);
    t.call("Dialogue.start", {Value("Mercader")});
    t.frame();
    check(rec.count(11) == before, "los callbacks de los dialogos se olvidan al parar");

    t.scripts.stop();
    std::filesystem::remove_all(root);
    return finish();
}
