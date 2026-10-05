#include "AndroidBuild.h"

#include <CramionCore/net/Http.h>
#include <CramionFX/asset/ImageFile.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace cramion::editor {

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    std::string result;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) result = value;
    std::free(value);
    return result;
}

bool fileExists(const fs::path& p) {
    std::error_code e;
    return fs::exists(p, e);
}

// "36.1.0" > "35.0.0": compara por numeros.
std::vector<int> versionNumbers(const std::string& text) {
    std::vector<int> out;
    int value = -1;
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            value = (value < 0 ? 0 : value * 10) + (c - '0');
        } else if (value >= 0) {
            out.push_back(value);
            value = -1;
        }
    }
    if (value >= 0) out.push_back(value);
    return out;
}

// Un argumento para la linea de comandos de Windows (comillas y barras).
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    int slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(static_cast<std::size_t>(slashes * 2 + 1), L'\\');
        else out.append(static_cast<std::size_t>(slashes), L'\\');
        slashes = 0;
        out += c;
    }
    out.append(static_cast<std::size_t>(slashes * 2), L'\\');
    out += L'"';
    return out;
}

// Ejecuta un programa sin ventana y junta su salida. Los .bat (apksigner,
// d8) van por cmd.exe. Devuelve el codigo de salida (-1 si no arranco).
int run(const fs::path& program, const std::vector<std::string>& args, std::string& output,
        const std::atomic<bool>* cancel = nullptr, const fs::path& folder = {}) {
    std::wstring command;
    std::string ext = utf8(program.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool batch = ext == ".bat" || ext == ".cmd";
    if (batch) command = L"cmd.exe /d /s /c \"";
    command += quote(program.wstring());
    for (const std::string& a : args) command += L" " + quote(widen(a));
    if (batch) command += L"\"";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_command = command;
    const std::wstring dir = folder.wstring();
    const BOOL started = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(write_pipe);
    if (!started) {
        CloseHandle(read_pipe);
        output += "No se pudo ejecutar " + utf8(program) + "\n";
        return -1;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) break;  // el programa cerro la tuberia
        if (available > 0) {
            DWORD got = 0;
            if (!ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || got == 0) break;
            output.append(buffer.data(), got);
            continue;
        }
        if (cancel != nullptr && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            // Lo que quede en la tuberia.
            DWORD got = 0;
            while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0 &&
                   ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got > 0) {
                output.append(buffer.data(), got);
            }
            break;
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_pipe);
    return static_cast<int>(code);
}

std::string xmlEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Un texto de recurso de Android (strings.xml): ademas de XML, ' " \ y @/? al principio.
std::string resourceString(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' || c == '\'' || c == '"') out += '\\';
        if (i == 0 && (c == '@' || c == '?')) out += '\\';
        out += c;
    }
    return xmlEscape(out);
}

// --- ZIP (solo lo que hace falta: copiar entradas tal cual y anadir sin comprimir) ---

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;  // 0 guardado, 8 deflate
    std::uint16_t flags = 0;
    std::uint16_t time = 0;
    std::uint16_t date = 0x21;  // 1980-01-01
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;  // en el archivo de salida
};

template <typename T>
void put(std::ostream& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) out.put(static_cast<char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF));
}

template <typename T>
T get(const std::vector<std::uint8_t>& b, std::size_t at) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(b[at + i]) << (8 * i));
    return v;
}

class ZipWriter {
public:
    bool open(const fs::path& file) {
        out_.open(file, std::ios::binary | std::ios::trunc);
        return static_cast<bool>(out_);
    }

    // Copia las entradas de otro zip (comprimidas tal cual). `rename`
    // devuelve el nombre nuevo o "" para saltarla.
    bool copyFrom(const fs::path& file, const std::function<std::string(const std::string&)>& rename, std::string* error) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return fail(error, "no se pudo abrir " + utf8(file));
        in.seekg(0, std::ios::end);
        const std::uint64_t length = static_cast<std::uint64_t>(in.tellg());
        const std::uint64_t tail_size = std::min<std::uint64_t>(length, 65557);
        std::vector<std::uint8_t> tail(tail_size);
        in.seekg(static_cast<std::streamoff>(length - tail_size));
        in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail_size));
        std::size_t eocd = std::string::npos;
        for (std::size_t i = tail_size >= 22 ? tail_size - 22 : 0;; --i) {
            if (get<std::uint32_t>(tail, i) == 0x06054b50u) {
                eocd = i;
                break;
            }
            if (i == 0) break;
        }
        if (eocd == std::string::npos) return fail(error, utf8(file) + " no es un zip");
        const std::uint16_t count = get<std::uint16_t>(tail, eocd + 10);
        const std::uint32_t cd_size = get<std::uint32_t>(tail, eocd + 12);
        const std::uint32_t cd_offset = get<std::uint32_t>(tail, eocd + 16);
        std::vector<std::uint8_t> cd(cd_size);
        in.seekg(cd_offset);
        in.read(reinterpret_cast<char*>(cd.data()), cd_size);
        std::size_t at = 0;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (at + 46 > cd.size() || get<std::uint32_t>(cd, at) != 0x02014b50u) return fail(error, "zip danado: " + utf8(file));
            ZipEntry e;
            e.flags = get<std::uint16_t>(cd, at + 8) & static_cast<std::uint16_t>(~0x0008u);
            e.method = get<std::uint16_t>(cd, at + 10);
            e.time = get<std::uint16_t>(cd, at + 12);
            e.date = get<std::uint16_t>(cd, at + 14);
            e.crc = get<std::uint32_t>(cd, at + 16);
            e.compressed = get<std::uint32_t>(cd, at + 20);
            e.size = get<std::uint32_t>(cd, at + 24);
            const std::uint16_t name_length = get<std::uint16_t>(cd, at + 28);
            const std::uint16_t extra_length = get<std::uint16_t>(cd, at + 30);
            const std::uint16_t comment_length = get<std::uint16_t>(cd, at + 32);
            const std::uint32_t local = get<std::uint32_t>(cd, at + 42);
            const std::string name(reinterpret_cast<const char*>(cd.data() + at + 46), name_length);
            at += 46u + name_length + extra_length + comment_length;
            const std::string renamed = rename ? rename(name) : name;
            if (renamed.empty()) continue;
            // Donde empiezan los datos: tras la cabecera local (su extra puede ser otro).
            std::vector<std::uint8_t> header(30);
            in.seekg(local);
            in.read(reinterpret_cast<char*>(header.data()), 30);
            const std::uint64_t data = local + 30u + get<std::uint16_t>(header, 26) + get<std::uint16_t>(header, 28);
            e.name = renamed;
            writeLocal(e);
            in.seekg(static_cast<std::streamoff>(data));
            copyBytes(in, e.compressed);
            entries_.push_back(e);
        }
        return static_cast<bool>(out_) || fail(error, "no se pudo escribir el zip");
    }

    // Un archivo del disco, sin comprimir.
    bool addFile(const std::string& name, const fs::path& file, std::string* error) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return fail(error, "no se pudo leer " + utf8(file));
        std::error_code ec;
        const std::uint64_t size = fs::file_size(file, ec);
        if (ec || size >= 0xFFFFFFFFull) return fail(error, utf8(file) + " es demasiado grande para el APK (4 GB): usa el OBB aparte");
        // El CRC va en la cabecera local: primero se calcula.
        std::uint32_t crc = 0;
        std::vector<char> buffer(1u << 20);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize n = in.gcount();
            if (n <= 0) break;
            crc = crc32(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(n), crc);
        }
        in.clear();
        in.seekg(0);
        ZipEntry e;
        e.name = name;
        e.crc = crc;
        e.compressed = e.size = size;
        writeLocal(e);
        copyBytes(in, size);
        entries_.push_back(e);
        return static_cast<bool>(out_) || fail(error, "no se pudo escribir el zip (disco lleno?)");
    }

    bool addText(const std::string& name, const std::string& text, std::string* error) {
        ZipEntry e;
        e.name = name;
        e.crc = crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        e.compressed = e.size = text.size();
        writeLocal(e);
        out_.write(text.data(), static_cast<std::streamsize>(text.size()));
        entries_.push_back(e);
        return static_cast<bool>(out_) || fail(error, "no se pudo escribir el zip");
    }

    bool close(std::string* error) {
        const std::uint64_t cd_offset = static_cast<std::uint64_t>(out_.tellp());
        if (cd_offset >= 0xFFFFFFFFull || entries_.size() >= 0xFFFF) {
            return fail(error, "el paquete pasa de 4 GB: usa el OBB aparte");
        }
        for (const ZipEntry& e : entries_) {
            put<std::uint32_t>(out_, 0x02014b50u);
            put<std::uint16_t>(out_, 20);  // hecho por
            put<std::uint16_t>(out_, 20);  // version necesaria
            put<std::uint16_t>(out_, e.flags);
            put<std::uint16_t>(out_, e.method);
            put<std::uint16_t>(out_, e.time);
            put<std::uint16_t>(out_, e.date);
            put<std::uint32_t>(out_, e.crc);
            put<std::uint32_t>(out_, static_cast<std::uint32_t>(e.compressed));
            put<std::uint32_t>(out_, static_cast<std::uint32_t>(e.size));
            put<std::uint16_t>(out_, static_cast<std::uint16_t>(e.name.size()));
            put<std::uint16_t>(out_, 0);
            put<std::uint16_t>(out_, 0);
            put<std::uint16_t>(out_, 0);
            put<std::uint16_t>(out_, 0);
            put<std::uint32_t>(out_, 0);
            put<std::uint32_t>(out_, static_cast<std::uint32_t>(e.offset));
            out_.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
        }
        const std::uint64_t cd_end = static_cast<std::uint64_t>(out_.tellp());
        put<std::uint32_t>(out_, 0x06054b50u);
        put<std::uint16_t>(out_, 0);
        put<std::uint16_t>(out_, 0);
        put<std::uint16_t>(out_, static_cast<std::uint16_t>(entries_.size()));
        put<std::uint16_t>(out_, static_cast<std::uint16_t>(entries_.size()));
        put<std::uint32_t>(out_, static_cast<std::uint32_t>(cd_end - cd_offset));
        put<std::uint32_t>(out_, static_cast<std::uint32_t>(cd_offset));
        put<std::uint16_t>(out_, 0);
        out_.close();
        return !out_.fail() || fail(error, "no se pudo terminar el zip");
    }

private:
    static bool fail(std::string* error, const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    }

    void writeLocal(ZipEntry& e) {
        e.offset = static_cast<std::uint64_t>(out_.tellp());
        put<std::uint32_t>(out_, 0x04034b50u);
        put<std::uint16_t>(out_, 20);
        put<std::uint16_t>(out_, e.flags);
        put<std::uint16_t>(out_, e.method);
        put<std::uint16_t>(out_, e.time);
        put<std::uint16_t>(out_, e.date);
        put<std::uint32_t>(out_, e.crc);
        put<std::uint32_t>(out_, static_cast<std::uint32_t>(e.compressed));
        put<std::uint32_t>(out_, static_cast<std::uint32_t>(e.size));
        put<std::uint16_t>(out_, static_cast<std::uint16_t>(e.name.size()));
        put<std::uint16_t>(out_, 0);
        out_.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
    }

    void copyBytes(std::istream& in, std::uint64_t count) {
        std::vector<char> buffer(1u << 20);
        while (count > 0 && in) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(n));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            out_.write(buffer.data(), got);
            count -= static_cast<std::uint64_t>(got);
        }
    }

    std::ofstream out_;
    std::vector<ZipEntry> entries_;
};

// Icono cuadrado de `size` px: la imagen encajada (sin deformar) y centrada.
bool writeIcon(const asset::ImageRgba8& source, int size, const fs::path& file) {
    asset::ImageRgba8 icon;
    icon.width = icon.height = static_cast<std::uint32_t>(size);
    icon.pixels.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    const float scale = static_cast<float>(size) / static_cast<float>(std::max(source.width, source.height));
    const float w = static_cast<float>(source.width) * scale;
    const float h = static_cast<float>(source.height) * scale;
    const float ox = (static_cast<float>(size) - w) * 0.5f;
    const float oy = (static_cast<float>(size) - h) * 0.5f;
    // Promedio de la caja de origen de cada pixel (reduce sin dientes).
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float sx0 = (static_cast<float>(x) - ox) / scale;
            const float sy0 = (static_cast<float>(y) - oy) / scale;
            const float sx1 = sx0 + 1.0f / scale;
            const float sy1 = sy0 + 1.0f / scale;
            if (sx1 <= 0.0f || sy1 <= 0.0f || sx0 >= static_cast<float>(source.width) || sy0 >= static_cast<float>(source.height)) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(sx0)));
            const int y0 = std::max(0, static_cast<int>(std::floor(sy0)));
            const int x1 = std::min(static_cast<int>(source.width), std::max(x0 + 1, static_cast<int>(std::ceil(sx1))));
            const int y1 = std::min(static_cast<int>(source.height), std::max(y0 + 1, static_cast<int>(std::ceil(sy1))));
            double sum[4] = {0, 0, 0, 0};
            double weight = 0.0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* p = &source.pixels[(static_cast<std::size_t>(sy) * source.width + static_cast<std::size_t>(sx)) * 4];
                    const double a = p[3] / 255.0;
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    sum[3] += p[3];
                    weight += 1.0;
                }
            }
            std::uint8_t* d = &icon.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4];
            const double alpha = sum[3] / weight;
            const double k = alpha > 0.0 ? 255.0 / alpha / weight : 0.0;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp(sum[c] * k, 0.0, 255.0));
            d[3] = static_cast<std::uint8_t>(std::clamp(alpha, 0.0, 255.0));
        }
    }
    std::error_code e;
    fs::create_directories(file.parent_path(), e);
    return asset::saveImagePng(file, icon);
}

fs::path bundletoolDownloadPath() { return fromUtf8(env("LOCALAPPDATA")) / "Cramion" / "Android" / "bundletool.jar"; }

bool downloadBundletool(std::string& error) {
    net::HttpRequest request;
    request.url = "https://github.com/google/bundletool/releases/download/1.18.3/bundletool-all-1.18.3.jar";
    request.timeout_ms = 300000;
    request.max_response_bytes = 128u << 20;
    const net::HttpResponse response = net::httpRequest(request);
    if (!response.ok) {
        error = response.error.empty() ? "respuesta " + std::to_string(response.status) : response.error;
        return false;
    }
    if (response.body.size() < 1000000 || response.body.compare(0, 2, "PK") != 0) {
        error = "la descarga no es un .jar";
        return false;
    }
    const fs::path file = bundletoolDownloadPath();
    std::error_code e;
    fs::create_directories(file.parent_path(), e);
    fs::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        if (!out) {
            error = "no se pudo guardar " + utf8(file);
            return false;
        }
    }
    fs::rename(part, file, e);
    if (e) {
        error = "no se pudo guardar " + utf8(file);
        return false;
    }
    return true;
}

const char* orientationName(int orientation) {
    switch (orientation) {
        case 1: return "sensorPortrait";
        case 2: return "fullSensor";
        case 3: return "landscape";
        case 4: return "portrait";
        default: return "sensorLandscape";
    }
}

}  // namespace

bool validAndroidPackage(const std::string& package) {
    if (package.empty() || package.size() > 150) return false;
    int parts = 0;
    bool start = true;
    for (const char c : package) {
        if (c == '.') {
            if (start) return false;
            start = true;
            continue;
        }
        const bool letter = std::isalpha(static_cast<unsigned char>(c)) != 0;
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        if (start) {
            if (!letter) return false;
            ++parts;
            start = false;
        } else if (!letter && !digit && c != '_') {
            return false;
        }
    }
    return !start && parts >= 2;
}

std::string defaultAndroidPackage(const std::string& game_name) {
    std::string id;
    for (const char c : game_name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 && u < 0x80) id += static_cast<char>(std::tolower(u));
    }
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id.front())) != 0) id = "juego" + id;
    return "com.cramion." + id;
}

std::string androidManifest(const AndroidPackageInput& input) {
    std::ostringstream m;
    m << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      << "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
      << "    package=\"" << xmlEscape(input.package) << "\"\n"
      << "    android:versionCode=\"" << input.version_code << "\"\n"
      << "    android:versionName=\"" << xmlEscape(input.version_name) << "\"\n"
      << "    android:installLocation=\"auto\">\n"
      // Vulkan 1.0 o mas (0x400003 = 1.0.3, lo que recomienda Google): el
      // renderizador va en modo compatible en los moviles (VulkanCompat.h).
      << "    <uses-feature android:name=\"android.hardware.vulkan.version\" android:version=\"0x400003\" android:required=\"true\" />\n"
      << "    <uses-feature android:name=\"android.hardware.touchscreen\" android:required=\"false\" />\n"
      << "    <uses-feature android:name=\"android.hardware.gamepad\" android:required=\"false\" />\n";
    if (input.internet) {
        m << "    <uses-permission android:name=\"android.permission.INTERNET\" />\n"
          << "    <uses-permission android:name=\"android.permission.ACCESS_NETWORK_STATE\" />\n";
    }
    if (input.vibrate) m << "    <uses-permission android:name=\"android.permission.VIBRATE\" />\n";
    if (input.record_audio) m << "    <uses-permission android:name=\"android.permission.RECORD_AUDIO\" />\n";
    m << "    <application\n"
      << "        android:label=\"@string/app_name\"\n"
      << "        android:icon=\"@mipmap/ic_launcher\"\n"
      << "        android:hasCode=\"false\"\n"
      << "        android:extractNativeLibs=\"false\"\n"
      << "        android:isGame=\"true\"\n"
      << "        android:appCategory=\"game\"\n"
      << "        android:allowBackup=\"true\"\n"
      << "        android:theme=\"@android:style/Theme.Black.NoTitleBar.Fullscreen\">\n"
      << "        <activity\n"
      << "            android:name=\"android.app.NativeActivity\"\n"
      << "            android:label=\"@string/app_name\"\n"
      << "            android:exported=\"true\"\n"
      << "            android:launchMode=\"singleTask\"\n"
      << "            android:screenOrientation=\"" << orientationName(input.orientation) << "\"\n"
      << "            android:configChanges=\"orientation|screenSize|screenLayout|smallestScreenSize|keyboard|keyboardHidden|navigation|uiMode|density\">\n"
      << "            <meta-data android:name=\"android.app.lib_name\" android:value=\"main\" />\n"
      << "            <intent-filter>\n"
      << "                <action android:name=\"android.intent.action.MAIN\" />\n"
      << "                <category android:name=\"android.intent.category.LAUNCHER\" />\n"
      << "            </intent-filter>\n"
      << "        </activity>\n"
      << "    </application>\n"
      << "</manifest>\n";
    return m.str();
}

AndroidToolchain findAndroidToolchain(const fs::path& editor_folder, int target_sdk) {
    AndroidToolchain t;
    std::vector<fs::path> sdks;
    for (const char* name : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (const std::string v = env(name); !v.empty()) sdks.push_back(fromUtf8(v));
    }
    if (const std::string local = env("LOCALAPPDATA"); !local.empty()) sdks.push_back(fromUtf8(local) / "Android" / "Sdk");
    for (const fs::path& s : sdks) {
        if (fileExists(s / "platforms") && fileExists(s / "build-tools")) {
            t.sdk = s;
            break;
        }
    }
    if (t.sdk.empty()) {
        t.error = "No se encontro el Android SDK. Instala Android Studio (o las command-line tools) y abre el SDK Manager una vez; "
                  "tambien vale definir ANDROID_HOME.";
        return t;
    }
    // build-tools: la version mas nueva con aapt2, zipalign y apksigner.
    std::vector<int> best;
    std::error_code e;
    for (fs::directory_iterator it(t.sdk / "build-tools", e); !e && it != fs::directory_iterator(); it.increment(e)) {
        const fs::path dir = it->path();
        if (!fileExists(dir / "aapt2.exe") || !fileExists(dir / "zipalign.exe") || !fileExists(dir / "apksigner.bat")) continue;
        const std::vector<int> v = versionNumbers(utf8(dir.filename()));
        if (t.build_tools.empty() || v > best) {
            best = v;
            t.build_tools = dir;
        }
    }
    // android.jar: el del targetSdk o el mas nuevo que haya.
    int best_platform = 0;
    for (fs::directory_iterator it(t.sdk / "platforms", e); !e && it != fs::directory_iterator(); it.increment(e)) {
        const fs::path jar = it->path() / "android.jar";
        if (!fileExists(jar)) continue;
        const std::vector<int> v = versionNumbers(utf8(it->path().filename()));
        const int level = v.empty() ? 0 : v.front();
        const bool exact = level == target_sdk;
        if (exact || (level > best_platform && best_platform != target_sdk)) {
            best_platform = level;
            t.android_jar = jar;
            t.platform = level;
        }
    }
    if (fileExists(t.sdk / "platform-tools" / "adb.exe")) t.adb = t.sdk / "platform-tools" / "adb.exe";
    // Java: JAVA_HOME, el de Android Studio o el del PATH.
    std::vector<fs::path> javas;
    if (const std::string home = env("JAVA_HOME"); !home.empty()) javas.push_back(fromUtf8(home) / "bin");
    for (const char* studio : {"C:/Program Files/Android/Android Studio/jbr/bin", "C:/Program Files/Android/Android Studio/jre/bin"}) {
        javas.emplace_back(studio);
    }
    if (const std::string local = env("LOCALAPPDATA"); !local.empty()) {
        javas.push_back(fromUtf8(local) / "Programs" / "Android Studio" / "jbr" / "bin");
    }
    for (const fs::path& j : javas) {
        if (fileExists(j / "java.exe") && fileExists(j / "keytool.exe")) {
            t.java_bin = j;
            break;
        }
    }
    for (const fs::path& b : {editor_folder / "android" / "bundletool.jar", bundletoolDownloadPath(), t.sdk / "bundletool.jar"}) {
        if (fileExists(b)) {
            t.bundletool = b;
            break;
        }
    }
    if (t.build_tools.empty()) {
        t.error = "Faltan las build-tools del Android SDK (aapt2, zipalign, apksigner): instalalas desde el SDK Manager.";
    } else if (t.android_jar.empty()) {
        t.error = "Falta una plataforma del Android SDK (platforms/android-N): instala una desde el SDK Manager.";
    } else if (t.java_bin.empty()) {
        t.error = "No se encontro Java (keytool/apksigner lo necesitan): instala Android Studio o define JAVA_HOME.";
    }
    return t;
}

bool buildAndroidPackage(const AndroidToolchain& tools, const AndroidPackageInput& input, const AndroidProgress& progress,
                         const std::atomic<bool>* cancel, AndroidPackageResult& result, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const auto step = [&](float f, const std::string& text) {
        if (progress) progress(f, text);
        return !(cancel != nullptr && cancel->load());
    };
    if (!tools.ok()) return fail(tools.error);
    if (!validAndroidPackage(input.package)) {
        return fail("Nombre de paquete no valido: \"" + input.package +
                    "\" (como com.estudio.juego: minimo dos partes, cada una empieza por letra, solo letras, numeros y _)");
    }
    if (input.version_code < 1 || input.version_code > 2100000000) return fail("El codigo de version tiene que ser 1 o mas");
    if (input.libraries.empty()) return fail("Falta libmain.so (el juego compilado para Android) junto al editor");
    // bundletool (32 MB, solo para el AAB) no viene en el zip: se descarga
    // la primera vez en %LOCALAPPDATA%/Cramion/Android.
    AndroidToolchain local_tools = tools;
    if (input.make_aab && local_tools.bundletool.empty()) {
        if (!step(0.01f, "Descargando bundletool (una sola vez, 32 MB)")) return fail("Cancelado");
        std::string download_error;
        if (!downloadBundletool(download_error)) return fail("Para el AAB hace falta bundletool: " + download_error);
        local_tools.bundletool = bundletoolDownloadPath();
    }
    const AndroidToolchain& tools_ref = local_tools;
    // Las herramientas .bat (apksigner) buscan java en JAVA_HOME.
    SetEnvironmentVariableW(L"JAVA_HOME", tools.java_bin.parent_path().wstring().c_str());

    std::error_code ec;
    const fs::path work = input.work_folder;
    fs::remove_all(work, ec);
    fs::create_directories(work / "res" / "values", ec);
    fs::create_directories(input.output_folder, ec);
    std::string& log = result.log;
    const auto tool = [&](const fs::path& program, const std::vector<std::string>& args, const std::string& what) {
        std::string out;
        const int code = run(program, args, out, cancel, work);
        log += "> " + utf8(program.filename());
        for (const std::string& a : args) log += " " + (a.find("pass:") == 0 ? std::string("pass:***") : a);
        log += "\n" + out;
        if (code != 0) {
            if (error != nullptr) {
                *error = what + " fallo (codigo " + std::to_string(code) + ")";
                // La ultima linea con texto suele decir por que.
                std::istringstream lines(out);
                std::string line;
                std::string last;
                while (std::getline(lines, line)) {
                    if (line.find_first_not_of(" \t\r") != std::string::npos) last = line;
                }
                if (!last.empty()) *error += ": " + last;
            }
            return false;
        }
        return true;
    };

    // --- 1. Manifiesto, nombre e iconos ---
    if (!step(0.02f, "Manifiesto e iconos")) return fail("Cancelado");
    std::ofstream(work / "AndroidManifest.xml", std::ios::binary) << androidManifest(input);
    std::ofstream(work / "res" / "values" / "strings.xml", std::ios::binary)
        << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n    <string name=\"app_name\">"
        << resourceString(input.label) << "</string>\n</resources>\n";
    {
        asset::ImageRgba8 image;
        bool loaded = !input.icon.empty() && asset::loadImageRgba8(input.icon, image) && image.width > 0;
        if (!loaded && !input.fallback_icon.empty()) loaded = asset::loadImageRgba8(input.fallback_icon, image) && image.width > 0;
        if (!loaded) return fail("No se pudo leer el icono (PNG/JPG/TGA/BMP)");
        static constexpr std::array<std::pair<const char*, int>, 5> kDensities = {
            {{"mdpi", 48}, {"hdpi", 72}, {"xhdpi", 96}, {"xxhdpi", 144}, {"xxxhdpi", 192}}};
        for (const auto& [density, size] : kDensities) {
            if (!writeIcon(image, size, work / "res" / (std::string("mipmap-") + density) / "ic_launcher.png")) {
                return fail("No se pudo escribir el icono");
            }
        }
    }

    // --- 2. Recursos (aapt2) ---
    const fs::path aapt2 = tools.build_tools / "aapt2.exe";
    if (!step(0.06f, "Compilando recursos (aapt2)")) return fail("Cancelado");
    if (!tool(aapt2, {"compile", "--dir", utf8(work / "res"), "-o", utf8(work / "res.zip")}, "aapt2 compile")) return false;
    const auto link = [&](const fs::path& out, bool proto) {
        std::vector<std::string> args = {"link", "-o", utf8(out), "-I", utf8(tools.android_jar), "--manifest",
                                         utf8(work / "AndroidManifest.xml"), "--min-sdk-version", std::to_string(input.min_sdk),
                                         "--target-sdk-version", std::to_string(input.target_sdk), utf8(work / "res.zip")};
        if (proto) args.insert(args.begin() + 1, "--proto-format");
        return tool(aapt2, args, "aapt2 link");
    };

    // Lo que va en el APK/AAB ademas de los recursos.
    const bool pack_inside = !input.pack.empty() && !(input.split_obb && !input.make_aab);
    const auto add_payload = [&](ZipWriter& zip, float from, float to) {
        const std::size_t total = input.libraries.size() + input.assets.size() + (pack_inside ? 1 : 0);
        std::size_t done = 0;
        const auto tick = [&](const std::string& what) {
            ++done;
            return step(from + (to - from) * static_cast<float>(done) / static_cast<float>(std::max<std::size_t>(total, 1)), what);
        };
        for (const auto& [abi, so] : input.libraries) {
            if (!zip.addFile("lib/" + abi + "/libmain.so", so, error)) return false;
            if (!tick("libmain.so (" + abi + ")")) return fail("Cancelado");
        }
        for (const auto& [name, file] : input.assets) {
            if (!zip.addFile("assets/" + name, file, error)) return false;
            if (!tick(name)) return fail("Cancelado");
        }
        if (pack_inside) {
            if (!step(from + (to - from) * static_cast<float>(done) / static_cast<float>(std::max<std::size_t>(total, 1)),
                      "Assets del juego (" + input.pack_name + ")")) {
                return fail("Cancelado");
            }
            if (!zip.addFile("assets/Game/" + input.pack_name, input.pack, error)) return false;
            tick(input.pack_name);
        }
        return true;
    };

    // Clave: la del usuario o la de depuracion (se crea una vez por PC).
    AndroidKey key = input.key;
    if (key.keystore.empty()) {
        fs::path folder = fromUtf8(env("LOCALAPPDATA")) / "Cramion" / "Android";
        fs::create_directories(folder, ec);
        key.keystore = folder / "debug.keystore";
        key.store_password = key.key_password = "android";
        key.alias = "androiddebugkey";
        if (!fileExists(key.keystore)) {
            if (!step(0.1f, "Creando la clave de depuracion")) return fail("Cancelado");
            if (!tool(tools.java_bin / "keytool.exe",
                      {"-genkeypair", "-keystore", utf8(key.keystore), "-storepass", "android", "-alias", "androiddebugkey",
                       "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname",
                       "CN=Android Debug,O=Android,C=US"},
                      "keytool")) {
                return false;
            }
        }
    } else if (!fileExists(key.keystore)) {
        return fail("No existe el keystore " + utf8(key.keystore));
    }
    if (key.alias.empty()) return fail("Falta el alias de la clave del keystore");
    if (key.key_password.empty()) key.key_password = key.store_password;

    // --- 3. APK ---
    if (input.make_apk) {
        if (!step(0.12f, "Enlazando el APK (aapt2)")) return fail("Cancelado");
        if (!link(work / "resources.apk", false)) return false;
        ZipWriter zip;
        if (!zip.open(work / "unaligned.apk")) return fail("No se pudo crear " + utf8(work / "unaligned.apk"));
        if (!zip.copyFrom(work / "resources.apk", nullptr, error)) return false;
        if (!add_payload(zip, 0.14f, input.make_aab ? 0.4f : 0.7f)) return false;
        if (!zip.close(error)) return false;
        if (!step(input.make_aab ? 0.42f : 0.74f, "Alineando (zipalign)")) return fail("Cancelado");
        const fs::path aligned = work / "aligned.apk";
        // -P 16: las .so alineadas a paginas de 16 KB (Android 15+).
        if (!tool(tools.build_tools / "zipalign.exe", {"-f", "-P", "16", "4", utf8(work / "unaligned.apk"), utf8(aligned)}, "zipalign")) {
            // build-tools viejas sin -P: con -p (4 KB).
            if (!tool(tools.build_tools / "zipalign.exe", {"-f", "-p", "4", utf8(work / "unaligned.apk"), utf8(aligned)}, "zipalign")) {
                return false;
            }
        }
        if (!step(input.make_aab ? 0.46f : 0.82f, "Firmando (apksigner)")) return fail("Cancelado");
        result.apk = input.output_folder / fromUtf8(input.file_stem + ".apk");
        fs::remove(result.apk, ec);
        if (!tool(tools.build_tools / "apksigner.bat",
                  {"sign", "--ks", utf8(key.keystore), "--ks-pass", "pass:" + key.store_password, "--ks-key-alias", key.alias,
                   "--key-pass", "pass:" + key.key_password, "--out", utf8(result.apk), utf8(aligned)},
                  "apksigner")) {
            return false;
        }
        fs::remove(fs::path(result.apk).concat(".idsig"), ec);
        if (input.split_obb && !input.pack.empty()) {
            if (!step(input.make_aab ? 0.5f : 0.88f, "OBB de expansion")) return fail("Cancelado");
            result.obb = input.output_folder / fromUtf8("main." + std::to_string(input.version_code) + "." + input.package + ".obb");
            fs::copy_file(input.pack, result.obb, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("No se pudo escribir el OBB: " + ec.message());
        }
    }

    // --- 4. AAB (Google Play) ---
    if (input.make_aab) {
        if (!step(0.52f, "Enlazando el AAB (aapt2 proto)")) return fail("Cancelado");
        if (!link(work / "proto.apk", true)) return false;
        ZipWriter zip;
        if (!zip.open(work / "base.zip")) return fail("No se pudo crear el modulo base");
        const bool copied = zip.copyFrom(work / "proto.apk", [](const std::string& name) {
            return name == "AndroidManifest.xml" ? std::string("manifest/AndroidManifest.xml") : name;
        }, error);
        if (!copied) return false;
        if (!add_payload(zip, 0.54f, 0.78f)) return false;
        if (!zip.close(error)) return false;
        // Los assets y las .so sin comprimir (el .crpack ya va comprimido).
        std::ofstream(work / "BundleConfig.json")
            << "{\"compression\":{\"uncompressedGlob\":[\"assets/**\"]},\"optimizations\":{\"uncompressNativeLibraries\":{\"enabled\":true}}}";
        if (!step(0.8f, "Construyendo el bundle (bundletool)")) return fail("Cancelado");
        result.aab = input.output_folder / fromUtf8(input.file_stem + ".aab");
        fs::remove(result.aab, ec);
        if (!tool(tools.java_bin / "java.exe",
                  {"-jar", utf8(tools_ref.bundletool), "build-bundle", "--modules=" + utf8(work / "base.zip"),
                   "--config=" + utf8(work / "BundleConfig.json"), "--output=" + utf8(result.aab)},
                  "bundletool")) {
            return false;
        }
        if (!step(0.92f, "Firmando el AAB (jarsigner)")) return fail("Cancelado");
        if (!tool(tools.java_bin / "jarsigner.exe",
                  {"-keystore", utf8(key.keystore), "-storepass", key.store_password, "-keypass", key.key_password, "-sigalg",
                   "SHA256withRSA", "-digestalg", "SHA-256", utf8(result.aab), key.alias},
                  "jarsigner")) {
            return false;
        }
    }
    step(1.0f, "Listo");
    fs::remove_all(work, ec);
    return true;
}

std::vector<std::string> androidDevices(const AndroidToolchain& tools) {
    std::vector<std::string> devices;
    if (tools.adb.empty()) return devices;
    std::string out;
    if (run(tools.adb, {"devices", "-l"}, out) != 0) return devices;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string serial;
        std::string state;
        words >> serial >> state;
        if (state != "device") continue;
        std::string model;
        std::string word;
        while (words >> word) {
            if (word.rfind("model:", 0) == 0) model = word.substr(6);
        }
        devices.push_back(model.empty() ? serial : serial + "  " + model);
    }
    return devices;
}

bool installAndroidPackage(const AndroidToolchain& tools, const std::string& device, const fs::path& apk, const fs::path& obb,
                           const std::string& package, std::string& log, std::string* error) {
    if (tools.adb.empty()) {
        if (error != nullptr) *error = "No se encontro adb (platform-tools del SDK)";
        return false;
    }
    const std::string serial = device.substr(0, device.find(' '));
    const auto adb = [&](std::vector<std::string> args, const std::string& what) {
        if (!serial.empty()) args.insert(args.begin(), {"-s", serial});
        std::string out;
        const int code = run(tools.adb, args, out);
        log += "> adb";
        for (const std::string& a : args) log += " " + a;
        log += "\n" + out;
        // adb install devuelve 0 aunque falle en algunos casos: se mira el texto.
        if (code != 0 || out.find("Failure") != std::string::npos || out.find("error:") != std::string::npos) {
            if (error != nullptr) *error = what + ": " + out;
            return false;
        }
        return true;
    };
    if (!adb({"install", "-r", "--no-incremental", utf8(apk)}, "No se pudo instalar el APK")) return false;
    if (!obb.empty()) {
        const std::string folder = "/sdcard/Android/obb/" + package;
        adb({"shell", "mkdir", "-p", folder}, "No se pudo crear la carpeta del OBB");
        if (!adb({"push", utf8(obb), folder + "/" + utf8(obb.filename())}, "No se pudo subir el OBB")) return false;
    }
    return adb({"shell", "am", "start", "-n", package + "/android.app.NativeActivity"}, "No se pudo abrir el juego");
}

}  // namespace cramion::editor
