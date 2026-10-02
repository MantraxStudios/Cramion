#ifndef CRAMION_CORE_PLATFORM_STEAM_H
#define CRAMION_CORE_PLATFORM_STEAM_H

// Steam (Steamworks) sin enlazar el SDK: steam_api64.dll se carga al vuelo
// (LoadLibrary / dlopen) y se usa su API plana (SteamAPI_ISteam*_*). Si no
// esta la DLL, Steam no esta abierto o el juego no es de esa cuenta, todo
// sigue funcionando: available() es false y cada llamada no hace nada (o
// devuelve vacio / false).
//
//   Logros y estadisticas   unlockAchievement, setStatInt, storeStats...
//   Marcadores              uploadScore / downloadScores (asincronos)
//   Presencia               setRichPresence ("Nivel 3, 2 vidas")
//   Overlay                 openOverlay("achievements"), openOverlayUrl
//   Nube (Steam Cloud)      cloudWrite / cloudRead (partidas guardadas)
//   Workshop                subscribedItems, workshopUpload
//   Salas (lobbies)         createLobby / findLobbies / joinLobby + datos de
//                           la sala (p. ej. la IP:puerto del servidor de la
//                           Network API para que los demas se conecten)
//
// Los resultados asincronos llegan en update() (cada frame) con el despacho
// manual de Steamworks (SteamAPI_ManualDispatch_*): sin hilos.
//
// Los identificadores de Steam (usuarios, salas, items) son de 64 bits: van
// como texto ("76561198000000000") para que Lua no pierda precision.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cramion::platform {

struct SteamLeaderboardEntry {
    std::string user_id;
    std::string name;
    int rank = 0;
    int score = 0;
    std::vector<int> details;
};

struct SteamWorkshopItem {
    std::string id;
    std::string folder;  // donde esta instalado (vacio si aun no)
    std::uint64_t size = 0;
    std::uint32_t state = 0;  // EItemState (1 suscrito, 4 instalado, 8 actualizar, 16 descargando)
    bool installed = false;
};

struct SteamWorkshopUpload {
    std::string id;  // vacio = item nuevo
    std::string title;
    std::string description;
    std::string folder;   // contenido (carpeta entera)
    std::string preview;  // imagen de vista previa (opcional)
    std::string change_note;
    int visibility = 0;  // 0 publico, 1 amigos, 2 privado, 3 oculto
    std::vector<std::string> tags;
};

struct SteamLobbyMember {
    std::string id;
    std::string name;
};

class Steam {
public:
    static Steam& instance();

    // app_id 0 = el de steam_appid.txt o el de Steam (juego lanzado desde
    // Steam). dll: la DLL o su carpeta (vacio = junto al ejecutable / PATH).
    // Se puede llamar varias veces: si ya esta en marcha no hace nada.
    bool init(std::uint32_t app_id, const std::filesystem::path& dll = {});
    void shutdown();
    // Cada frame: resultados asincronos y avisos (overlay, invitaciones).
    void update();
    bool available() const;
    // Por que no esta disponible (sin DLL, Steam cerrado...).
    const std::string& error() const;
    std::uint32_t appId() const;
    // Si el juego no se abrio desde Steam y deberia: lo relanza Steam (true = cerrar el juego).
    static bool restartAppIfNecessary(std::uint32_t app_id, const std::filesystem::path& dll = {});

    // --- Usuario ---
    std::string userId() const;
    std::string userName() const;
    std::string friendName(const std::string& id) const;
    std::string language() const;  // "spanish", "english"...
    bool isDlcInstalled(std::uint32_t app) const;
    bool onSteamDeck() const;

    // --- Logros y estadisticas ---
    bool unlockAchievement(const std::string& name);
    bool clearAchievement(const std::string& name);
    bool achievementUnlocked(const std::string& name) const;
    // Muestra el aviso de progreso (p. ej. 3/10) sin desbloquear.
    bool indicateAchievementProgress(const std::string& name, std::uint32_t current, std::uint32_t max);
    bool setStatInt(const std::string& name, int value);
    bool setStatFloat(const std::string& name, float value);
    std::optional<int> statInt(const std::string& name) const;
    std::optional<float> statFloat(const std::string& name) const;
    // Sube los logros y estadisticas cambiados (si no, se suben al cerrar).
    bool storeStats();

    // --- Marcadores (leaderboards) ---
    // keep_best: solo si mejora la puntuacion guardada. El marcador se crea si
    // no existe (descendente, numerico).
    using ScoreCallback = std::function<void(bool ok, int rank, bool changed)>;
    void uploadScore(const std::string& board, int score, bool keep_best, std::vector<int> details, ScoreCallback done);
    // mode: 0 global (filas start..end), 1 alrededor del jugador (start..end
    // relativo, p. ej. -5..5), 2 amigos.
    using EntriesCallback = std::function<void(bool ok, std::vector<SteamLeaderboardEntry> entries)>;
    void downloadScores(const std::string& board, int mode, int start, int end, EntriesCallback done);

    // --- Presencia y overlay ---
    bool setRichPresence(const std::string& key, const std::string& value);
    void clearRichPresence();
    // dialog: friends, community, players, settings, officialgamegroup, stats, achievements
    void openOverlay(const std::string& dialog);
    void openOverlayUrl(const std::string& url);
    void openStore(std::uint32_t app = 0);  // 0 = este juego
    bool overlayEnabled() const;
    bool overlayActive() const;  // el overlay esta abierto ahora (pausar el juego)

    // --- Steam Cloud ---
    bool cloudEnabled() const;
    bool cloudWrite(const std::string& file, const std::string& data);
    std::optional<std::string> cloudRead(const std::string& file) const;
    bool cloudExists(const std::string& file) const;
    bool cloudDelete(const std::string& file);
    std::vector<std::pair<std::string, int>> cloudFiles() const;  // nombre, bytes

    // --- Workshop ---
    std::vector<SteamWorkshopItem> subscribedItems() const;
    using UploadCallback = std::function<void(bool ok, const std::string& id, bool needs_legal, const std::string& error)>;
    void workshopUpload(const SteamWorkshopUpload& item, UploadCallback done);
    // 0..1 de la subida en curso (-1 si no hay ninguna).
    float workshopUploadProgress() const;

    // --- Salas (lobbies) ---
    using LobbyCallback = std::function<void(bool ok, const std::string& lobby)>;
    // type: 0 privada, 1 solo amigos, 2 publica, 3 invisible
    void createLobby(int type, int max_members, LobbyCallback done);
    void joinLobby(const std::string& lobby, LobbyCallback done);
    void leaveLobby(const std::string& lobby);
    using LobbyListCallback = std::function<void(bool ok, std::vector<std::string> lobbies)>;
    // filters: clave = valor exacto (datos de la sala)
    void findLobbies(const std::vector<std::pair<std::string, std::string>>& filters, int max_results, LobbyListCallback done);
    bool setLobbyData(const std::string& lobby, const std::string& key, const std::string& value);
    std::string lobbyData(const std::string& lobby, const std::string& key) const;
    std::vector<SteamLobbyMember> lobbyMembers(const std::string& lobby) const;
    std::string lobbyOwner(const std::string& lobby) const;
    void inviteToLobby(const std::string& lobby);  // el dialogo de invitar del overlay
    // Un amigo invito al jugador y acepto (o "Unirse a la partida" en Steam).
    void setLobbyJoinRequestedListener(std::function<void(const std::string& lobby)> listener);
    void setOverlayListener(std::function<void(bool active)> listener);

    // Olvida los callbacks pendientes de los scripts (al parar el juego): sus
    // resultados llegan pero no se llaman.
    void cancelCallbacks();

    Steam(const Steam&) = delete;
    Steam& operator=(const Steam&) = delete;

private:
    Steam();
    ~Steam();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::platform

#endif  // CRAMION_CORE_PLATFORM_STEAM_H
