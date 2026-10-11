// Pruebas de SteamApi.cpp (Steam.*) sin cliente de Steam: todo responde "no
// disponible" (false, "", nil, listas vacias) y los callbacks llegan con
// ok = false. Lo unico que se puede simular es una DLL que no carga.

#include "ApiTest.h"

#include "CramionCore/platform/Steam.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

using namespace cramion;
using namespace cramion::apitest;

namespace {

Value fn(std::uint64_t id) { return Value::function(id); }

Value object(std::initializer_list<std::pair<const char*, Value>> fields) {
    Value o = Value::object();
    for (const auto& [k, v] : fields) o.set(k, v);
    return o;
}

}  // namespace

int main() {
    std::printf("SteamApi\n");
    ApiFixture t;
    std::map<std::uint64_t, json> calls;  // callback -> sus argumentos
    t.scripts.setBridgeCallbackSink([&](std::uint64_t id, const std::string& args) { calls[id] = json::parse(args); });

    // --- Todos los nombres de la API de Lua ---
    const char* names[] = {"available", "error", "appId", "userId", "userName", "friendName", "language", "isDlcInstalled",
                           "isSteamDeck", "unlockAchievement", "clearAchievement", "isAchievementUnlocked",
                           "achievementProgress", "setStatInt", "setStatFloat", "getStatInt", "getStatFloat", "storeStats",
                           "uploadScore", "downloadScores", "setRichPresence", "clearRichPresence", "openOverlay",
                           "openOverlayUrl", "openStore", "overlayEnabled", "overlayActive", "cloudEnabled", "cloudWrite",
                           "cloudRead", "cloudExists", "cloudDelete", "cloudFiles", "workshopItems", "workshopUpload",
                           "workshopProgress", "createLobby", "joinLobby", "leaveLobby", "findLobbies", "setLobbyData",
                           "getLobbyData", "lobbyMembers", "lobbyOwner", "inviteToLobby", "onLobbyJoinRequested", "onOverlay"};
    int registered = 0;
    for (const char* n : names) {
        const scripting::api::Entry* e = t.api().find(std::string("Steam.") + n);
        registered += e != nullptr && !e->doc.description.empty();
    }
    check(registered == 47, "las 47 funciones de Steam estan registradas y documentadas");

    // --- Sin Steam: valores de "no disponible" ---
    check(t.call("Steam.available").isBool() && !t.call("Steam.available").truthy(), "available = false");
    check(t.call("Steam.error").isString(), "error es un texto");
    check(t.call("Steam.appId").asNumber() == 0.0, "appId = 0");
    check(t.call("Steam.userId").asString().empty() && t.call("Steam.userName").asString().empty(), "userId y userName vacios");
    check(t.call("Steam.friendName", {Value("76561198000000000")}).asString().empty(), "friendName vacio");
    check(t.call("Steam.language").asString().empty(), "language vacio");
    check(!t.call("Steam.isDlcInstalled", {Value(480)}).truthy() && !t.call("Steam.isSteamDeck").truthy(), "sin DLC ni Steam Deck");
    check(!t.call("Steam.unlockAchievement", {Value("PRIMERA_SANGRE")}).truthy() &&
              !t.call("Steam.clearAchievement", {Value("PRIMERA_SANGRE")}).truthy() &&
              !t.call("Steam.isAchievementUnlocked", {Value("PRIMERA_SANGRE")}).truthy(),
          "logros: false");
    check(t.call("Steam.achievementProgress", {Value("COLECCIONISTA"), Value(5), Value(10)}).isBool(), "achievementProgress da un bool");
    check(!t.call("Steam.setStatInt", {Value("partidas"), Value(3)}).truthy() &&
              !t.call("Steam.setStatFloat", {Value("km"), Value(4.5)}).truthy() && !t.call("Steam.storeStats").truthy(),
          "estadisticas: false");
    check(t.call("Steam.getStatInt", {Value("partidas")}).isNil() && t.call("Steam.getStatFloat", {Value("km")}).isNil(),
          "getStatInt / getStatFloat: nil");
    check(!t.call("Steam.setRichPresence", {Value("steam_display"), Value("#Jugando")}).truthy(), "setRichPresence: false");
    check(t.call("Steam.clearRichPresence").isNil() && t.call("Steam.openOverlay").isNil() &&
              t.call("Steam.openOverlayUrl", {Value("https://example.com")}).isNil() && t.call("Steam.openStore").isNil() &&
              t.call("Steam.inviteToLobby", {Value("1")}).isNil() && t.call("Steam.leaveLobby", {Value("1")}).isNil(),
          "las que no devuelven nada no fallan");
    check(!t.call("Steam.overlayEnabled").truthy() && !t.call("Steam.overlayActive").truthy(), "overlay: false");
    check(!t.call("Steam.cloudEnabled").truthy() && !t.call("Steam.cloudWrite", {Value("partida.json"), Value("{}")}).truthy() &&
              !t.call("Steam.cloudExists", {Value("partida.json")}).truthy() &&
              !t.call("Steam.cloudDelete", {Value("partida.json")}).truthy(),
          "Steam Cloud: false");
    check(t.call("Steam.cloudRead", {Value("partida.json")}).isNil(), "cloudRead: nil");
    check(t.call("Steam.cloudFiles").isArray() && t.call("Steam.cloudFiles").size() == 0, "cloudFiles: lista vacia");
    check(t.call("Steam.workshopItems").isArray() && t.call("Steam.workshopItems").size() == 0, "workshopItems: lista vacia");
    check(t.call("Steam.workshopProgress").asNumber() == -1.0, "workshopProgress = -1");
    check(!t.call("Steam.setLobbyData", {Value("1"), Value("ip"), Value("1.2.3.4:7777")}).truthy() &&
              t.call("Steam.getLobbyData", {Value("1"), Value("ip")}).asString().empty() &&
              t.call("Steam.lobbyOwner", {Value("1")}).asString().empty(),
          "datos de la sala: false / vacio");
    check(t.call("Steam.lobbyMembers", {Value("1")}).isArray() && t.call("Steam.lobbyMembers", {Value("1")}).size() == 0,
          "lobbyMembers: lista vacia");

    // --- Callbacks: llegan con ok = false ---
    t.call("Steam.uploadScore", {Value("Puntos"), Value(1200), fn(1)});
    check(calls.count(1) && calls[1] == json::parse("[false,0,false]"), "uploadScore -> (false, 0, false)");
    t.call("Steam.uploadScore", {Value("Puntos"), Value(1200), Value(false), fn(2)});
    check(calls.count(2) && calls[2][0] == false, "uploadScore con keep_best antes del callback");
    t.call("Steam.downloadScores", {Value("Puntos"), fn(3)});  // como en Lua
    check(calls.count(3) && calls[3] == json::parse("[false,[]]"), "downloadScores(marcador, callback) -> (false, {})");
    t.call("Steam.downloadScores", {Value("Puntos"), Value("around"), Value(-3), Value(3), fn(4)});  // como en la documentacion
    check(calls.count(4) && calls[4] == json::parse("[false,[]]"), "downloadScores(marcador, modo, desde, hasta, callback)");
    t.call("Steam.createLobby", {Value("friends"), Value(8), fn(5)});
    check(calls.count(5) && calls[5] == json::parse("[false,\"\"]"), "createLobby -> (false, \"\")");
    t.call("Steam.createLobby", {fn(6)});
    check(calls.count(6) && calls[6][0] == false, "createLobby(callback) con los valores por defecto");
    t.call("Steam.joinLobby", {Value("109775241000000000"), fn(7)});
    check(calls.count(7) && calls[7] == json::parse("[false,\"109775241000000000\"]"), "joinLobby -> (false, sala)");
    t.call("Steam.findLobbies", {object({{"modo", Value("coop")}, {"nivel", Value(3)}}), Value(20), fn(8)});
    check(calls.count(8) && calls[8] == json::parse("[false,[]]"), "findLobbies(filtros, max, callback) -> (false, {})");
    t.call("Steam.findLobbies", {fn(9)});
    check(calls.count(9) && calls[9] == json::parse("[false,[]]"), "findLobbies(callback)");
    const Value tags(Value::Array{Value("mapa"), Value("coop")});
    t.call("Steam.workshopUpload", {object({{"title", Value("Mi mapa")}, {"folder", Value("Mods/Mapa")}, {"tags", tags},
                                            {"visibility", Value("private")}}),
                                    fn(10)});
    check(calls.count(10) && calls[10].size() == 4 && calls[10][0] == false && calls[10][1] == "" && calls[10][2] == false &&
              calls[10][3].is_string(),
          "workshopUpload -> (false, \"\", false, error)");
    // Sin callback no pasa nada.
    t.call("Steam.uploadScore", {Value("Puntos"), Value(5)});
    check(true, "uploadScore sin callback no falla");

    // Por el puente de los scripts de C++ (el mismo JSON que el SDK).
    const json r = t.bridge({{"fn", "Steam.available"}, {"args", json::array()}});
    check(r.value("ok", false) && r["result"] == false, "puente: Steam.available -> false");
    const json r2 = t.bridge({{"fn", "Steam.createLobby"}, {"args", {"public", 4, {{"$f", 11}}}}});
    check(r2.value("ok", false) && calls.count(11) && calls[11][0] == false, "puente: createLobby con callback");
    const json r3 = t.bridge({{"fn", "Steam.getStatInt"}, {"args", {"partidas"}}});
    check(r3.value("ok", false) && r3["result"].is_null(), "puente: getStatInt -> null");

    // --- Avisos: se ponen y se quitan sin fallar ---
    check(t.call("Steam.onOverlay", {fn(12)}).isNil() && t.call("Steam.onLobbyJoinRequested", {fn(13)}).isNil(),
          "onOverlay / onLobbyJoinRequested con callback");
    check(t.call("Steam.onOverlay", {Value()}).isNil() && t.call("Steam.onLobbyJoinRequested", {Value()}).isNil(),
          "onOverlay / onLobbyJoinRequested(nil) los quitan");
    t.call("Steam.onOverlay", {fn(14)});
    t.scripts.stop();
    t.scripts.start(t.world);
    check(t.call("Steam.available").isBool(), "parar y volver a empezar con avisos puestos");

    // --- Una DLL que no carga: available sigue en false y error dice por que ---
    platform::Steam::instance().init(0, std::filesystem::temp_directory_path() / "no_existe_steam_api64.dll");
    check(!t.call("Steam.available").truthy(), "con una DLL que no existe: available = false");
    std::printf("  (Steam.error: %s)\n", t.call("Steam.error").asString().c_str());
    check(!t.call("Steam.error").asString().empty(), "y Steam.error dice por que");
    t.call("Steam.uploadScore", {Value("Puntos"), Value(1), fn(15)});
    check(calls.count(15) && calls[15][0] == false, "los callbacks siguen llegando con ok = false");

    return finish();
}
