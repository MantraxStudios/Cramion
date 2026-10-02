#include "CramionCore/platform/Steam.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cramion::platform {

namespace {

// --- Tipos de la API plana de Steamworks (steam_api_flat.h) ---
// Los punteros a interfaces se tratan como opacos.
using Iface = void*;
using SteamAPICall = std::uint64_t;
using HSteamPipe = std::int32_t;

// Las estructuras de los resultados: empaquetadas como las de Steam (8 en
// Windows, 4 en Linux/macOS: VALVE_CALLBACK_PACK_LARGE / SMALL).
#if defined(_WIN32)
#pragma pack(push, 8)
#else
#pragma pack(push, 4)
#endif
struct CallbackMsg {
    std::int32_t user;
    int callback;
    std::uint8_t* param;
    int size;
};
struct SteamAPICallCompleted {  // 703
    std::uint64_t call;
    int callback;
    std::uint32_t size;
};
struct LeaderboardFindResult {  // 1104
    std::uint64_t leaderboard;
    std::uint8_t found;
};
struct LeaderboardScoresDownloaded {  // 1105
    std::uint64_t leaderboard;
    std::uint64_t entries;
    int count;
};
struct LeaderboardScoreUploaded {  // 1106
    std::uint8_t success;
    std::uint64_t leaderboard;
    std::int32_t score;
    std::uint8_t changed;
    int rank_new;
    int rank_previous;
};
struct LeaderboardEntry {
    std::uint64_t user;
    std::int32_t rank;
    std::int32_t score;
    std::int32_t details;
    std::uint64_t ugc;
};
struct LobbyCreated {  // 513
    int result;
    std::uint64_t lobby;
};
struct LobbyEnter {  // 504
    std::uint64_t lobby;
    std::uint32_t permissions;
    bool locked;
    std::uint32_t response;  // 1 = bien
};
struct LobbyMatchList {  // 510
    std::uint32_t count;
};
struct GameLobbyJoinRequested {  // 333
    std::uint64_t lobby;
    std::uint64_t friend_id;
};
struct GameOverlayActivated {  // 331
    std::uint8_t active;
};
struct CreateItemResult {  // 3403
    int result;
    std::uint64_t file;
    bool needs_legal;
};
struct SubmitItemUpdateResult {  // 3404
    int result;
    bool needs_legal;
    std::uint64_t file;
};
struct ParamStringArray {
    const char** strings;
    std::int32_t count;
};
#pragma pack(pop)

constexpr int kCallCompleted = 703;
constexpr int kLeaderboardFind = 1104;
constexpr int kLeaderboardDownloaded = 1105;
constexpr int kLeaderboardUploaded = 1106;
constexpr int kLobbyEnter = 504;
constexpr int kLobbyMatchList = 510;
constexpr int kLobbyCreated = 513;
constexpr int kOverlayActivated = 331;
constexpr int kLobbyJoinRequested = 333;
constexpr int kCreateItem = 3403;
constexpr int kSubmitItem = 3404;

std::string idText(std::uint64_t id) { return id == 0 ? std::string() : std::to_string(id); }

std::uint64_t idValue(const std::string& text) {
    std::uint64_t v = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    while (begin < end && (*begin == ' ' || *begin == '"')) ++begin;
    std::from_chars(begin, end, v);
    return v;
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value.c_str());
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// La DLL de Steam: la dada (archivo o carpeta), junto al ejecutable o la del PATH.
void* openLibrary(const std::filesystem::path& hint, std::string& error) {
#if defined(_WIN32)
    const wchar_t* kName = L"steam_api64.dll";
    std::vector<std::filesystem::path> tries;
    if (!hint.empty()) {
        std::error_code ec;
        tries.push_back(std::filesystem::is_directory(hint, ec) ? hint / kName : hint);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    tries.push_back(std::filesystem::path(exe).parent_path() / kName);
    tries.emplace_back(kName);
    for (const std::filesystem::path& p : tries) {
        if (HMODULE m = LoadLibraryW(p.c_str())) return reinterpret_cast<void*>(m);
    }
    error = "no se encontro steam_api64.dll (ponla junto al juego o elige su ruta en Configuraciones de compilacion > Steam)";
    return nullptr;
#else
#if defined(__APPLE__)
    const char* kName = "libsteam_api.dylib";
#else
    const char* kName = "libsteam_api.so";
#endif
    std::vector<std::string> tries;
    if (!hint.empty()) {
        std::error_code ec;
        tries.push_back((std::filesystem::is_directory(hint, ec) ? hint / kName : hint).string());
    }
    tries.emplace_back(std::string("./") + kName);
    tries.emplace_back(kName);
    for (const std::string& p : tries) {
        if (void* m = dlopen(p.c_str(), RTLD_NOW)) return m;
    }
    error = std::string("no se encontro ") + kName;
    return nullptr;
#endif
}

void* symbol(void* library, const char* name) {
    if (library == nullptr) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

// La primera de varias versiones de una interfaz (SteamAPI_SteamUGC_v021,
// _v020... segun el SDK de la DLL).
Iface interfaceOf(void* library, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (auto fn = reinterpret_cast<Iface (*)()>(symbol(library, n))) {
            if (Iface i = fn()) return i;
        }
    }
    return nullptr;
}

}  // namespace

struct Steam::Impl {
    void* library = nullptr;
    bool ready = false;
    std::string error = "Steam no se ha iniciado";
    std::uint32_t app_id = 0;
    HSteamPipe pipe = 0;
    std::uint64_t generation = 1;  // cancelCallbacks
    bool overlay_active = false;

    Iface user = nullptr, user_stats = nullptr, friends = nullptr, utils = nullptr, apps = nullptr, storage = nullptr,
          ugc = nullptr, matchmaking = nullptr;

    // --- Funciones ---
    void (*shutdown)() = nullptr;
    HSteamPipe (*get_pipe)() = nullptr;
    void (*dispatch_init)() = nullptr;
    void (*dispatch_frame)(HSteamPipe) = nullptr;
    bool (*dispatch_next)(HSteamPipe, CallbackMsg*) = nullptr;
    void (*dispatch_free)(HSteamPipe) = nullptr;
    bool (*dispatch_result)(HSteamPipe, SteamAPICall, void*, int, int, bool*) = nullptr;
    void (*run_callbacks)() = nullptr;

    std::uint64_t (*user_id)(Iface) = nullptr;

    bool (*set_achievement)(Iface, const char*) = nullptr;
    bool (*clear_achievement)(Iface, const char*) = nullptr;
    bool (*get_achievement)(Iface, const char*, bool*) = nullptr;
    bool (*indicate_progress)(Iface, const char*, std::uint32_t, std::uint32_t) = nullptr;
    bool (*store_stats)(Iface) = nullptr;
    bool (*request_stats)(Iface) = nullptr;
    bool (*get_stat_int)(Iface, const char*, std::int32_t*) = nullptr;
    bool (*get_stat_float)(Iface, const char*, float*) = nullptr;
    bool (*set_stat_int)(Iface, const char*, std::int32_t) = nullptr;
    bool (*set_stat_float)(Iface, const char*, float) = nullptr;
    SteamAPICall (*find_or_create_board)(Iface, const char*, int, int) = nullptr;
    SteamAPICall (*upload_score)(Iface, std::uint64_t, int, std::int32_t, const std::int32_t*, int) = nullptr;
    SteamAPICall (*download_entries)(Iface, std::uint64_t, int, int, int) = nullptr;
    bool (*downloaded_entry)(Iface, std::uint64_t, int, LeaderboardEntry*, std::int32_t*, int) = nullptr;

    const char* (*persona_name)(Iface) = nullptr;
    const char* (*friend_name)(Iface, std::uint64_t) = nullptr;
    bool (*set_rich_presence)(Iface, const char*, const char*) = nullptr;
    void (*clear_rich_presence)(Iface) = nullptr;
    void (*overlay)(Iface, const char*) = nullptr;
    void (*overlay_url)(Iface, const char*, int) = nullptr;
    void (*overlay_store)(Iface, std::uint32_t, int) = nullptr;
    void (*overlay_invite)(Iface, std::uint64_t) = nullptr;

    std::uint32_t (*utils_app_id)(Iface) = nullptr;
    bool (*overlay_enabled)(Iface) = nullptr;
    bool (*steam_deck)(Iface) = nullptr;

    const char* (*game_language)(Iface) = nullptr;
    bool (*dlc_installed)(Iface, std::uint32_t) = nullptr;

    bool (*file_write)(Iface, const char*, const void*, std::int32_t) = nullptr;
    std::int32_t (*file_read)(Iface, const char*, void*, std::int32_t) = nullptr;
    bool (*file_exists)(Iface, const char*) = nullptr;
    bool (*file_delete)(Iface, const char*) = nullptr;
    std::int32_t (*file_size)(Iface, const char*) = nullptr;
    std::int32_t (*file_count)(Iface) = nullptr;
    const char* (*file_name_and_size)(Iface, int, std::int32_t*) = nullptr;
    bool (*cloud_account)(Iface) = nullptr;
    bool (*cloud_app)(Iface) = nullptr;

    SteamAPICall (*create_item)(Iface, std::uint32_t, int) = nullptr;
    std::uint64_t (*start_update)(Iface, std::uint32_t, std::uint64_t) = nullptr;
    bool (*set_title)(Iface, std::uint64_t, const char*) = nullptr;
    bool (*set_description)(Iface, std::uint64_t, const char*) = nullptr;
    bool (*set_content)(Iface, std::uint64_t, const char*) = nullptr;
    bool (*set_preview)(Iface, std::uint64_t, const char*) = nullptr;
    bool (*set_visibility)(Iface, std::uint64_t, int) = nullptr;
    bool (*set_tags)(Iface, std::uint64_t, const ParamStringArray*, bool) = nullptr;
    SteamAPICall (*submit_update)(Iface, std::uint64_t, const char*) = nullptr;
    int (*update_progress)(Iface, std::uint64_t, std::uint64_t*, std::uint64_t*) = nullptr;
    // El ultimo bool (bIncludeLocallyDisabled) solo existe en los SDK nuevos;
    // en x64 un argumento de mas no molesta a las versiones viejas.
    std::uint32_t (*num_subscribed)(Iface, bool) = nullptr;
    std::uint32_t (*subscribed_items)(Iface, std::uint64_t*, std::uint32_t, bool) = nullptr;
    std::uint32_t (*item_state)(Iface, std::uint64_t) = nullptr;
    bool (*item_install_info)(Iface, std::uint64_t, std::uint64_t*, char*, std::uint32_t, std::uint32_t*) = nullptr;

    SteamAPICall (*create_lobby)(Iface, int, int) = nullptr;
    SteamAPICall (*join_lobby)(Iface, std::uint64_t) = nullptr;
    void (*leave_lobby)(Iface, std::uint64_t) = nullptr;
    SteamAPICall (*request_lobby_list)(Iface) = nullptr;
    void (*lobby_string_filter)(Iface, const char*, const char*, int) = nullptr;
    void (*lobby_count_filter)(Iface, int) = nullptr;
    std::uint64_t (*lobby_by_index)(Iface, int) = nullptr;
    int (*lobby_member_count)(Iface, std::uint64_t) = nullptr;
    std::uint64_t (*lobby_member)(Iface, std::uint64_t, int) = nullptr;
    const char* (*get_lobby_data)(Iface, std::uint64_t, const char*) = nullptr;
    bool (*set_lobby_data)(Iface, std::uint64_t, const char*, const char*) = nullptr;
    std::uint64_t (*lobby_owner)(Iface, std::uint64_t) = nullptr;

    // --- Llamadas asincronas en curso ---
    struct Pending {
        int expected = 0;
        std::function<void(const std::vector<std::uint8_t>& data, bool failed)> done;
    };
    std::unordered_map<SteamAPICall, Pending> pending;
    std::map<std::string, std::uint64_t> boards;  // marcadores ya encontrados
    std::uint64_t upload_handle = 0;              // Workshop: la subida en curso

    std::function<void(const std::string&)> join_listener;
    std::function<void(bool)> overlay_listener;

    template <typename Fn>
    void load(Fn& fn, const char* name) {
        fn = reinterpret_cast<Fn>(symbol(library, name));
    }

    void loadAll() {
        load(shutdown, "SteamAPI_Shutdown");
        load(get_pipe, "SteamAPI_GetHSteamPipe");
        load(dispatch_init, "SteamAPI_ManualDispatch_Init");
        load(dispatch_frame, "SteamAPI_ManualDispatch_RunFrame");
        load(dispatch_next, "SteamAPI_ManualDispatch_GetNextCallback");
        load(dispatch_free, "SteamAPI_ManualDispatch_FreeLastCallback");
        load(dispatch_result, "SteamAPI_ManualDispatch_GetAPICallResult");
        load(run_callbacks, "SteamAPI_RunCallbacks");

        user = interfaceOf(library, {"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021", "SteamAPI_SteamUser_v020"});
        user_stats = interfaceOf(library, {"SteamAPI_SteamUserStats_v013", "SteamAPI_SteamUserStats_v012", "SteamAPI_SteamUserStats_v011"});
        friends = interfaceOf(library, {"SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017"});
        utils = interfaceOf(library, {"SteamAPI_SteamUtils_v010", "SteamAPI_SteamUtils_v009"});
        apps = interfaceOf(library, {"SteamAPI_SteamApps_v008"});
        storage = interfaceOf(library, {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_SteamRemoteStorage_v014"});
        ugc = interfaceOf(library, {"SteamAPI_SteamUGC_v021", "SteamAPI_SteamUGC_v020", "SteamAPI_SteamUGC_v018", "SteamAPI_SteamUGC_v017",
                                    "SteamAPI_SteamUGC_v016", "SteamAPI_SteamUGC_v015", "SteamAPI_SteamUGC_v014"});
        matchmaking = interfaceOf(library, {"SteamAPI_SteamMatchmaking_v009"});

        load(user_id, "SteamAPI_ISteamUser_GetSteamID");

        load(set_achievement, "SteamAPI_ISteamUserStats_SetAchievement");
        load(clear_achievement, "SteamAPI_ISteamUserStats_ClearAchievement");
        load(get_achievement, "SteamAPI_ISteamUserStats_GetAchievement");
        load(indicate_progress, "SteamAPI_ISteamUserStats_IndicateAchievementProgress");
        load(store_stats, "SteamAPI_ISteamUserStats_StoreStats");
        load(request_stats, "SteamAPI_ISteamUserStats_RequestCurrentStats");
        load(get_stat_int, "SteamAPI_ISteamUserStats_GetStatInt32");
        load(get_stat_float, "SteamAPI_ISteamUserStats_GetStatFloat");
        load(set_stat_int, "SteamAPI_ISteamUserStats_SetStatInt32");
        load(set_stat_float, "SteamAPI_ISteamUserStats_SetStatFloat");
        load(find_or_create_board, "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard");
        load(upload_score, "SteamAPI_ISteamUserStats_UploadLeaderboardScore");
        load(download_entries, "SteamAPI_ISteamUserStats_DownloadLeaderboardEntries");
        load(downloaded_entry, "SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry");

        load(persona_name, "SteamAPI_ISteamFriends_GetPersonaName");
        load(friend_name, "SteamAPI_ISteamFriends_GetFriendPersonaName");
        load(set_rich_presence, "SteamAPI_ISteamFriends_SetRichPresence");
        load(clear_rich_presence, "SteamAPI_ISteamFriends_ClearRichPresence");
        load(overlay, "SteamAPI_ISteamFriends_ActivateGameOverlay");
        load(overlay_url, "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage");
        load(overlay_store, "SteamAPI_ISteamFriends_ActivateGameOverlayToStore");
        load(overlay_invite, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");

        load(utils_app_id, "SteamAPI_ISteamUtils_GetAppID");
        load(overlay_enabled, "SteamAPI_ISteamUtils_IsOverlayEnabled");
        load(steam_deck, "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck");

        load(game_language, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
        load(dlc_installed, "SteamAPI_ISteamApps_BIsDlcInstalled");

        load(file_write, "SteamAPI_ISteamRemoteStorage_FileWrite");
        load(file_read, "SteamAPI_ISteamRemoteStorage_FileRead");
        load(file_exists, "SteamAPI_ISteamRemoteStorage_FileExists");
        load(file_delete, "SteamAPI_ISteamRemoteStorage_FileDelete");
        load(file_size, "SteamAPI_ISteamRemoteStorage_GetFileSize");
        load(file_count, "SteamAPI_ISteamRemoteStorage_GetFileCount");
        load(file_name_and_size, "SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");
        load(cloud_account, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
        load(cloud_app, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");

        load(create_item, "SteamAPI_ISteamUGC_CreateItem");
        load(start_update, "SteamAPI_ISteamUGC_StartItemUpdate");
        load(set_title, "SteamAPI_ISteamUGC_SetItemTitle");
        load(set_description, "SteamAPI_ISteamUGC_SetItemDescription");
        load(set_content, "SteamAPI_ISteamUGC_SetItemContent");
        load(set_preview, "SteamAPI_ISteamUGC_SetItemPreview");
        load(set_visibility, "SteamAPI_ISteamUGC_SetItemVisibility");
        load(set_tags, "SteamAPI_ISteamUGC_SetItemTags");
        load(submit_update, "SteamAPI_ISteamUGC_SubmitItemUpdate");
        load(update_progress, "SteamAPI_ISteamUGC_GetItemUpdateProgress");
        load(num_subscribed, "SteamAPI_ISteamUGC_GetNumSubscribedItems");
        load(subscribed_items, "SteamAPI_ISteamUGC_GetSubscribedItems");
        load(item_state, "SteamAPI_ISteamUGC_GetItemState");
        load(item_install_info, "SteamAPI_ISteamUGC_GetItemInstallInfo");

        load(create_lobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
        load(join_lobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
        load(leave_lobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
        load(request_lobby_list, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
        load(lobby_string_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
        load(lobby_count_filter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
        load(lobby_by_index, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
        load(lobby_member_count, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
        load(lobby_member, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
        load(get_lobby_data, "SteamAPI_ISteamMatchmaking_GetLobbyData");
        load(set_lobby_data, "SteamAPI_ISteamMatchmaking_SetLobbyData");
        load(lobby_owner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    }

    // Una llamada asincrona: su resultado (de tipo `expected`) llega en update().
    bool expect(SteamAPICall call, int expected, std::function<void(const std::vector<std::uint8_t>&, bool)> done) {
        if (call == 0) return false;  // k_uAPICallInvalid
        pending[call] = Pending{expected, std::move(done)};
        return true;
    }

    // Los callbacks de los scripts: no si se cancelaron entre medias.
    bool current(std::uint64_t g) const { return g == generation; }

    template <typename T>
    static const T* as(const std::vector<std::uint8_t>& data) {
        return data.size() >= sizeof(T) ? reinterpret_cast<const T*>(data.data()) : nullptr;
    }

    // El marcador por nombre (lo crea si no existe) y luego `then`.
    void withBoard(const std::string& board, std::function<void(std::uint64_t)> then) {
        if (const auto it = boards.find(board); it != boards.end()) {
            then(it->second);
            return;
        }
        if (find_or_create_board == nullptr || user_stats == nullptr) {
            then(0);
            return;
        }
        // Descendente (mas = mejor), numerico.
        const SteamAPICall call = find_or_create_board(user_stats, board.c_str(), 2, 1);
        if (!expect(call, kLeaderboardFind, [this, board, then](const std::vector<std::uint8_t>& data, bool failed) {
                const LeaderboardFindResult* r = as<LeaderboardFindResult>(data);
                const std::uint64_t handle = !failed && r != nullptr && r->found != 0 ? r->leaderboard : 0;
                if (handle != 0) boards[board] = handle;
                then(handle);
            })) {
            then(0);
        }
    }
};

Steam& Steam::instance() {
    static Steam steam;
    return steam;
}

Steam::Steam() : impl_(std::make_unique<Impl>()) {}

Steam::~Steam() {
    // Al cerrar el programa: Steam ve que el juego termino.
    shutdown();
}

bool Steam::init(std::uint32_t app_id, const std::filesystem::path& dll) {
    Impl& d = *impl_;
    if (d.ready) return true;
    if (app_id != 0) {
        // Sin steam_appid.txt: Steam lee el AppID de estas variables.
        setEnv("SteamAppId", std::to_string(app_id));
        setEnv("SteamGameId", std::to_string(app_id));
    }
    if (d.library == nullptr) d.library = openLibrary(dll, d.error);
    if (d.library == nullptr) {
        std::cerr << "[Steam] " << d.error << std::endl;
        return false;
    }
    // SDK 1.58+: SteamAPI_InitFlat (con el motivo del fallo); antes, InitSafe / Init.
    bool ok = false;
    if (auto init_flat = reinterpret_cast<int (*)(char*)>(symbol(d.library, "SteamAPI_InitFlat"))) {
        char message[1024] = {};
        const int result = init_flat(message);
        ok = result == 0;
        if (!ok) d.error = message[0] != 0 ? std::string(message) : "SteamAPI_InitFlat fallo (" + std::to_string(result) + ")";
    } else if (auto init_safe = reinterpret_cast<bool (*)()>(symbol(d.library, "SteamAPI_InitSafe"))) {
        ok = init_safe();
    } else if (auto init_old = reinterpret_cast<bool (*)()>(symbol(d.library, "SteamAPI_Init"))) {
        ok = init_old();
    } else {
        d.error = "la DLL no parece de Steamworks (no tiene SteamAPI_Init)";
    }
    if (!ok) {
        if (d.error == "Steam no se ha iniciado") {
            d.error = "Steam no esta abierto, o la cuenta no tiene el juego (AppID " + std::to_string(app_id) + ")";
        }
        std::cerr << "[Steam] No disponible: " << d.error << std::endl;
        return false;
    }
    d.loadAll();
    if (d.dispatch_init != nullptr && d.get_pipe != nullptr) {
        d.dispatch_init();
        d.pipe = d.get_pipe();
    }
    d.app_id = d.utils != nullptr && d.utils_app_id != nullptr ? d.utils_app_id(d.utils) : app_id;
    // SDK antiguos: hay que pedir las estadisticas antes de usarlas.
    if (d.request_stats != nullptr && d.user_stats != nullptr) d.request_stats(d.user_stats);
    d.ready = true;
    d.error.clear();
    std::cout << "[Steam] Conectado (AppID " << d.app_id << ", " << userName() << ")" << std::endl;
    return true;
}

void Steam::shutdown() {
    Impl& d = *impl_;
    if (d.ready) {
        if (d.store_stats != nullptr && d.user_stats != nullptr) d.store_stats(d.user_stats);
        if (d.shutdown != nullptr) d.shutdown();
    }
    d.ready = false;
    d.pending.clear();
    d.boards.clear();
    d.error = "Steam no se ha iniciado";
    // La DLL se queda cargada (volver a iniciar sin recargarla es lo seguro).
}

bool Steam::restartAppIfNecessary(std::uint32_t app_id, const std::filesystem::path& dll) {
    if (app_id == 0) return false;
    std::string error;
    void* library = openLibrary(dll, error);
    auto fn = reinterpret_cast<bool (*)(std::uint32_t)>(symbol(library, "SteamAPI_RestartAppIfNecessary"));
    // La DLL se queda cargada: init() la reutiliza.
    return fn != nullptr && fn(app_id);
}

void Steam::update() {
    Impl& d = *impl_;
    if (!d.ready) return;
    if (d.dispatch_frame == nullptr || d.dispatch_next == nullptr || d.dispatch_free == nullptr) {
        if (d.run_callbacks != nullptr) d.run_callbacks();
        return;
    }
    d.dispatch_frame(d.pipe);
    // Se recogen primero y se atienden despues (lo que se haga al atenderlos
    // puede llamar a Steam).
    struct Ready {
        Impl::Pending pending;
        std::vector<std::uint8_t> data;
        bool failed = false;
    };
    std::vector<Ready> done;
    std::vector<std::pair<int, std::vector<std::uint8_t>>> events;
    CallbackMsg msg{};
    while (d.dispatch_next(d.pipe, &msg)) {
        if (msg.callback == kCallCompleted && msg.param != nullptr && msg.size >= static_cast<int>(sizeof(SteamAPICallCompleted))) {
            SteamAPICallCompleted completed{};
            std::memcpy(&completed, msg.param, sizeof(completed));
            const auto it = d.pending.find(completed.call);
            if (it != d.pending.end()) {
                Ready r;
                r.data.resize(std::max<std::uint32_t>(completed.size, 8u));
                bool failed = false;
                const bool got = d.dispatch_result != nullptr &&
                                 d.dispatch_result(d.pipe, completed.call, r.data.data(), static_cast<int>(completed.size),
                                                   it->second.expected, &failed);
                r.failed = !got || failed;
                r.pending = std::move(it->second);
                d.pending.erase(it);
                done.push_back(std::move(r));
            }
        } else if ((msg.callback == kOverlayActivated || msg.callback == kLobbyJoinRequested) && msg.param != nullptr && msg.size > 0) {
            events.emplace_back(msg.callback, std::vector<std::uint8_t>(msg.param, msg.param + msg.size));
        }
        d.dispatch_free(d.pipe);
    }
    for (Ready& r : done) {
        if (r.pending.done) r.pending.done(r.data, r.failed);
    }
    for (const auto& [kind, data] : events) {
        if (kind == kOverlayActivated) {
            if (const GameOverlayActivated* o = Impl::as<GameOverlayActivated>(data)) {
                d.overlay_active = o->active != 0;
                if (d.overlay_listener) d.overlay_listener(d.overlay_active);
            }
        } else if (const GameLobbyJoinRequested* j = Impl::as<GameLobbyJoinRequested>(data)) {
            if (d.join_listener) d.join_listener(idText(j->lobby));
        }
    }
}

bool Steam::available() const { return impl_->ready; }
const std::string& Steam::error() const { return impl_->error; }
std::uint32_t Steam::appId() const { return impl_->app_id; }

void Steam::cancelCallbacks() {
    Impl& d = *impl_;
    ++d.generation;
    d.join_listener = nullptr;
    d.overlay_listener = nullptr;
}

// --- Usuario ---
std::string Steam::userId() const {
    const Impl& d = *impl_;
    return d.ready && d.user != nullptr && d.user_id != nullptr ? idText(d.user_id(d.user)) : std::string();
}
std::string Steam::userName() const {
    const Impl& d = *impl_;
    const char* n = d.ready && d.friends != nullptr && d.persona_name != nullptr ? d.persona_name(d.friends) : nullptr;
    return n != nullptr ? n : "";
}
std::string Steam::friendName(const std::string& id) const {
    const Impl& d = *impl_;
    const char* n = d.ready && d.friends != nullptr && d.friend_name != nullptr ? d.friend_name(d.friends, idValue(id)) : nullptr;
    return n != nullptr ? n : "";
}
std::string Steam::language() const {
    const Impl& d = *impl_;
    const char* l = d.ready && d.apps != nullptr && d.game_language != nullptr ? d.game_language(d.apps) : nullptr;
    return l != nullptr ? l : "";
}
bool Steam::isDlcInstalled(std::uint32_t app) const {
    const Impl& d = *impl_;
    return d.ready && d.apps != nullptr && d.dlc_installed != nullptr && d.dlc_installed(d.apps, app);
}
bool Steam::onSteamDeck() const {
    const Impl& d = *impl_;
    return d.ready && d.utils != nullptr && d.steam_deck != nullptr && d.steam_deck(d.utils);
}

// --- Logros y estadisticas ---
bool Steam::unlockAchievement(const std::string& name) {
    Impl& d = *impl_;
    if (!d.ready || d.user_stats == nullptr || d.set_achievement == nullptr) return false;
    const bool ok = d.set_achievement(d.user_stats, name.c_str());
    if (ok && d.store_stats != nullptr) d.store_stats(d.user_stats);  // el aviso sale ya
    return ok;
}
bool Steam::clearAchievement(const std::string& name) {
    Impl& d = *impl_;
    if (!d.ready || d.user_stats == nullptr || d.clear_achievement == nullptr) return false;
    const bool ok = d.clear_achievement(d.user_stats, name.c_str());
    if (ok && d.store_stats != nullptr) d.store_stats(d.user_stats);
    return ok;
}
bool Steam::achievementUnlocked(const std::string& name) const {
    const Impl& d = *impl_;
    bool achieved = false;
    return d.ready && d.user_stats != nullptr && d.get_achievement != nullptr && d.get_achievement(d.user_stats, name.c_str(), &achieved) &&
           achieved;
}
bool Steam::indicateAchievementProgress(const std::string& name, std::uint32_t current, std::uint32_t max) {
    Impl& d = *impl_;
    return d.ready && d.user_stats != nullptr && d.indicate_progress != nullptr && d.indicate_progress(d.user_stats, name.c_str(), current, max);
}
bool Steam::setStatInt(const std::string& name, int value) {
    Impl& d = *impl_;
    return d.ready && d.user_stats != nullptr && d.set_stat_int != nullptr && d.set_stat_int(d.user_stats, name.c_str(), value);
}
bool Steam::setStatFloat(const std::string& name, float value) {
    Impl& d = *impl_;
    return d.ready && d.user_stats != nullptr && d.set_stat_float != nullptr && d.set_stat_float(d.user_stats, name.c_str(), value);
}
std::optional<int> Steam::statInt(const std::string& name) const {
    const Impl& d = *impl_;
    std::int32_t v = 0;
    if (d.ready && d.user_stats != nullptr && d.get_stat_int != nullptr && d.get_stat_int(d.user_stats, name.c_str(), &v)) return v;
    return std::nullopt;
}
std::optional<float> Steam::statFloat(const std::string& name) const {
    const Impl& d = *impl_;
    float v = 0.0f;
    if (d.ready && d.user_stats != nullptr && d.get_stat_float != nullptr && d.get_stat_float(d.user_stats, name.c_str(), &v)) return v;
    return std::nullopt;
}
bool Steam::storeStats() {
    Impl& d = *impl_;
    return d.ready && d.user_stats != nullptr && d.store_stats != nullptr && d.store_stats(d.user_stats);
}

// --- Marcadores ---
void Steam::uploadScore(const std::string& board, int score, bool keep_best, std::vector<int> details, ScoreCallback done) {
    Impl& d = *impl_;
    const std::uint64_t gen = d.generation;
    auto finish = [this, gen, done](bool ok, int rank, bool changed) {
        if (done && impl_->current(gen)) done(ok, rank, changed);
    };
    if (!d.ready || d.upload_score == nullptr) {
        finish(false, 0, false);
        return;
    }
    d.withBoard(board, [this, score, keep_best, details = std::move(details), finish](std::uint64_t handle) {
        Impl& s = *impl_;
        if (handle == 0) {
            finish(false, 0, false);
            return;
        }
        const SteamAPICall call = s.upload_score(s.user_stats, handle, keep_best ? 1 : 2, score, details.empty() ? nullptr : details.data(),
                                                 static_cast<int>(details.size()));
        if (!s.expect(call, kLeaderboardUploaded, [finish](const std::vector<std::uint8_t>& data, bool failed) {
                const LeaderboardScoreUploaded* r = Impl::as<LeaderboardScoreUploaded>(data);
                const bool ok = !failed && r != nullptr && r->success != 0;
                finish(ok, ok ? r->rank_new : 0, ok && r->changed != 0);
            })) {
            finish(false, 0, false);
        }
    });
}

void Steam::downloadScores(const std::string& board, int mode, int start, int end, EntriesCallback done) {
    Impl& d = *impl_;
    const std::uint64_t gen = d.generation;
    auto finish = [this, gen, done](bool ok, std::vector<SteamLeaderboardEntry> entries) {
        if (done && impl_->current(gen)) done(ok, std::move(entries));
    };
    if (!d.ready || d.download_entries == nullptr) {
        finish(false, {});
        return;
    }
    d.withBoard(board, [this, mode, start, end, finish](std::uint64_t handle) {
        Impl& s = *impl_;
        if (handle == 0) {
            finish(false, {});
            return;
        }
        const SteamAPICall call = s.download_entries(s.user_stats, handle, std::clamp(mode, 0, 2), start, end);
        if (!s.expect(call, kLeaderboardDownloaded, [this, finish](const std::vector<std::uint8_t>& data, bool failed) {
                Impl& t = *impl_;
                const LeaderboardScoresDownloaded* r = Impl::as<LeaderboardScoresDownloaded>(data);
                std::vector<SteamLeaderboardEntry> list;
                if (failed || r == nullptr || t.downloaded_entry == nullptr) {
                    finish(false, {});
                    return;
                }
                for (int i = 0; i < r->count; ++i) {
                    LeaderboardEntry e{};
                    std::int32_t details[16] = {};
                    if (!t.downloaded_entry(t.user_stats, r->entries, i, &e, details, 16)) continue;
                    SteamLeaderboardEntry out;
                    out.user_id = idText(e.user);
                    out.name = friendName(out.user_id);
                    out.rank = e.rank;
                    out.score = e.score;
                    for (int k = 0; k < std::min(e.details, 16); ++k) out.details.push_back(details[k]);
                    list.push_back(std::move(out));
                }
                finish(true, std::move(list));
            })) {
            finish(false, {});
        }
    });
}

// --- Presencia y overlay ---
bool Steam::setRichPresence(const std::string& key, const std::string& value) {
    Impl& d = *impl_;
    return d.ready && d.friends != nullptr && d.set_rich_presence != nullptr && d.set_rich_presence(d.friends, key.c_str(), value.c_str());
}
void Steam::clearRichPresence() {
    Impl& d = *impl_;
    if (d.ready && d.friends != nullptr && d.clear_rich_presence != nullptr) d.clear_rich_presence(d.friends);
}
void Steam::openOverlay(const std::string& dialog) {
    Impl& d = *impl_;
    if (d.ready && d.friends != nullptr && d.overlay != nullptr) d.overlay(d.friends, dialog.empty() ? "friends" : dialog.c_str());
}
void Steam::openOverlayUrl(const std::string& url) {
    Impl& d = *impl_;
    if (d.ready && d.friends != nullptr && d.overlay_url != nullptr) d.overlay_url(d.friends, url.c_str(), 0);
}
void Steam::openStore(std::uint32_t app) {
    Impl& d = *impl_;
    if (d.ready && d.friends != nullptr && d.overlay_store != nullptr) d.overlay_store(d.friends, app != 0 ? app : d.app_id, 0);
}
bool Steam::overlayEnabled() const {
    const Impl& d = *impl_;
    return d.ready && d.utils != nullptr && d.overlay_enabled != nullptr && d.overlay_enabled(d.utils);
}
bool Steam::overlayActive() const { return impl_->ready && impl_->overlay_active; }

// --- Steam Cloud ---
bool Steam::cloudEnabled() const {
    const Impl& d = *impl_;
    if (!d.ready || d.storage == nullptr) return false;
    const bool account = d.cloud_account == nullptr || d.cloud_account(d.storage);
    const bool app = d.cloud_app == nullptr || d.cloud_app(d.storage);
    return account && app;
}
bool Steam::cloudWrite(const std::string& file, const std::string& data) {
    Impl& d = *impl_;
    return d.ready && d.storage != nullptr && d.file_write != nullptr &&
           d.file_write(d.storage, file.c_str(), data.data(), static_cast<std::int32_t>(data.size()));
}
std::optional<std::string> Steam::cloudRead(const std::string& file) const {
    const Impl& d = *impl_;
    if (!d.ready || d.storage == nullptr || d.file_read == nullptr || d.file_size == nullptr) return std::nullopt;
    if (d.file_exists != nullptr && !d.file_exists(d.storage, file.c_str())) return std::nullopt;
    const std::int32_t size = d.file_size(d.storage, file.c_str());
    if (size < 0) return std::nullopt;
    std::string out(static_cast<std::size_t>(size), '\0');
    const std::int32_t got = size > 0 ? d.file_read(d.storage, file.c_str(), out.data(), size) : 0;
    out.resize(static_cast<std::size_t>(std::max(got, 0)));
    return out;
}
bool Steam::cloudExists(const std::string& file) const {
    const Impl& d = *impl_;
    return d.ready && d.storage != nullptr && d.file_exists != nullptr && d.file_exists(d.storage, file.c_str());
}
bool Steam::cloudDelete(const std::string& file) {
    Impl& d = *impl_;
    return d.ready && d.storage != nullptr && d.file_delete != nullptr && d.file_delete(d.storage, file.c_str());
}
std::vector<std::pair<std::string, int>> Steam::cloudFiles() const {
    const Impl& d = *impl_;
    std::vector<std::pair<std::string, int>> out;
    if (!d.ready || d.storage == nullptr || d.file_count == nullptr || d.file_name_and_size == nullptr) return out;
    const std::int32_t n = d.file_count(d.storage);
    for (std::int32_t i = 0; i < n; ++i) {
        std::int32_t size = 0;
        const char* name = d.file_name_and_size(d.storage, i, &size);
        if (name != nullptr) out.emplace_back(name, size);
    }
    return out;
}

// --- Workshop ---
std::vector<SteamWorkshopItem> Steam::subscribedItems() const {
    const Impl& d = *impl_;
    std::vector<SteamWorkshopItem> out;
    if (!d.ready || d.ugc == nullptr || d.num_subscribed == nullptr || d.subscribed_items == nullptr) return out;
    const std::uint32_t n = d.num_subscribed(d.ugc, false);
    std::vector<std::uint64_t> ids(n);
    const std::uint32_t got = n > 0 ? d.subscribed_items(d.ugc, ids.data(), n, false) : 0;
    for (std::uint32_t i = 0; i < std::min(n, got); ++i) {
        SteamWorkshopItem item;
        item.id = idText(ids[i]);
        item.state = d.item_state != nullptr ? d.item_state(d.ugc, ids[i]) : 0;
        if (d.item_install_info != nullptr) {
            char folder[1024] = {};
            std::uint64_t size = 0;
            std::uint32_t stamp = 0;
            if (d.item_install_info(d.ugc, ids[i], &size, folder, sizeof(folder), &stamp)) {
                item.folder = folder;
                item.size = size;
                item.installed = true;
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

void Steam::workshopUpload(const SteamWorkshopUpload& item, UploadCallback done) {
    Impl& d = *impl_;
    const std::uint64_t gen = d.generation;
    auto finish = [this, gen, done](bool ok, const std::string& id, bool legal, const std::string& error) {
        impl_->upload_handle = 0;
        if (done && impl_->current(gen)) done(ok, id, legal, error);
    };
    if (!d.ready || d.ugc == nullptr || d.start_update == nullptr || d.submit_update == nullptr) {
        finish(false, item.id, false, d.ready ? "esta version de Steam no tiene Workshop (UGC)" : d.error);
        return;
    }
    if (d.upload_handle != 0) {
        finish(false, item.id, false, "ya hay una subida en curso");
        return;
    }
    // Rellena y envia la actualizacion de un item que ya existe.
    auto submit = [this, item, finish](std::uint64_t file) {
        Impl& s = *impl_;
        const std::uint64_t handle = s.start_update(s.ugc, s.app_id, file);
        if (handle == 0) {
            finish(false, idText(file), false, "StartItemUpdate fallo");
            return;
        }
        s.upload_handle = handle;
        if (!item.title.empty() && s.set_title != nullptr) s.set_title(s.ugc, handle, item.title.c_str());
        if (!item.description.empty() && s.set_description != nullptr) s.set_description(s.ugc, handle, item.description.c_str());
        if (!item.folder.empty() && s.set_content != nullptr) s.set_content(s.ugc, handle, item.folder.c_str());
        if (!item.preview.empty() && s.set_preview != nullptr) s.set_preview(s.ugc, handle, item.preview.c_str());
        if (s.set_visibility != nullptr) s.set_visibility(s.ugc, handle, std::clamp(item.visibility, 0, 3));
        if (!item.tags.empty() && s.set_tags != nullptr) {
            std::vector<const char*> list;
            for (const std::string& t : item.tags) list.push_back(t.c_str());
            const ParamStringArray tags{list.data(), static_cast<std::int32_t>(list.size())};
            s.set_tags(s.ugc, handle, &tags, false);
        }
        const SteamAPICall call = s.submit_update(s.ugc, handle, item.change_note.empty() ? nullptr : item.change_note.c_str());
        if (!s.expect(call, kSubmitItem, [finish, file](const std::vector<std::uint8_t>& data, bool failed) {
                const SubmitItemUpdateResult* r = Impl::as<SubmitItemUpdateResult>(data);
                const bool ok = !failed && r != nullptr && r->result == 1;
                finish(ok, idText(file), r != nullptr && r->needs_legal,
                       ok ? std::string() : "SubmitItemUpdate fallo (EResult " + std::to_string(r != nullptr ? r->result : 0) + ")");
            })) {
            finish(false, idText(file), false, "SubmitItemUpdate no arranco");
        }
    };
    if (!item.id.empty()) {
        submit(idValue(item.id));
        return;
    }
    if (d.create_item == nullptr) {
        finish(false, {}, false, "CreateItem no esta en la DLL");
        return;
    }
    const SteamAPICall call = d.create_item(d.ugc, d.app_id, 0);
    if (!d.expect(call, kCreateItem, [submit, finish](const std::vector<std::uint8_t>& data, bool failed) {
            const CreateItemResult* r = Impl::as<CreateItemResult>(data);
            if (failed || r == nullptr || r->result != 1) {
                finish(false, {}, r != nullptr && r->needs_legal,
                       "CreateItem fallo (EResult " + std::to_string(r != nullptr ? r->result : 0) + ")");
                return;
            }
            submit(r->file);
        })) {
        finish(false, {}, false, "CreateItem no arranco");
    }
}

float Steam::workshopUploadProgress() const {
    const Impl& d = *impl_;
    if (!d.ready || d.upload_handle == 0 || d.update_progress == nullptr) return -1.0f;
    std::uint64_t processed = 0, total = 0;
    const int status = d.update_progress(d.ugc, d.upload_handle, &processed, &total);
    if (status == 0) return -1.0f;
    // Las fases (preparar, subir contenido, vista previa, confirmar) y el
    // progreso de bytes dentro de la de subir.
    const float bytes = total > 0 ? static_cast<float>(static_cast<double>(processed) / static_cast<double>(total)) : 0.0f;
    switch (status) {
        case 1: return 0.05f;
        case 2: return 0.1f;
        case 3: return 0.1f + 0.8f * bytes;
        case 4: return 0.92f;
        case 5: return 0.97f;
        default: return 0.0f;
    }
}

// --- Salas ---
void Steam::createLobby(int type, int max_members, LobbyCallback done) {
    Impl& d = *impl_;
    const std::uint64_t gen = d.generation;
    auto finish = [this, gen, done](bool ok, const std::string& lobby) {
        if (done && impl_->current(gen)) done(ok, lobby);
    };
    if (!d.ready || d.matchmaking == nullptr || d.create_lobby == nullptr) {
        finish(false, {});
        return;
    }
    const SteamAPICall call = d.create_lobby(d.matchmaking, std::clamp(type, 0, 3), std::clamp(max_members, 1, 250));
    if (!d.expect(call, kLobbyCreated, [finish](const std::vector<std::uint8_t>& data, bool failed) {
            const LobbyCreated* r = Impl::as<LobbyCreated>(data);
            const bool ok = !failed && r != nullptr && r->result == 1;
            finish(ok, ok ? idText(r->lobby) : std::string());
        })) {
        finish(false, {});
    }
}

void Steam::joinLobby(const std::string& lobby, LobbyCallback done) {
    Impl& d = *impl_;
    const std::uint64_t gen = d.generation;
    auto finish = [this, gen, done](bool ok, const std::string& id) {
        if (done && impl_->current(gen)) done(ok, id);
    };
    if (!d.ready || d.matchmaking == nullptr || d.join_lobby == nullptr) {
        finish(false, lobby);
        return;
    }
    const SteamAPICall call = d.join_lobby(d.matchmaking, idValue(lobby));
    if (!d.expect(call, kLobbyEnter, [finish, lobby](const std::vector<std::uint8_t>& data, bool failed) {
            const LobbyEnter* r = Impl::as<LobbyEnter>(data);
            finish(!failed && r != nullptr && r->response == 1, lobby);
        })) {
        finish(false, lobby);
    }
}

void Steam::leaveLobby(const std::string& lobby) {
    Impl& d = *impl_;
    if (d.ready && d.matchmaking != nullptr && d.leave_lobby != nullptr) d.leave_lobby(d.matchmaking, idValue(lobby));
}

void Steam::findLobbies(const std::vector<std::pair<std::string, std::string>>& filters, int max_results, LobbyListCallback done) {
    Impl& d = *impl_;
    const std::uint64_t gen = d.generation;
    auto finish = [this, gen, done](bool ok, std::vector<std::string> list) {
        if (done && impl_->current(gen)) done(ok, std::move(list));
    };
    if (!d.ready || d.matchmaking == nullptr || d.request_lobby_list == nullptr) {
        finish(false, {});
        return;
    }
    if (d.lobby_string_filter != nullptr) {
        for (const auto& [key, value] : filters) d.lobby_string_filter(d.matchmaking, key.c_str(), value.c_str(), 0);
    }
    if (d.lobby_count_filter != nullptr && max_results > 0) d.lobby_count_filter(d.matchmaking, max_results);
    const SteamAPICall call = d.request_lobby_list(d.matchmaking);
    if (!d.expect(call, kLobbyMatchList, [this, finish](const std::vector<std::uint8_t>& data, bool failed) {
            Impl& s = *impl_;
            const LobbyMatchList* r = Impl::as<LobbyMatchList>(data);
            std::vector<std::string> list;
            if (!failed && r != nullptr && s.lobby_by_index != nullptr) {
                for (std::uint32_t i = 0; i < r->count; ++i) list.push_back(idText(s.lobby_by_index(s.matchmaking, static_cast<int>(i))));
            }
            finish(!failed && r != nullptr, std::move(list));
        })) {
        finish(false, {});
    }
}

bool Steam::setLobbyData(const std::string& lobby, const std::string& key, const std::string& value) {
    Impl& d = *impl_;
    return d.ready && d.matchmaking != nullptr && d.set_lobby_data != nullptr &&
           d.set_lobby_data(d.matchmaking, idValue(lobby), key.c_str(), value.c_str());
}

std::string Steam::lobbyData(const std::string& lobby, const std::string& key) const {
    const Impl& d = *impl_;
    const char* v = d.ready && d.matchmaking != nullptr && d.get_lobby_data != nullptr
                        ? d.get_lobby_data(d.matchmaking, idValue(lobby), key.c_str())
                        : nullptr;
    return v != nullptr ? v : "";
}

std::vector<SteamLobbyMember> Steam::lobbyMembers(const std::string& lobby) const {
    const Impl& d = *impl_;
    std::vector<SteamLobbyMember> out;
    if (!d.ready || d.matchmaking == nullptr || d.lobby_member_count == nullptr || d.lobby_member == nullptr) return out;
    const std::uint64_t id = idValue(lobby);
    const int n = d.lobby_member_count(d.matchmaking, id);
    for (int i = 0; i < n; ++i) {
        SteamLobbyMember m;
        m.id = idText(d.lobby_member(d.matchmaking, id, i));
        m.name = friendName(m.id);
        out.push_back(std::move(m));
    }
    return out;
}

std::string Steam::lobbyOwner(const std::string& lobby) const {
    const Impl& d = *impl_;
    return d.ready && d.matchmaking != nullptr && d.lobby_owner != nullptr ? idText(d.lobby_owner(d.matchmaking, idValue(lobby)))
                                                                             : std::string();
}

void Steam::inviteToLobby(const std::string& lobby) {
    Impl& d = *impl_;
    if (d.ready && d.friends != nullptr && d.overlay_invite != nullptr) d.overlay_invite(d.friends, idValue(lobby));
}

void Steam::setLobbyJoinRequestedListener(std::function<void(const std::string& lobby)> listener) {
    impl_->join_listener = std::move(listener);
}

void Steam::setOverlayListener(std::function<void(bool active)> listener) { impl_->overlay_listener = std::move(listener); }

}  // namespace cramion::platform
