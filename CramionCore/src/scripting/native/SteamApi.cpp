// Steam: logros, estadisticas, marcadores, presencia, overlay, Steam Cloud,
// Workshop y salas (platform/Steam.h). Sin Steam (sin la DLL, cerrado...)
// todo responde "no disponible": false, "", nil, listas vacias y los
// callbacks llegan enseguida con ok = false.
//
// Los callbacks (marcadores, salas, Workshop) los guarda Steam hasta que llega
// su resultado; si el juego se paro entre medias, ese resultado ya no llama a
// nada (cada Play es una "sesion" nueva).

#include "Modules.h"

#include "CramionCore/platform/Steam.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cramion::scripting::native {
namespace {

struct SteamState {
    Runtime* rt = nullptr;
    std::uint64_t session = 1;      // cambia al parar el juego
    api::Value on_join;             // Steam.onLobbyJoinRequested
    api::Value on_overlay;          // Steam.onOverlay
};

// Llama al callback `fn` si el juego en que se pidio sigue en marcha.
struct Fire {
    std::weak_ptr<SteamState> state;
    std::uint64_t session = 0;
    api::Value fn;
    void operator()(const api::Value::Array& args) const {
        const std::shared_ptr<SteamState> s = state.lock();
        if (!s || s->session != session || !fn.isFunction()) return;
        s->rt->native.invoke(fn, args);
    }
};

Fire keep(const std::shared_ptr<SteamState>& state, const api::Value& fn) { return Fire{state, state->session, fn}; }

// El primer argumento (desde `from`) que es una funcion (nil si no hay).
const api::Value& firstFunction(const api::Call& c, std::size_t from) {
    for (std::size_t i = from; i < c.count(); ++i) {
        if (c.arg(i).isFunction()) return c.arg(i);
    }
    return api::Value::nil();
}

// Los argumentos (desde `from`) que no son funciones, en orden: asi el
// callback puede ir donde lo ponia Lua o al final, como en la documentacion.
std::vector<api::Value> rest(const api::Call& c, std::size_t from) {
    std::vector<api::Value> out;
    for (std::size_t i = from; i < c.count(); ++i) {
        if (!c.arg(i).isFunction()) out.push_back(c.arg(i));
    }
    return out;
}

// Un campo de texto de un objeto (vacio si falta).
std::string field(const api::Value& object, const char* key, const std::string& fallback = {}) {
    const api::Value& v = object[key];
    return v.isNil() ? fallback : v.asString();
}

int lobbyType(const std::string& t) { return t == "private" ? 0 : (t == "friends" ? 1 : (t == "invisible" ? 3 : 2)); }

}  // namespace

void registerSteamApi(Runtime& rt) {
    auto state = std::make_shared<SteamState>();
    state->rt = &rt;
    platform::Steam& steam = platform::Steam::instance();

    // Al parar: los resultados que lleguen tarde no llaman a nada y se
    // olvidan los avisos (overlay, invitaciones).
    rt.onStop([state, &steam](bool) {
        ++state->session;
        state->on_join = {};
        state->on_overlay = {};
        steam.setLobbyJoinRequestedListener(nullptr);
        steam.setOverlayListener(nullptr);
    });

    // --- Usuario ---
    rt.native.function("Steam.available", [&steam](api::Call&) { return api::Value(steam.available()); },
                       {"", "Steam esta abierto y la DLL cargada", "bool"});
    rt.native.function("Steam.error", [&steam](api::Call&) { return api::Value(steam.error()); },
                       {"", "por que no lo esta", "texto"});
    rt.native.function("Steam.appId", [&steam](api::Call&) { return api::Value(static_cast<double>(steam.appId())); },
                       {"", "AppID", "numero"});
    rt.native.function("Steam.userId", [&steam](api::Call&) { return api::Value(steam.userId()); },
                       {"", "SteamID del jugador", "texto"});
    rt.native.function("Steam.userName", [&steam](api::Call&) { return api::Value(steam.userName()); },
                       {"", "nombre del jugador", "texto"});
    rt.native.function("Steam.friendName", [&steam](api::Call& c) { return api::Value(steam.friendName(c.string(0))); },
                       {"id", "nombre de un amigo", "texto"});
    rt.native.function("Steam.language", [&steam](api::Call&) { return api::Value(steam.language()); },
                       {"", "idioma de Steam", "texto"});
    rt.native.function("Steam.isDlcInstalled",
                       [&steam](api::Call& c) { return api::Value(steam.isDlcInstalled(static_cast<std::uint32_t>(c.number(0)))); },
                       {"appId", "tiene el DLC?", "bool"});
    rt.native.function("Steam.isSteamDeck", [&steam](api::Call&) { return api::Value(steam.onSteamDeck()); },
                       {"", "corre en una Steam Deck?", "bool"});

    // --- Logros y estadisticas ---
    rt.native.function("Steam.unlockAchievement", [&steam](api::Call& c) { return api::Value(steam.unlockAchievement(c.string(0))); },
                       {"\"PRIMERA_SANGRE\"", "desbloquea un logro", "bool"});
    rt.native.function("Steam.clearAchievement", [&steam](api::Call& c) { return api::Value(steam.clearAchievement(c.string(0))); },
                       {"\"PRIMERA_SANGRE\"", "lo vuelve a bloquear (pruebas)", "bool"});
    rt.native.function("Steam.isAchievementUnlocked",
                       [&steam](api::Call& c) { return api::Value(steam.achievementUnlocked(c.string(0))); },
                       {"\"PRIMERA_SANGRE\"", "esta desbloqueado?", "bool"});
    rt.native.function("Steam.achievementProgress",
                       [&steam](api::Call& c) {
                           return api::Value(steam.indicateAchievementProgress(
                               c.string(0), static_cast<std::uint32_t>(std::max(0.0, c.number(1))),
                               static_cast<std::uint32_t>(std::max(1.0, c.number(2)))));
                       },
                       {"\"COLECCIONISTA\", 5, 10", "muestra el progreso", "bool"});
    rt.native.function("Steam.setStatInt",
                       [&steam](api::Call& c) { return api::Value(steam.setStatInt(c.string(0), static_cast<int>(c.number(1)))); },
                       {"\"partidas\", 3", "estadistica entera", "bool"});
    rt.native.function("Steam.setStatFloat",
                       [&steam](api::Call& c) { return api::Value(steam.setStatFloat(c.string(0), static_cast<float>(c.number(1)))); },
                       {"\"km\", 4.5", "estadistica decimal", "bool"});
    rt.native.function("Steam.getStatInt",
                       [&steam](api::Call& c) {
                           const std::optional<int> v = steam.statInt(c.string(0));
                           return v ? api::Value(*v) : api::Value{};
                       },
                       {"\"partidas\"", "lee una estadistica (nil si no hay)", "numero"});
    rt.native.function("Steam.getStatFloat",
                       [&steam](api::Call& c) {
                           const std::optional<float> v = steam.statFloat(c.string(0));
                           return v ? api::Value(static_cast<double>(*v)) : api::Value{};
                       },
                       {"\"km\"", "lee una estadistica (nil si no hay)", "numero"});
    rt.native.function("Steam.storeStats", [&steam](api::Call&) { return api::Value(steam.storeStats()); },
                       {"", "envia logros y estadisticas a Steam", "bool"});

    // --- Marcadores ---
    // uploadScore(marcador, puntos, callback(ok, puesto, cambio), solo_si_mejora = true)
    rt.native.function("Steam.uploadScore",
                       [state, &steam](api::Call& c) {
                           const Fire fire = keep(state, firstFunction(c, 2));
                           bool keep_best = true;
                           for (const api::Value& v : rest(c, 2)) {
                               if (v.isBool()) keep_best = v.truthy();
                           }
                           steam.uploadScore(c.string(0), static_cast<int>(c.number(1)), keep_best, {},
                                             [fire](bool ok, int rank, bool changed) { fire({ok, rank, changed}); });
                           return api::Value{};
                       },
                       {"\"Puntos\", 1200, function(ok, puesto) end", "sube una puntuacion al marcador"});
    // downloadScores(marcador, modo = "global", desde, hasta, callback(ok, filas)); el
    // callback tambien puede ir segundo (como en Lua).
    rt.native.function("Steam.downloadScores",
                       [state, &steam](api::Call& c) {
                           const Fire fire = keep(state, firstFunction(c, 1));
                           const std::vector<api::Value> args = rest(c, 1);
                           const auto arg = [&args](std::size_t i) -> const api::Value& {
                               return i < args.size() ? args[i] : api::Value::nil();
                           };
                           const std::string m = arg(0).isNil() ? std::string("global") : arg(0).asString();
                           const int kind = m == "around" || m == "alrededor" ? 1 : (m == "friends" || m == "amigos" ? 2 : 0);
                           const int first = arg(1).isNumber() ? static_cast<int>(arg(1).asNumber()) : (kind == 1 ? -5 : 1);
                           const int last = arg(2).isNumber() ? static_cast<int>(arg(2).asNumber()) : (kind == 1 ? 5 : 10);
                           steam.downloadScores(c.string(0), kind, first, last,
                                                [fire](bool ok, std::vector<platform::SteamLeaderboardEntry> entries) {
                                                    api::Value::Array rows;
                                                    for (const platform::SteamLeaderboardEntry& e : entries) {
                                                        api::Value row = api::Value::object();
                                                        row.set("id", e.user_id);
                                                        row.set("name", e.name);
                                                        row.set("rank", e.rank);
                                                        row.set("score", e.score);
                                                        rows.push_back(std::move(row));
                                                    }
                                                    fire({ok, api::Value(std::move(rows))});
                                                });
                           return api::Value{};
                       },
                       {"\"Puntos\", \"global\", 1, 10, function(ok, filas) end", "lee el marcador"});

    // --- Presencia y overlay ---
    rt.native.function("Steam.setRichPresence",
                       [&steam](api::Call& c) { return api::Value(steam.setRichPresence(c.string(0), c.string(1))); },
                       {"\"steam_display\", \"#Jugando\"", "estado que ven los amigos", "bool"});
    rt.native.function("Steam.clearRichPresence", [&steam](api::Call&) { steam.clearRichPresence(); return api::Value{}; },
                       {"", "lo borra"});
    rt.native.function("Steam.openOverlay", [&steam](api::Call& c) { steam.openOverlay(c.string(0, "friends")); return api::Value{}; },
                       {"\"friends\"", "abre el overlay"});
    rt.native.function("Steam.openOverlayUrl", [&steam](api::Call& c) { steam.openOverlayUrl(c.string(0)); return api::Value{}; },
                       {"\"https://...\"", "web en el overlay"});
    rt.native.function("Steam.openStore",
                       [&steam](api::Call& c) { steam.openStore(static_cast<std::uint32_t>(c.number(0, 0.0))); return api::Value{}; },
                       {"", "la pagina de la tienda"});
    rt.native.function("Steam.overlayEnabled", [&steam](api::Call&) { return api::Value(steam.overlayEnabled()); },
                       {"", "el overlay funciona?", "bool"});
    rt.native.function("Steam.overlayActive", [&steam](api::Call&) { return api::Value(steam.overlayActive()); },
                       {"", "esta abierto ahora?", "bool"});

    // --- Steam Cloud ---
    rt.native.function("Steam.cloudEnabled", [&steam](api::Call&) { return api::Value(steam.cloudEnabled()); },
                       {"", "Steam Cloud activo?", "bool"});
    rt.native.function("Steam.cloudWrite", [&steam](api::Call& c) { return api::Value(steam.cloudWrite(c.string(0), c.string(1))); },
                       {"\"partida.json\", texto", "guarda en la nube", "bool"});
    rt.native.function("Steam.cloudRead",
                       [&steam](api::Call& c) {
                           std::optional<std::string> v = steam.cloudRead(c.string(0));
                           return v ? api::Value(std::move(*v)) : api::Value{};
                       },
                       {"\"partida.json\"", "lee de la nube (o nil)", "texto"});
    rt.native.function("Steam.cloudExists", [&steam](api::Call& c) { return api::Value(steam.cloudExists(c.string(0))); },
                       {"\"partida.json\"", "existe?", "bool"});
    rt.native.function("Steam.cloudDelete", [&steam](api::Call& c) { return api::Value(steam.cloudDelete(c.string(0))); },
                       {"\"partida.json\"", "lo borra", "bool"});
    rt.native.function("Steam.cloudFiles",
                       [&steam](api::Call&) {
                           api::Value::Array out;
                           for (const auto& [name, size] : steam.cloudFiles()) {
                               api::Value row = api::Value::object();
                               row.set("name", name);
                               row.set("size", size);
                               out.push_back(std::move(row));
                           }
                           return api::Value(std::move(out));
                       },
                       {"", "{name, size} de cada archivo", "lista de objetos"});

    // --- Workshop ---
    rt.native.function("Steam.workshopItems",
                       [&steam](api::Call&) {
                           api::Value::Array out;
                           for (const platform::SteamWorkshopItem& item : steam.subscribedItems()) {
                               api::Value row = api::Value::object();
                               row.set("id", item.id);
                               row.set("folder", item.folder);
                               row.set("size", static_cast<double>(item.size));
                               row.set("installed", item.installed);
                               row.set("state", static_cast<double>(item.state));
                               out.push_back(std::move(row));
                           }
                           return api::Value(std::move(out));
                       },
                       {"", "objetos del Workshop suscritos", "lista de objetos"});
    // workshopUpload({id, title, description, folder, preview, changeNote, visibility, tags},
    //                callback(ok, id, acuerdo_legal, error))
    rt.native.function("Steam.workshopUpload",
                       [state, &steam, &rt](api::Call& c) {
                           const Fire fire = keep(state, firstFunction(c, 1));
                           const api::Value& options = c.object(0);
                           platform::SteamWorkshopUpload item;
                           item.id = field(options, "id");
                           item.title = field(options, "title");
                           item.description = field(options, "description");
                           item.folder = field(options, "folder");
                           item.preview = field(options, "preview");
                           item.change_note = field(options, "changeNote");
                           const std::string visibility = field(options, "visibility", "public");
                           item.visibility = visibility == "friends" ? 1 : (visibility == "private" ? 2 : (visibility == "unlisted" ? 3 : 0));
                           // Las rutas de Assets (las del juego) a absolutas.
                           for (std::string* path : {&item.folder, &item.preview}) {
                               if (path->empty() || rt.root.empty()) continue;
                               const std::filesystem::path p = pathFromUtf8(*path);
                               if (!p.is_relative()) continue;
                               const std::u8string full = (rt.root / p).u8string();
                               *path = std::string(full.begin(), full.end());
                           }
                           const api::Value& tags = options["tags"];
                           if (tags.isArray()) {
                               for (const api::Value& t : tags.items()) item.tags.push_back(t.asString());
                           }
                           steam.workshopUpload(item, [fire](bool ok, const std::string& id, bool legal, const std::string& error) {
                               fire({ok, id, legal, error});
                           });
                           return api::Value{};
                       },
                       {"{title, description, folder, preview, tags}, function(ok, id) end", "sube al Workshop"});
    rt.native.function("Steam.workshopProgress", [&steam](api::Call&) { return api::Value(steam.workshopUploadProgress()); },
                       {"", "0..1 de la subida (-1 si no hay)", "numero"});

    // --- Salas (lobbies) ---
    // Con la Network API, el anfitrion guarda su direccion en los datos de la
    // sala y los demas se conectan a ella.
    // createLobby(tipo = "public", max = 4, callback(ok, sala))
    rt.native.function("Steam.createLobby",
                       [state, &steam](api::Call& c) {
                           const Fire fire = keep(state, firstFunction(c, 0));
                           std::string type = "public";
                           int max_members = 4;
                           for (const api::Value& v : rest(c, 0)) {
                               if (v.isString()) type = v.asString();
                               else if (v.isNumber()) max_members = static_cast<int>(v.asNumber());
                           }
                           steam.createLobby(lobbyType(type), max_members,
                                             [fire](bool ok, const std::string& lobby) { fire({ok, lobby}); });
                           return api::Value{};
                       },
                       {"\"public\", 4, function(ok, sala) end", "crea una sala"});
    rt.native.function("Steam.joinLobby",
                       [state, &steam](api::Call& c) {
                           const Fire fire = keep(state, firstFunction(c, 1));
                           steam.joinLobby(c.string(0), [fire](bool ok, const std::string& joined) { fire({ok, joined}); });
                           return api::Value{};
                       },
                       {"sala, function(ok, sala) end", "entra en una sala"});
    rt.native.function("Steam.leaveLobby", [&steam](api::Call& c) { steam.leaveLobby(c.string(0)); return api::Value{}; },
                       {"sala", "sale"});
    // findLobbies(callback) o findLobbies({modo = "carreras"}, [max = 50], callback(ok, salas))
    rt.native.function("Steam.findLobbies",
                       [state, &steam](api::Call& c) {
                           const Fire fire = keep(state, firstFunction(c, 0));
                           std::vector<std::pair<std::string, std::string>> filters;
                           int max_results = 50;
                           for (const api::Value& v : rest(c, 0)) {
                               if (v.isNumber()) max_results = static_cast<int>(v.asNumber());
                               if (!v.isObject()) continue;
                               for (const auto& [key, value] : v.fields()) {
                                   if (value.isString()) filters.emplace_back(key, value.asString());
                                   else if (value.isNumber()) filters.emplace_back(key, std::to_string(static_cast<long long>(value.asNumber())));
                               }
                           }
                           steam.findLobbies(filters, std::max(1, max_results), [fire](bool ok, std::vector<std::string> lobbies) {
                               api::Value::Array out;
                               for (std::string& l : lobbies) out.emplace_back(std::move(l));
                               fire({ok, api::Value(std::move(out))});
                           });
                           return api::Value{};
                       },
                       {"{modo = \"coop\"}, 20, function(ok, salas) end", "busca salas"});
    rt.native.function("Steam.setLobbyData",
                       [&steam](api::Call& c) { return api::Value(steam.setLobbyData(c.string(0), c.string(1), c.string(2))); },
                       {"sala, \"ip\", \"1.2.3.4:7777\"", "dato de la sala", "bool"});
    rt.native.function("Steam.getLobbyData", [&steam](api::Call& c) { return api::Value(steam.lobbyData(c.string(0), c.string(1))); },
                       {"sala, \"ip\"", "lo lee", "texto"});
    rt.native.function("Steam.lobbyMembers",
                       [&steam](api::Call& c) {
                           api::Value::Array out;
                           for (const platform::SteamLobbyMember& m : steam.lobbyMembers(c.string(0))) {
                               api::Value row = api::Value::object();
                               row.set("id", m.id);
                               row.set("name", m.name);
                               out.push_back(std::move(row));
                           }
                           return api::Value(std::move(out));
                       },
                       {"sala", "{id, name} de cada jugador", "lista de objetos"});
    rt.native.function("Steam.lobbyOwner", [&steam](api::Call& c) { return api::Value(steam.lobbyOwner(c.string(0))); },
                       {"sala", "SteamID del dueno", "texto"});
    rt.native.function("Steam.inviteToLobby", [&steam](api::Call& c) { steam.inviteToLobby(c.string(0)); return api::Value{}; },
                       {"sala", "dialogo de invitar del overlay"});

    // Avisos: cada uno con su callback (nil lo quita) hasta que se pare el juego.
    // Steam.onLobbyJoinRequested(function(sala) Steam.joinLobby(sala, ...) end)
    rt.native.function("Steam.onLobbyJoinRequested",
                       [state, &steam](api::Call& c) {
                           state->on_join = c.function(0);
                           if (!state->on_join.isFunction()) {
                               steam.setLobbyJoinRequestedListener(nullptr);
                               return api::Value{};
                           }
                           const std::weak_ptr<SteamState> weak = state;
                           steam.setLobbyJoinRequestedListener([weak](const std::string& lobby) {
                               const std::shared_ptr<SteamState> s = weak.lock();
                               if (s && s->on_join.isFunction()) s->rt->native.invoke(s->on_join, {lobby});
                           });
                           return api::Value{};
                       },
                       {"function(sala) end", "un amigo invito y el jugador acepto"});
    rt.native.function("Steam.onOverlay",
                       [state, &steam](api::Call& c) {
                           state->on_overlay = c.function(0);
                           if (!state->on_overlay.isFunction()) {
                               steam.setOverlayListener(nullptr);
                               return api::Value{};
                           }
                           const std::weak_ptr<SteamState> weak = state;
                           steam.setOverlayListener([weak](bool active) {
                               const std::shared_ptr<SteamState> s = weak.lock();
                               if (s && s->on_overlay.isFunction()) s->rt->native.invoke(s->on_overlay, {active});
                           });
                           return api::Value{};
                       },
                       {"function(abierto) end", "se abrio o cerro el overlay (pausar)"});
}

}  // namespace cramion::scripting::native
