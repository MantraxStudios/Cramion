// Actualizaciones de Cramion (ver Update.h).

#include <CramionUpdater/Update.h>

#include "Inflate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#ifndef CRAMION_VERSION_STRING
#define CRAMION_VERSION_STRING "0.0.0"
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace cramion::update {

namespace {

void setError(std::string* error, const std::string& text) {
    if (error != nullptr) *error = text;
}

// Texto del error de Windows en UTF-8 (ec.message() viene en la pagina de
// codigos del sistema y los acentos saldrian rotos en la interfaz).
std::string errorText(const std::error_code& ec) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(ec.value()), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = n > 0 && buffer != nullptr ? std::wstring(buffer, n) : L"error " + std::to_wstring(ec.value());
    if (buffer != nullptr) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
    return narrow(text);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring lowerWide(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    fs::rename(temp, path, ec);
    return !ec;
}

// "file:///C:/a%20b/x.zip" o "C:/x.zip" -> ruta; vacio si es http(s).
fs::path localPath(const std::string& url) {
    std::string rest;
    if (startsWith(url, "file:///")) {
        rest = url.substr(8);
    } else if (startsWith(url, "file://")) {
        rest = url.substr(7);
    } else if (url.find("://") == std::string::npos) {
        return fs::path(widen(url));
    } else {
        return {};
    }
    std::string decoded;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
            decoded += static_cast<char>(std::stoi(rest.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += rest[i];
        }
    }
    return fs::path(widen(decoded));
}

// Nombre con el mutex del sistema: reopen.json lo pueden tocar varios editores.
class NamedLock {
public:
    explicit NamedLock(const wchar_t* name) : mutex_(CreateMutexW(nullptr, FALSE, name)) {
        if (mutex_ != nullptr) WaitForSingleObject(mutex_, 3000);
    }
    ~NamedLock() {
        if (mutex_ != nullptr) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }
    NamedLock(const NamedLock&) = delete;
    NamedLock& operator=(const NamedLock&) = delete;

private:
    HANDLE mutex_;
};

// --- HTTP ----------------------------------------------------------------------------

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::string winError(const char* what) {
    const DWORD code = GetLastError();
    std::string text = what;
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: text += ": sin conexion a Internet (no se encuentra el servidor)"; break;
        case ERROR_WINHTTP_TIMEOUT: text += ": el servidor no responde (tiempo agotado)"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: text += ": no se pudo conectar con el servidor"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: text += ": fallo la conexion segura (HTTPS)"; break;
        default: text += " (error " + std::to_string(code) + ")"; break;
    }
    return text;
}

using Sink = std::function<bool(const char* data, std::size_t size)>;

// Descarga `url` pasando los datos a `sink`. `total` = tamano si se conoce.
bool transfer(const std::string& url, const Sink& sink, const std::function<void(std::uint64_t)>& on_total,
              std::string* error) {
    if (const fs::path local = localPath(url); !local.empty()) {
        std::ifstream in(local, std::ios::binary);
        if (!in) {
            setError(error, "No se encuentra " + narrow(local.wstring()));
            return false;
        }
        std::error_code ec;
        if (on_total) on_total(static_cast<std::uint64_t>(fs::file_size(local, ec)));
        std::vector<char> buffer(1 << 16);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got > 0 && !sink(buffer.data(), static_cast<std::size_t>(got))) {
                setError(error, "Cancelado");
                return false;
            }
        }
        return true;
    }

    const std::wstring wide_url = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        setError(error, "Direccion no valida: " + url);
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring agent = L"CramionUpdater/" + widen(currentVersion().str());
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
#else
    Handle session;
#endif
    if (!session) {
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        setError(error, winError("No se pudo iniciar WinHTTP"));
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 15000, 20000, 60000);
    // TLS 1.2 y 1.3 (Windows 10 no activa la 1.3 por defecto).
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection) {
        setError(error, winError("No se pudo conectar"));
        return false;
    }
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        setError(error, winError("No se pudo crear la peticion"));
        return false;
    }
    std::wstring headers;
    if (lowerWide(host) == L"api.github.com") {
        headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    }
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        setError(error, winError("No se pudo descargar"));
        return false;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        std::string text = "El servidor respondio " + std::to_string(status);
        if (status == 404) text += " (no encontrado)";
        if (status == 403 || status == 429) text += ": demasiadas consultas a GitHub, prueba dentro de un rato";
        setError(error, text);
        return false;
    }
    if (on_total) {
        wchar_t length[32] = {};
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                                &length_size, WINHTTP_NO_HEADER_INDEX)) {
            on_total(std::wcstoull(length, nullptr, 10));
        }
    }
    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            setError(error, winError("Se corto la descarga"));
            return false;
        }
        if (available == 0) break;
        while (available > 0) {
            DWORD read = 0;
            const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!WinHttpReadData(request.h, buffer.data(), chunk, &read)) {
                setError(error, winError("Se corto la descarga"));
                return false;
            }
            if (read == 0) break;
            if (!sink(buffer.data(), read)) {
                setError(error, "Cancelado");
                return false;
            }
            available -= read;
        }
    }
    return true;
}

// --- Zip -------------------------------------------------------------------------------

std::uint32_t crc32(const unsigned char* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const unsigned char* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

struct ZipEntry {
    std::string name;  // con '/'
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint64_t local_offset = 0;
    bool directory = false;
};

bool readZipDirectory(const std::vector<unsigned char>& zip, std::vector<ZipEntry>& entries, std::string* error) {
    const std::size_t n = zip.size();
    if (n < 22) {
        setError(error, "El zip esta vacio o roto");
        return false;
    }
    // Fin del directorio central: se busca hacia atras (puede haber comentario).
    std::size_t eocd = std::string::npos;
    const std::size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (std::size_t i = n - 22 + 1; i-- > lowest;) {
        if (le32(&zip[i]) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        setError(error, "No es un zip valido (falta el directorio central)");
        return false;
    }
    std::uint64_t count = le16(&zip[eocd + 10]);
    std::uint64_t cd_offset = le32(&zip[eocd + 16]);
    // Zip64: el localizador esta justo antes.
    if ((count == 0xFFFFu || cd_offset == 0xFFFFFFFFu) && eocd >= 20 && le32(&zip[eocd - 20]) == 0x07064b50u) {
        const std::uint64_t z64 = le64(&zip[eocd - 20 + 8]);
        if (z64 + 56 <= n && le32(&zip[z64]) == 0x06064b50u) {
            count = le64(&zip[z64 + 32]);
            cd_offset = le64(&zip[z64 + 48]);
        }
    }
    std::size_t p = static_cast<std::size_t>(cd_offset);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (p + 46 > n || le32(&zip[p]) != 0x02014b50u) {
            setError(error, "El directorio del zip esta roto");
            return false;
        }
        ZipEntry e;
        e.method = le16(&zip[p + 10]);
        e.crc = le32(&zip[p + 16]);
        e.compressed = le32(&zip[p + 20]);
        e.size = le32(&zip[p + 24]);
        const std::uint16_t name_len = le16(&zip[p + 28]);
        const std::uint16_t extra_len = le16(&zip[p + 30]);
        const std::uint16_t comment_len = le16(&zip[p + 32]);
        e.local_offset = le32(&zip[p + 42]);
        if (p + 46 + name_len + extra_len > n) {
            setError(error, "El directorio del zip esta roto");
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(&zip[p + 46]), name_len);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        // Campo extra Zip64 (0x0001): solo los valores que valen 0xFFFFFFFF.
        std::size_t x = p + 46 + name_len;
        const std::size_t x_end = x + extra_len;
        while (x + 4 <= x_end) {
            const std::uint16_t id = le16(&zip[x]);
            const std::uint16_t len = le16(&zip[x + 2]);
            std::size_t q = x + 4;
            if (id == 0x0001u) {
                if (e.size == 0xFFFFFFFFu && q + 8 <= x_end) e.size = le64(&zip[q]), q += 8;
                if (e.compressed == 0xFFFFFFFFu && q + 8 <= x_end) e.compressed = le64(&zip[q]), q += 8;
                if (e.local_offset == 0xFFFFFFFFu && q + 8 <= x_end) e.local_offset = le64(&zip[q]);
            }
            x += 4u + len;
        }
        e.directory = !e.name.empty() && e.name.back() == '/';
        entries.push_back(std::move(e));
        p += 46u + name_len + extra_len + comment_len;
    }
    return true;
}

// Ruta segura dentro del destino (sin .., sin unidad ni raiz).
bool safeRelative(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

fs::path cleanupListFile() { return dataFolder() / "cleanup.txt"; }

}  // namespace

// --- Versiones -------------------------------------------------------------------------

Version Version::parse(std::string_view text) {
    Version v;
    std::size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    int* fields[3] = {&v.major, &v.minor, &v.patch};
    int parsed = 0;
    while (parsed < 3 && i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        int value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            if (value > 1000000) value = 1000000;
            ++i;
        }
        *fields[parsed++] = value;
        if (i < text.size() && text[i] == '.' && i + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            ++i;
        } else {
            break;
        }
    }
    v.valid = parsed >= 2;
    return v;
}

std::string Version::str() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

Version currentVersion() {
    static const Version v = Version::parse(CRAMION_VERSION_STRING);
    return v;
}

// --- Releases --------------------------------------------------------------------------

std::string feedUrl() {
    wchar_t env[2048] = {};
    if (GetEnvironmentVariableW(L"CRAMION_UPDATE_FEED", env, 2048) > 0) return narrow(env);
    const Settings s = loadSettings();
    return s.feed.empty() ? std::string(kDefaultFeed) : s.feed;
}

bool parseRelease(const std::string& text, Release& out, std::string* error, const std::string& feed_url) {
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        setError(error, "La respuesta del servidor no es JSON");
        return false;
    }
    // Lista de releases: la version mas alta que no sea borrador ni previa.
    if (doc.is_array()) {
        json best;
        Version best_version;
        for (const json& r : doc) {
            if (!r.is_object() || r.value("draft", false) || r.value("prerelease", false)) continue;
            const Version v = Version::parse(r.value("tag_name", r.value("version", std::string{})));
            if (v.valid && (!best_version.valid || v > best_version)) {
                best = r;
                best_version = v;
            }
        }
        if (best.is_null()) {
            setError(error, "No hay ninguna version publicada");
            return false;
        }
        doc = best;
    }
    if (!doc.is_object()) {
        setError(error, "Respuesta inesperada del servidor");
        return false;
    }
    if (doc.contains("message") && !doc.contains("tag_name")) {
        setError(error, "GitHub: " + doc.value("message", std::string{}));
        return false;
    }
    Release r;
    r.tag = doc.value("tag_name", doc.value("version", std::string{}));
    r.version = Version::parse(r.tag);
    if (!r.version.valid) {
        setError(error, "La version publicada no tiene numero (" + r.tag + ")");
        return false;
    }
    r.name = doc.value("name", std::string{});
    if (r.name.empty()) r.name = "Cramion " + r.version.str();
    const json& body = doc.contains("body") ? doc["body"] : doc.contains("notes") ? doc["notes"] : json();
    if (body.is_string()) r.notes = body.get<std::string>();
    r.page_url = doc.value("html_url", std::string(kReleasesPage));
    r.published_at = doc.value("published_at", std::string{});
    if (doc.contains("assets") && doc["assets"].is_array()) {
        const json* pick = nullptr;
        for (const json& a : doc["assets"]) {
            const std::string name = a.value("name", std::string{});
            if (name == kPackageAsset) {
                pick = &a;
                break;
            }
            const std::string low = lowerAscii(name);
            if (pick == nullptr && low.size() > 4 && low.ends_with(".zip") && low.find("win64") != std::string::npos) pick = &a;
        }
        if (pick != nullptr) {
            r.zip_url = pick->value("browser_download_url", pick->value("url", std::string{}));
            r.zip_size = pick->value("size", std::uint64_t{0});
        }
    } else if (doc.contains("zip_url")) {
        r.zip_url = doc.value("zip_url", std::string{});
        r.zip_size = doc.value("zip_size", std::uint64_t{0});
    }
    // Zip con ruta relativa (feeds locales de prueba): junto al feed.
    if (!r.zip_url.empty() && r.zip_url.find("://") == std::string::npos && !fs::path(widen(r.zip_url)).is_absolute() &&
        !feed_url.empty()) {
        if (const fs::path feed_file = localPath(feed_url); !feed_file.empty()) {
            r.zip_url = narrow((feed_file.parent_path() / widen(r.zip_url)).wstring());
        } else if (const std::size_t slash = feed_url.rfind('/'); slash != std::string::npos) {
            r.zip_url = feed_url.substr(0, slash + 1) + r.zip_url;
        }
    }
    out = std::move(r);
    return true;
}

bool fetchLatest(const std::string& feed, Release& out, std::string* error) {
    std::string body;
    if (!httpGet(feed, body, error)) return false;
    return parseRelease(body, out, error, feed);
}

std::vector<NoteLine> parseNotes(const std::string& markdown) {
    std::vector<NoteLine> lines;
    std::istringstream in(markdown);
    std::string raw;
    const auto clean = [](std::string s) {
        // [texto](url) -> texto
        for (std::size_t open = s.find('['); open != std::string::npos; open = s.find('[', open + 1)) {
            const std::size_t close = s.find("](", open);
            const std::size_t end = close == std::string::npos ? close : s.find(')', close);
            if (close == std::string::npos || end == std::string::npos) break;
            s = s.substr(0, open) + s.substr(open + 1, close - open - 1) + s.substr(end + 1);
        }
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if ((s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') || (s[i] == '_' && i + 1 < s.size() && s[i + 1] == '_')) {
                ++i;
                continue;
            }
            if (s[i] == '`' || s[i] == '*') continue;  // *cursiva* tambien
            out += s[i];
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        return out;
    };
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::size_t indent = 0;
        while (indent < raw.size() && (raw[indent] == ' ' || raw[indent] == '\t')) ++indent;
        const std::string body = raw.substr(indent);
        NoteLine line;
        if (body.empty()) {
            if (lines.empty() || lines.back().kind == NoteLine::Kind::Blank) continue;
            line.kind = NoteLine::Kind::Blank;
        } else if (body[0] == '#') {
            std::size_t level = 0;
            while (level < body.size() && body[level] == '#') ++level;
            line.kind = NoteLine::Kind::Heading;
            line.level = static_cast<int>(level);
            line.text = clean(body.substr(std::min(body.size(), level + (level < body.size() && body[level] == ' ' ? 1 : 0))));
        } else if (body.size() >= 2 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') {
            line.kind = NoteLine::Kind::Bullet;
            line.level = static_cast<int>(indent / 2);
            line.text = clean(body.substr(2));
        } else {
            line.kind = NoteLine::Kind::Text;
            line.text = clean(body);
        }
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().kind == NoteLine::Kind::Blank) lines.pop_back();
    return lines;
}

std::string formatDate(const std::string& iso) {
    if (iso.size() < 10) return iso;
    return iso.substr(8, 2) + "/" + iso.substr(5, 2) + "/" + iso.substr(0, 4);
}

std::string formatBytes(std::uint64_t bytes) {
    char text[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    }
    std::string s = text;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// --- Descargas -------------------------------------------------------------------------

bool httpGet(const std::string& url, std::string& body, std::string* error) {
    body.clear();
    return transfer(
        url,
        [&](const char* data, std::size_t size) {
            body.append(data, size);
            return body.size() < 64u * 1024u * 1024u;
        },
        {}, error);
}

bool downloadFile(const std::string& url, const fs::path& destination, const Progress& progress, std::string* error) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    const fs::path part = destination.wstring() + L".part";
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    bool ok = false;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            setError(error, "No se puede escribir en " + narrow(part.parent_path().wstring()));
            return false;
        }
        ok = transfer(
            url,
            [&](const char* data, std::size_t size) {
                out.write(data, static_cast<std::streamsize>(size));
                if (!out) return false;
                done += size;
                return !progress || progress(done, total);
            },
            [&](std::uint64_t t) {
                total = t;
                if (progress) progress(0, total);
            },
            error);
        if (ok && !out) {
            setError(error, "No hay espacio en el disco para la descarga");
            ok = false;
        }
    }
    if (ok && total > 0 && done != total) {
        setError(error, "La descarga llego incompleta (" + formatBytes(done) + " de " + formatBytes(total) + ")");
        ok = false;
    }
    if (!ok) {
        fs::remove(part, ec);
        return false;
    }
    fs::remove(destination, ec);
    fs::rename(part, destination, ec);
    if (ec) {
        setError(error, "No se pudo guardar la descarga: " + errorText(ec));
        return false;
    }
    return true;
}

// --- Zip e instalacion -----------------------------------------------------------------

bool extractZip(const fs::path& zip_path, const fs::path& destination, const Progress& progress, std::string* error) {
    std::vector<unsigned char> zip;
    {
        std::ifstream in(zip_path, std::ios::binary);
        if (!in) {
            setError(error, "No se puede abrir " + narrow(zip_path.wstring()));
            return false;
        }
        in.seekg(0, std::ios::end);
        zip.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
        if (!in) {
            setError(error, "No se pudo leer el zip");
            return false;
        }
    }
    std::vector<ZipEntry> entries;
    if (!readZipDirectory(zip, entries, error)) return false;

    // Carpeta comun de primer nivel (Cramion-0.6.2-win64/): se quita.
    std::string prefix;
    {
        bool first = true;
        for (const ZipEntry& e : entries) {
            const std::size_t slash = e.name.find('/');
            const std::string top = slash == std::string::npos ? std::string{} : e.name.substr(0, slash + 1);
            if (first) {
                prefix = top;
                first = false;
            } else if (top != prefix) {
                prefix.clear();
                break;
            }
            if (top.empty()) {
                prefix.clear();
                break;
            }
        }
    }

    std::error_code ec;
    fs::remove_all(destination, ec);
    fs::create_directories(destination, ec);
    if (ec) {
        setError(error, "No se puede crear " + narrow(destination.wstring()));
        return false;
    }
    std::uint64_t total = 0;
    for (const ZipEntry& e : entries) total += e.size;
    std::uint64_t done = 0;
    std::vector<unsigned char> data;
    for (const ZipEntry& e : entries) {
        const std::string name = e.name.substr(prefix.size());
        if (name.empty()) continue;
        if (!safeRelative(name)) {
            setError(error, "El zip tiene una ruta no permitida: " + e.name);
            return false;
        }
        const fs::path target = destination / fs::path(widen(name));
        if (e.directory) {
            fs::create_directories(target, ec);
            continue;
        }
        const std::size_t local = static_cast<std::size_t>(e.local_offset);
        if (local + 30 > zip.size() || le32(&zip[local]) != 0x04034b50u) {
            setError(error, "El zip esta roto (" + e.name + ")");
            return false;
        }
        const std::size_t start = local + 30u + le16(&zip[local + 26]) + le16(&zip[local + 28]);
        if (start + e.compressed > zip.size()) {
            setError(error, "El zip esta incompleto (" + e.name + ")");
            return false;
        }
        data.resize(static_cast<std::size_t>(e.size));
        if (e.method == 0) {
            if (e.compressed != e.size) {
                setError(error, "El zip esta roto (" + e.name + ")");
                return false;
            }
            if (e.size > 0) std::memcpy(data.data(), &zip[start], static_cast<std::size_t>(e.size));
        } else if (e.method == 8) {
            if (!detail::inflateRaw(&zip[start], static_cast<std::size_t>(e.compressed), data.data(), data.size())) {
                setError(error, "No se pudo descomprimir " + e.name);
                return false;
            }
        } else {
            setError(error, "Compresion no soportada en " + e.name + " (metodo " + std::to_string(e.method) + ")");
            return false;
        }
        if (crc32(data.data(), data.size()) != e.crc) {
            setError(error, "El archivo " + e.name + " llego danado (CRC). Vuelve a descargar.");
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            setError(error, "No se pudo escribir " + narrow(target.wstring()));
            return false;
        }
        done += e.size;
        if (progress && !progress(done, total)) {
            setError(error, "Cancelado");
            return false;
        }
    }
    return true;
}

bool looksLikeEnginePackage(const fs::path& folder) {
    std::error_code ec;
    return fs::exists(folder / kEditorExe, ec) && fs::exists(folder / "shaders", ec);
}

bool installFiles(const fs::path& staged, const fs::path& install_dir, const Progress& progress, std::string* error) {
    struct Step {
        fs::path target;
        fs::path old;  // vacio si no existia
        bool copied = false;
    };
    std::vector<fs::path> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        files.push_back(fs::relative(it->path(), staged, ec));
        total += it->file_size(ec);
    }
    if (ec || files.empty()) {
        setError(error, "La carpeta descomprimida esta vacia");
        return false;
    }
    std::vector<Step> steps;
    const auto rollback = [&] {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            std::error_code e;
            if (it->copied) fs::remove(it->target, e);
            if (!it->old.empty()) fs::rename(it->old, it->target, e);
        }
    };
    std::uint64_t done = 0;
    for (const fs::path& rel : files) {
        const fs::path source = staged / rel;
        const fs::path target = install_dir / rel;
        fs::create_directories(target.parent_path(), ec);
        Step step;
        step.target = target;
        if (fs::exists(target, ec)) {
            // Un nombre libre para el viejo (el de una instalacion anterior
            // puede seguir en uso).
            fs::path old = target.wstring() + kOldSuffix;
            for (int i = 2; fs::exists(old, ec); ++i) {
                std::error_code e;
                if (fs::remove(old, e)) break;
                old = target.wstring() + kOldSuffix + std::to_wstring(i);
            }
            fs::rename(target, old, ec);
            if (ec) {
                rollback();
                setError(error, "No se puede reemplazar " + narrow(rel.wstring()) +
                                    ": esta en uso o sin permiso (" + errorText(ec) + ")");
                return false;
            }
            step.old = old;
        }
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        step.copied = !ec;
        steps.push_back(step);
        if (ec) {
            rollback();
            setError(error, "No se pudo copiar " + narrow(rel.wstring()) + ": " + errorText(ec));
            return false;
        }
        done += fs::file_size(source, ec);
        if (progress && !progress(done, total)) {
            rollback();
            setError(error, "Cancelado");
            return false;
        }
    }
    // Los viejos se borran ahora si se puede; los que siguen en uso (el propio
    // actualizador y sus .dll), al volver a abrir el motor.
    std::string pending = readTextFile(cleanupListFile());
    for (const Step& s : steps) {
        if (s.old.empty()) continue;
        std::error_code e;
        if (!fs::remove(s.old, e)) pending += narrow(s.old.wstring()) + "\n";
    }
    writeTextFile(cleanupListFile(), pending);
    return true;
}

int cleanupOldFiles() {
    NamedLock lock(L"CramionUpdateCleanup");
    const std::string list = readTextFile(cleanupListFile());
    if (list.empty()) return 0;
    std::istringstream in(list);
    std::string line;
    std::string remaining;
    int left = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const fs::path path(widen(line));
        // Solo lo que puso el actualizador.
        if (path.filename().wstring().find(kOldSuffix) == std::wstring::npos) continue;
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;
        if (!fs::remove(path, ec)) {
            remaining += line + "\n";
            ++left;
        }
    }
    std::error_code ec;
    if (remaining.empty()) {
        fs::remove(cleanupListFile(), ec);
    } else {
        writeTextFile(cleanupListFile(), remaining);
    }
    return left;
}

// --- Ajustes y carpetas ----------------------------------------------------------------

fs::path dataFolder() {
    wchar_t env[1024] = {};
    if (GetEnvironmentVariableW(L"CRAMION_UPDATE_DATA", env, 1024) > 0) return fs::path(env);
    PWSTR local = nullptr;
    fs::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) folder = fs::path(local) / "Cramion";
    CoTaskMemFree(local);
    if (folder.empty()) folder = fs::temp_directory_path() / "Cramion";
    return folder;
}

fs::path downloadsFolder() { return dataFolder() / "Updates"; }

fs::path installFolder() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
}

Settings loadSettings() {
    Settings s;
    const json doc = json::parse(readTextFile(dataFolder() / "update.json"), nullptr, false);
    if (!doc.is_object()) return s;
    s.check_on_startup = doc.value("check_on_startup", true);
    s.skipped_version = doc.value("skipped_version", std::string{});
    s.feed = doc.value("feed", std::string{});
    s.last_check = doc.value("last_check", std::int64_t{0});
    s.last_seen_version = doc.value("last_seen_version", std::string{});
    return s;
}

void saveSettings(const Settings& s) {
    json doc;
    doc["check_on_startup"] = s.check_on_startup;
    doc["skipped_version"] = s.skipped_version;
    doc["feed"] = s.feed;
    doc["last_check"] = s.last_check;
    doc["last_seen_version"] = s.last_seen_version;
    writeTextFile(dataFolder() / "update.json", doc.dump(2));
}

namespace {

json readReopen() {
    json doc = json::parse(readTextFile(dataFolder() / "reopen.json"), nullptr, false);
    if (!doc.is_object()) doc = json::object();
    if (!doc.contains("projects") || !doc["projects"].is_array()) doc["projects"] = json::array();
    return doc;
}

}  // namespace

void addReopenProject(const fs::path& project) {
    NamedLock lock(L"CramionReopenList");
    json doc = readReopen();
    const std::string path = narrow(project.wstring());
    for (const json& p : doc["projects"]) {
        if (p.is_string() && p.get<std::string>() == path) return;
    }
    doc["projects"].push_back(path);
    writeTextFile(dataFolder() / "reopen.json", doc.dump(2));
}

void markReopenHub() {
    NamedLock lock(L"CramionReopenList");
    json doc = readReopen();
    doc["hub"] = true;
    writeTextFile(dataFolder() / "reopen.json", doc.dump(2));
}

ReopenList takeReopenList() {
    NamedLock lock(L"CramionReopenList");
    ReopenList list;
    const json doc = readReopen();
    list.hub = doc.value("hub", false);
    for (const json& p : doc["projects"]) {
        if (p.is_string()) list.projects.emplace_back(widen(p.get<std::string>()));
    }
    std::error_code ec;
    fs::remove(dataFolder() / "reopen.json", ec);
    return list;
}

// --- Procesos --------------------------------------------------------------------------

std::vector<RunningApp> runningEngineApps(const fs::path& install_dir, unsigned long exclude_pid) {
    std::vector<RunningApp> apps;
    std::error_code ec;
    const std::wstring folder = lowerWide(fs::weakly_canonical(install_dir, ec).wstring());
    static const std::set<std::wstring> kApps = {L"cramioneditor.exe", L"cramionplayer.exe", L"cramion.exe"};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return apps;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == exclude_pid) continue;
        const std::wstring exe = lowerWide(entry.szExeFile);
        if (!kApps.contains(exe)) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = MAX_PATH * 2;
        const BOOL got = QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
        if (!got) continue;
        const std::wstring dir = lowerWide(fs::weakly_canonical(fs::path(path).parent_path(), ec).wstring());
        if (dir != folder) continue;
        apps.push_back({entry.th32ProcessID, entry.szExeFile, exe == L"cramioneditor.exe"});
    }
    CloseHandle(snapshot);
    return apps;
}

int askAppsToSaveAndClose(const std::vector<RunningApp>& apps) {
    struct Context {
        const std::vector<RunningApp>* apps;
        UINT message;
        int sent;
    } context{&apps, RegisterWindowMessageW(kPrepareMessage), 0};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Context*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            for (const RunningApp& app : *c->apps) {
                if (app.pid != pid) continue;
                // El editor guarda todo; el resto (juegos, demo) se cierra.
                if (PostMessageW(hwnd, app.editor ? c->message : WM_CLOSE, 0, 0)) ++c->sent;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return context.sent;
}

std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    int backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(static_cast<std::size_t>(backslashes * 2 + 1), L'\\');
        } else {
            out.append(static_cast<std::size_t>(backslashes), L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(static_cast<std::size_t>(backslashes * 2), L'\\');
    out += L'"';
    return out;
}

unsigned long launch(const fs::path& exe, const std::wstring& arguments, const fs::path& working_dir) {
    std::wstring command = quoteArgument(exe.wstring());
    if (!arguments.empty()) command += L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const fs::path cwd = working_dir.empty() ? exe.parent_path() : working_dir;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &startup, &info)) {
        return 0;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return info.dwProcessId;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool openUrl(const std::string& url) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

// --- Busqueda en segundo plano ---------------------------------------------------------

std::shared_ptr<CheckJob> startCheck(const std::string& feed) {
    auto job = std::make_shared<CheckJob>();
    const std::string url = feed.empty() ? feedUrl() : feed;
    std::thread([job, url] {
        Release release;
        std::string error;
        if (fetchLatest(url, release, &error)) {
            Settings s = loadSettings();
            s.last_check = static_cast<std::int64_t>(std::time(nullptr));
            s.last_seen_version = release.version.str();
            saveSettings(s);
            job->release = std::move(release);
            job->state = CheckJob::State::Done;
        } else {
            job->error = error;
            job->state = CheckJob::State::Failed;
        }
    }).detach();
    return job;
}

}  // namespace cramion::update
