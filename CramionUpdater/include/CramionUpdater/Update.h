#ifndef CRAMION_UPDATER_UPDATE_H
#define CRAMION_UPDATER_UPDATE_H

// Actualizaciones de Cramion: lo comparten el actualizador (CramionUpdater.exe,
// una aplicacion aparte con su propia interfaz) y el editor (el Hub y el aviso
// dentro del editor).
//
//   1. Buscar: la ultima version publicada en GitHub (la API de releases) o en
//      otro feed con el mismo JSON (CRAMION_UPDATE_FEED o update.json).
//   2. Descargar el paquete (Cramion-win64.zip) con WinHTTP, con progreso.
//   3. Descomprimir en una carpeta aparte (zip propio + inflate de stb) y
//      comprobar el CRC de cada archivo.
//   4. Instalar: cada archivo de la carpeta del motor se renombra a
//      *.cramion-old antes de copiar el nuevo (asi se pueden reemplazar incluso
//      los .exe y .dll en uso). Si algo falla, se deshace todo.
//
// Los editores abiertos se guardan y se cierran solos: el actualizador les
// manda el mensaje kPrepareMessage y cada uno guarda la escena, los prefabs,
// los scripts y el Animator, apunta su proyecto en reopen.json y se cierra.
// Al terminar, el actualizador vuelve a abrir esos proyectos.

#include <atomic>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::update {

inline constexpr const char* kDefaultFeed = "https://api.github.com/repos/MantraxStudios/Cramion/releases/latest";
// Canal beta: la lista de releases (incluye las previas; "latest" no las da).
inline constexpr const char* kBetaFeed = "https://api.github.com/repos/MantraxStudios/Cramion/releases?per_page=30";
inline constexpr const char* kReleasesPage = "https://github.com/MantraxStudios/Cramion/releases";
inline constexpr const char* kPackageAsset = "Cramion-win64.zip";
inline constexpr const wchar_t* kUpdaterExe = L"CramionUpdater.exe";
inline constexpr const wchar_t* kEditorExe = L"CramionEditor.exe";
// Mensaje de ventana registrado (RegisterWindowMessageW) que el actualizador
// manda a los editores: "guarda todo y cierrate".
inline constexpr const wchar_t* kPrepareMessage = L"CramionPrepareForUpdate";
// Sufijo de los archivos sustituidos (se borran al volver a abrir el motor).
inline constexpr const wchar_t* kOldSuffix = L".cramion-old";

// --- Versiones -------------------------------------------------------------------

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;
    // Version previa: "beta.1" en "0.7.0-beta.1" (vacio = estable). Como en
    // semver, 0.7.0-beta.1 < 0.7.0-beta.2 < 0.7.0.
    std::string pre;

    // "0.6.1", "v0.6.1", "Cramion 0.6.1", "v0.7.0-beta.1".
    static Version parse(std::string_view text);
    std::string str() const;
    std::strong_ordering operator<=>(const Version& other) const;
    bool operator==(const Version& other) const { return (*this <=> other) == 0; }
};

// La version de este ejecutable (CRAMION_VERSION_STRING al compilar).
Version currentVersion();

// --- Releases ----------------------------------------------------------------------

struct Release {
    Version version;
    std::string tag;           // "v0.6.2"
    std::string name;          // "Cramion Engine v0.6.2"
    std::string notes;         // cuerpo en Markdown
    std::string page_url;      // la pagina de la release
    std::string published_at;  // ISO 8601
    std::string zip_url;       // vacio: la release aun no tiene paquete
    std::uint64_t zip_size = 0;
    bool prerelease = false;   // version beta (canal beta)
    // SHA-256 del zip (hex en minusculas): el "digest" que da GitHub para cada
    // archivo, el campo "sha256" de otros feeds o el Cramion-win64.zip.sha256
    // publicado al lado (sha256_url, se descarga al comprobar).
    std::string sha256;
    std::string sha256_url;
};

// El feed: CRAMION_UPDATE_FEED > update.json ("feed") > GitHub (la ultima
// estable, o la lista de releases con el canal beta).
std::string feedUrl();
// El canal beta esta activado (update.json "channel": "beta").
bool betaChannel();
// JSON de GitHub (releases/latest) o una lista de releases (la mas nueva que
// no sea borrador; las previas solo con `allow_prerelease`). `feed_url`
// resuelve las rutas relativas del zip.
bool parseRelease(const std::string& json, Release& out, std::string* error, const std::string& feed_url = {},
                  bool allow_prerelease = false);
bool fetchLatest(const std::string& feed, Release& out, std::string* error, bool allow_prerelease = false);

// --- SHA-256 ---------------------------------------------------------------------

std::string sha256Hex(const void* data, std::size_t size);
// Vacio si no se puede leer.
std::string sha256File(const std::filesystem::path& file, std::string* error = nullptr);
// Comprueba el zip con el SHA-256 de la release (lo descarga si hace falta).
// false = no coincide o no se pudo comprobar. `verified` = habia SHA-256
// publicado y coincide (sin SHA-256 publicado devuelve true y verified=false).
bool verifyPackage(const Release& release, const std::filesystem::path& zip, bool* verified, std::string* error);

// Notas en Markdown sencillo para dibujarlas (titulos, vinetas, texto).
struct NoteLine {
    enum class Kind { Heading, Bullet, Text, Blank };
    Kind kind = Kind::Text;
    int level = 0;  // titulos: 1 = #, 2 = ##...; vinetas: sangria
    std::string text;  // sin ** ni `
};
std::vector<NoteLine> parseNotes(const std::string& markdown);
// "2026-09-27T01:48:31Z" -> "27/09/2026".
std::string formatDate(const std::string& iso);
// 16252928 -> "15,5 MB".
std::string formatBytes(std::uint64_t bytes);

// --- Descargas ---------------------------------------------------------------------

// Devuelve false para cancelar.
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

// http(s)://, file:/// o una ruta local.
bool httpGet(const std::string& url, std::string& body, std::string* error);
bool downloadFile(const std::string& url, const std::filesystem::path& destination, const Progress& progress,
                  std::string* error);

// --- Zip e instalacion -------------------------------------------------------------

// Descomprime en `destination` (se vacia antes). Si todas las entradas estan
// dentro de una misma carpeta (Cramion-0.6.2-win64/), se quita. Rechaza rutas
// con .. o absolutas y comprueba el CRC.
bool extractZip(const std::filesystem::path& zip, const std::filesystem::path& destination, const Progress& progress,
                std::string* error);

// Copia los archivos de `staged` sobre `install_dir` (renombrando antes los que
// ya existen a *.cramion-old). Nada se borra: los archivos que no vienen en el
// paquete (escenas de demostracion, etc.) se quedan. Si algo falla, todo
// vuelve a como estaba.
bool installFiles(const std::filesystem::path& staged, const std::filesystem::path& install_dir,
                  const Progress& progress, std::string* error);
// Borra los *.cramion-old que dejo la ultima instalacion (apuntados en
// cleanup.txt) y que ya no esten en uso. Devuelve cuantos quedan.
int cleanupOldFiles();
// Tiene los ejecutables del motor (sirve para no instalar un zip que no es).
bool looksLikeEnginePackage(const std::filesystem::path& folder);

// --- Ajustes y carpetas ------------------------------------------------------------

// %LOCALAPPDATA%/Cramion
std::filesystem::path dataFolder();
// Descargas y paquetes descomprimidos.
std::filesystem::path downloadsFolder();
// Carpeta del ejecutable actual (la instalacion del motor).
std::filesystem::path installFolder();

struct Settings {
    bool check_on_startup = true;
    std::string skipped_version;  // "0.6.2": no avisar de esa
    std::string feed;             // vacio = GitHub
    std::string channel = "estable";  // "estable" o "beta"
    std::int64_t last_check = 0;  // segundos desde 1970
    std::string last_seen_version;
};
Settings loadSettings();
void saveSettings(const Settings& settings);

// Proyectos a reabrir tras actualizar (reopen.json). El editor apunta el suyo
// al cerrarse para la actualizacion; el actualizador los lee y borra.
void addReopenProject(const std::filesystem::path& project);
void markReopenHub();  // el Hub estaba abierto: se vuelve a abrir
struct ReopenList {
    std::vector<std::filesystem::path> projects;
    bool hub = false;
};
ReopenList takeReopenList();

// --- Procesos ----------------------------------------------------------------------

struct RunningApp {
    unsigned long pid = 0;
    std::wstring exe;  // nombre del ejecutable
    bool editor = false;
};
// Procesos que ejecutan algo de `install_dir` (menos `exclude_pid`).
std::vector<RunningApp> runningEngineApps(const std::filesystem::path& install_dir, unsigned long exclude_pid);
// Manda kPrepareMessage a las ventanas de esos procesos. Devuelve cuantas.
int askAppsToSaveAndClose(const std::vector<RunningApp>& apps);
// Lanza un ejecutable (argumentos ya entrecomillados). 0 si falla.
unsigned long launch(const std::filesystem::path& exe, const std::wstring& arguments,
                     const std::filesystem::path& working_dir = {});
std::wstring quoteArgument(const std::wstring& argument);
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
bool openUrl(const std::string& url);

// --- Busqueda en segundo plano (editor y actualizador) ------------------------------

struct CheckJob {
    enum class State { Checking, Done, Failed };
    std::atomic<State> state{State::Checking};
    Release release;  // valido con Done
    std::string error;
    bool newer() const { return state == State::Done && release.version.valid && release.version > currentVersion(); }
};
// Lanza la busqueda en otro hilo. El hilo se desprende: si nadie espera,
// termina solo.
std::shared_ptr<CheckJob> startCheck(const std::string& feed = {});

}  // namespace cramion::update

#endif  // CRAMION_UPDATER_UPDATE_H
