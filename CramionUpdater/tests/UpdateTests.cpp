// Prueba de consola del actualizador: versiones, feed (formato de GitHub),
// notas, descarga desde un feed local, zip (carpeta de primer nivel, rutas
// peligrosas, CRC), instalacion con vuelta atras, ajustes y reopen.json.
//
//   CramionUpdateTests.exe            todo sin conexion
//   CramionUpdateTests.exe --online   ademas consulta GitHub de verdad

#include <CramionUpdater/Update.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace cramion;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  OK    " : "  FALLO ") << what << "\n";
    if (!ok) ++g_failures;
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Zip minimo sin comprimir (metodo 0) para los casos raros.
std::uint32_t crc32(const std::string& s) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char c : s) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc & 1u) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

void put16(std::string& out, std::uint32_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

void writeStoredZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files, bool bad_crc = false) {
    std::string zip;
    std::string central;
    for (const auto& [name, data] : files) {
        const std::uint32_t offset = static_cast<std::uint32_t>(zip.size());
        const std::uint32_t crc = crc32(data) ^ (bad_crc ? 1u : 0u);
        put32(zip, 0x04034b50u);
        put16(zip, 20);
        put16(zip, 0);
        put16(zip, 0);  // metodo 0
        put16(zip, 0);
        put16(zip, 0);
        put32(zip, crc);
        put32(zip, static_cast<std::uint32_t>(data.size()));
        put32(zip, static_cast<std::uint32_t>(data.size()));
        put16(zip, static_cast<std::uint32_t>(name.size()));
        put16(zip, 0);
        zip += name;
        zip += data;
        put32(central, 0x02014b50u);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, crc);
        put32(central, static_cast<std::uint32_t>(data.size()));
        put32(central, static_cast<std::uint32_t>(data.size()));
        put16(central, static_cast<std::uint32_t>(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += name;
    }
    const std::uint32_t cd_offset = static_cast<std::uint32_t>(zip.size());
    zip += central;
    put32(zip, 0x06054b50u);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<std::uint32_t>(files.size()));
    put16(zip, static_cast<std::uint32_t>(files.size()));
    put32(zip, static_cast<std::uint32_t>(central.size()));
    put32(zip, cd_offset);
    put16(zip, 0);
    writeAll(path, zip);
}

std::string fileUrl(const fs::path& path) {
    std::string s = update::narrow(path.wstring());
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    std::string out = "file:///";
    for (const char c : s) {
        if (c == ' ') out += "%20";
        else out += c;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool online = argc >= 2 && std::string(argv[1]) == "--online";
    const fs::path root = fs::temp_directory_path() / "cramion update tests";  // con espacio a proposito
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    // Ajustes y reopen.json en la carpeta de la prueba, no en %LOCALAPPDATA%.
    SetEnvironmentVariableW(L"CRAMION_UPDATE_DATA", (root / "data").c_str());
    SetEnvironmentVariableW(L"CRAMION_UPDATE_FEED", nullptr);

    std::cout << "Versiones\n";
    {
        using update::Version;
        check(Version::parse("v0.6.1") == Version{0, 6, 1, true}, "v0.6.1 se lee");
        check(Version::parse("Cramion Engine v1.2").valid && Version::parse("Cramion Engine v1.2").minor == 2, "sin parche");
        check(Version::parse("0.10.0") > Version::parse("0.9.9"), "0.10.0 > 0.9.9 (numerico, no texto)");
        check(Version::parse("1.0.0") > Version::parse("0.99.99"), "1.0.0 > 0.99.99");
        check(!Version::parse("sin numero").valid, "texto sin version no vale");
        check(update::currentVersion() == Version::parse(CRAMION_VERSION_STRING), "version actual = la del proyecto");
    }

    std::cout << "Feed (formato de GitHub)\n";
    {
        const std::string github = R"({
            "tag_name": "v0.7.0", "name": "Cramion Engine v0.7.0",
            "html_url": "https://github.com/MantraxStudios/Cramion/releases/tag/v0.7.0",
            "published_at": "2026-10-02T10:00:00Z",
            "body": "# Cramion 0.7\n\n### Novedades\n- **Hub** nuevo con `Actualizaciones`\n  - sub [enlace](https://x)\ntexto\n",
            "assets": [
                {"name": "Cramion-win64.zip.sha256", "browser_download_url": "https://x/sha", "size": 64},
                {"name": "Cramion-win64.zip", "browser_download_url": "https://x/Cramion-win64.zip", "size": 16252928}
            ]})";
        update::Release r;
        std::string error;
        check(update::parseRelease(github, r, &error), "se lee la release");
        check(r.version == update::Version::parse("0.7.0"), "version 0.7.0");
        check(r.zip_url == "https://x/Cramion-win64.zip" && r.zip_size == 16252928, "elige Cramion-win64.zip");
        check(update::formatDate(r.published_at) == "02/10/2026", "fecha dd/mm/aaaa");
        const auto notes = update::parseNotes(r.notes);
        check(notes.size() == 6, "notas: 6 lineas (" + std::to_string(notes.size()) + ")");
        if (notes.size() == 6) {
            check(notes[0].kind == update::NoteLine::Kind::Heading && notes[0].level == 1 && notes[0].text == "Cramion 0.7", "titulo #");
            check(notes[2].kind == update::NoteLine::Kind::Heading && notes[2].level == 3, "titulo ###");
            check(notes[3].kind == update::NoteLine::Kind::Bullet && notes[3].text == "Hub nuevo con Actualizaciones",
                  "vineta sin ** ni ` (" + notes[3].text + ")");
            check(notes[4].level == 1 && notes[4].text == "sub enlace", "vineta anidada y enlace");
        }

        const std::string list = R"([
            {"tag_name": "v0.9.0", "draft": true},
            {"tag_name": "v0.8.0-beta", "prerelease": true},
            {"tag_name": "v0.7.1", "assets": []},
            {"tag_name": "v0.7.0"}])";
        check(update::parseRelease(list, r, &error) && r.version == update::Version::parse("0.7.1") && r.zip_url.empty(),
              "lista: la mas alta sin borradores ni previas, sin paquete");
        check(!update::parseRelease(R"({"message": "API rate limit exceeded"})", r, &error) &&
                  error.find("rate limit") != std::string::npos,
              "mensaje de error de GitHub");
        check(!update::parseRelease("<html>", r, &error), "respuesta que no es JSON");
    }

    std::cout << "Formatos\n";
    check(update::formatBytes(16252928) == "15,5 MB", "15,5 MB (" + update::formatBytes(16252928) + ")");
    check(update::formatBytes(2048) == "2 KB", "2 KB");

    std::cout << "Descarga desde un feed local y zip\n";
    const fs::path fixture = fs::path(CRAMION_UPDATE_FIXTURE);
    const fs::path feed_dir = root / "feed";
    fs::create_directories(feed_dir);
    fs::copy_file(fixture / "Cramion-win64.zip", feed_dir / "Cramion-win64.zip");
    const std::uint64_t zip_size = fs::file_size(feed_dir / "Cramion-win64.zip");
    writeAll(feed_dir / "latest.json", R"({"tag_name": "v9.9.9", "body": "- prueba",
        "assets": [{"name": "Cramion-win64.zip", "browser_download_url": "Cramion-win64.zip", "size": )" +
                                           std::to_string(zip_size) + "}]}");
    update::Release release;
    std::string error;
    check(update::fetchLatest(fileUrl(feed_dir / "latest.json"), release, &error), "feed file:/// con espacios: " + error);
    check(release.version == update::Version::parse("9.9.9"), "version 9.9.9 en el feed");
    check(fs::path(update::widen(release.zip_url)) == feed_dir / "Cramion-win64.zip", "zip relativo al feed");

    const fs::path zip = root / "downloads" / "Cramion-9.9.9-win64.zip";
    int calls = 0;
    std::uint64_t last_done = 0;
    check(update::downloadFile(release.zip_url, zip,
                               [&](std::uint64_t done, std::uint64_t total) {
                                   ++calls;
                                   last_done = done;
                                   return total == zip_size;
                               },
                               &error),
          "descarga: " + error);
    check(calls > 0 && last_done == zip_size && fs::file_size(zip) == zip_size, "progreso hasta el final");
    check(!fs::exists(zip.wstring() + L".part"), "sin .part al terminar");
    check(!update::downloadFile(release.zip_url, root / "downloads" / "cancelada.zip",
                                [](std::uint64_t, std::uint64_t) { return false; }, &error) &&
              !fs::exists(root / "downloads" / "cancelada.zip") && !fs::exists(root / "downloads" / "cancelada.zip.part"),
          "cancelar no deja archivos");

    const fs::path staged = root / "staged";
    check(update::extractZip(zip, staged, {}, &error), "descomprime (deflate de cmake): " + error);
    check(readAll(staged / "CramionEditor.exe") == "editor nuevo", "quita la carpeta Cramion-9.9.9-win64/");
    check(readAll(staged / "README.md") == readAll(fixture / "pkg" / "Cramion-9.9.9-win64" / "README.md"), "archivo comprimido intacto");
    check(readAll(staged / "docs" / "manual" / "index.html") == "<html>manual nuevo</html>", "subcarpetas");
    check(update::looksLikeEnginePackage(staged), "parece un paquete del motor");

    writeStoredZip(root / "evil.zip", {{"ok.txt", "bien"}, {"../fuera.txt", "mal"}});
    check(!update::extractZip(root / "evil.zip", root / "evil", {}, &error) && !fs::exists(root / "fuera.txt"),
          "rechaza ../ (" + error + ")");
    writeStoredZip(root / "crc.zip", {{"a/x.txt", "hola"}}, true);
    check(!update::extractZip(root / "crc.zip", root / "crc", {}, &error) && error.find("CRC") != std::string::npos,
          "detecta CRC malo");
    writeStoredZip(root / "flat.zip", {{"x.txt", "hola"}, {"sub/y.txt", "adios"}});
    check(update::extractZip(root / "flat.zip", root / "flat", {}, &error) && readAll(root / "flat" / "sub" / "y.txt") == "adios",
          "zip sin carpeta comun se deja igual");

    std::cout << "Instalacion\n";
    const fs::path install = root / "install";
    writeAll(install / "CramionEditor.exe", "editor viejo");
    writeAll(install / "shaders" / "mesh.vert.spv", "shader viejo");
    writeAll(install / "assets" / "mio.txt", "no tocar");
    {
        // Un archivo bloqueado del todo: falla y todo vuelve a como estaba.
        HANDLE lock = CreateFileW((install / "shaders" / "mesh.vert.spv").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "bloqueo de prueba");
        const bool ok = update::installFiles(staged, install, {}, &error);
        CloseHandle(lock);
        check(!ok, "falla con un archivo bloqueado (" + error + ")");
        check(readAll(install / "CramionEditor.exe") == "editor viejo", "vuelta atras: el editor viejo sigue");
        check(!fs::exists(install / "CramionUpdater.exe"), "vuelta atras: no quedan archivos nuevos");
        bool leftovers = false;
        for (const auto& e : fs::recursive_directory_iterator(install)) {
            if (e.path().wstring().find(update::kOldSuffix) != std::wstring::npos) leftovers = true;
        }
        check(!leftovers, "vuelta atras: sin .cramion-old");
    }
    std::uint64_t installed = 0;
    check(update::installFiles(staged, install,
                               [&](std::uint64_t done, std::uint64_t) {
                                   installed = done;
                                   return true;
                               },
                               &error),
          "instala: " + error);
    check(readAll(install / "CramionEditor.exe") == "editor nuevo" && readAll(install / "shaders" / "mesh.vert.spv") == "shader nuevo",
          "archivos reemplazados");
    check(readAll(install / "assets" / "mio.txt") == "no tocar", "lo que no viene en el paquete no se toca");
    check(installed > 0, "progreso de la instalacion");
    check(!fs::exists(install / L"CramionEditor.exe.cramion-old"), "los viejos libres se borran al momento");
    check(update::cleanupOldFiles() == 0, "limpieza sin pendientes");
    {
        // Un viejo en uso (como el propio actualizador): queda apuntado y se
        // borra al liberarse.
        writeAll(install / "CramionUpdater.exe", "actualizador en uso");
        HANDLE in_use = CreateFileW((install / "CramionUpdater.exe").c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(update::installFiles(staged, install, {}, &error), "instala con un archivo abierto (renombrable): " + error);
        check(readAll(install / "CramionUpdater.exe") == "actualizador nuevo", "el abierto se reemplaza igual");
        CloseHandle(in_use);
        check(update::cleanupOldFiles() == 0 && !fs::exists(install / L"CramionUpdater.exe.cramion-old"),
              "al volver a abrir se borra el viejo");
    }

    std::cout << "Ajustes, reopen.json y procesos\n";
    {
        update::Settings s = update::loadSettings();
        check(s.check_on_startup && s.skipped_version.empty(), "ajustes por defecto");
        s.check_on_startup = false;
        s.skipped_version = "9.9.9";
        update::saveSettings(s);
        const update::Settings t = update::loadSettings();
        check(!t.check_on_startup && t.skipped_version == "9.9.9", "ajustes guardados");
        check(update::feedUrl() == update::kDefaultFeed, "feed por defecto: GitHub");
        SetEnvironmentVariableW(L"CRAMION_UPDATE_FEED", L"file:///C:/feed.json");
        check(update::feedUrl() == "file:///C:/feed.json", "CRAMION_UPDATE_FEED manda");
        SetEnvironmentVariableW(L"CRAMION_UPDATE_FEED", nullptr);

        update::addReopenProject(L"C:/Proyectos/Mi juego/Mi juego.crproj");
        update::addReopenProject(L"C:/Proyectos/Mi juego/Mi juego.crproj");
        update::addReopenProject(L"C:/Proyectos/Otro/Otro.crproj");
        update::markReopenHub();
        const update::ReopenList list = update::takeReopenList();
        check(list.projects.size() == 2 && list.hub, "reopen: 2 proyectos sin repetir y el Hub");
        check(update::takeReopenList().projects.empty(), "reopen se vacia al leerlo");
        check(update::runningEngineApps(install, GetCurrentProcessId()).empty(), "ningun editor abierto en la carpeta de prueba");
        check(update::quoteArgument(L"C:\\a b\\c.crproj") == L"\"C:\\a b\\c.crproj\"", "argumento con espacios");
        check(update::quoteArgument(L"C:\\a b\\") == L"\"C:\\a b\\\\\"", "barra final antes de la comilla");
    }

    if (online) {
        std::cout << "GitHub (en linea)\n";
        update::Release r;
        const bool ok = update::fetchLatest(update::kDefaultFeed, r, &error);
        check(ok, "consulta a GitHub: " + (ok ? r.tag + " " + (r.zip_url.empty() ? "(sin paquete)" : update::formatBytes(r.zip_size)) : error));
    }

    fs::remove_all(root, ec);
    if (g_failures == 0) {
        std::cout << "TODO OK\n";
        return 0;
    }
    std::cout << g_failures << " fallo(s)\n";
    return 1;
}
