#include "CramionCore/crwpack/CrwPack.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace cramion::crwpack {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'C', 'R', 'W', 'P', 'A', 'C', 'K', '\0'};
constexpr char kIndexMagic[4] = {'C', 'W', 'I', 'X'};
constexpr std::uint32_t kHeaderSize = 64;
constexpr std::uint32_t kMaxManifest = 1u << 20;

struct Header {
    char magic[8];
    std::uint32_t format;
    std::uint32_t flags;
    std::uint32_t block_size;
    std::uint32_t file_count;
    std::uint64_t block_count;
    std::uint64_t raw_size;
    std::uint64_t manifest_offset;
    std::uint32_t manifest_size;
    std::uint32_t index_size;
    std::uint64_t index_offset;
};
static_assert(sizeof(Header) == kHeaderSize, "la cabecera del .crwpack mide 64 bytes");

void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}

std::string pathText(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path pathFromText(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

int threadCount(int requested, std::size_t jobs) {
    int n = requested > 0 ? requested : static_cast<int>(std::thread::hardware_concurrency());
    if (n < 1) n = 1;
    if (static_cast<std::size_t>(n) > jobs) n = static_cast<int>(std::max<std::size_t>(jobs, 1));
    return n;
}

// --- Escritura/lectura binaria del indice ------------------------------------

struct Writer {
    std::string bytes;
    template <class T>
    void put(T value) {
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    void raw(const void* data, std::size_t size) { bytes.append(static_cast<const char*>(data), size); }
};

struct Reader {
    const char* data;
    std::size_t size;
    std::size_t pos = 0;
    template <class T>
    bool get(T& value) {
        if (size - pos < sizeof(T)) return false;
        std::memcpy(&value, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    bool raw(std::string& out, std::size_t n) {
        if (size - pos < n) return false;
        out.assign(data + pos, n);
        pos += n;
        return true;
    }
};

// --- Tuberia en paralelo con salida en orden ---------------------------------
//
// produce(i) se ejecuta en `threads` hilos (cualquier orden); consume(i) en el
// hilo que llama, en orden 0, 1, 2... Como mucho `window` resultados esperan
// a ser consumidos, asi la memoria queda acotada aunque el disco sea lento.

template <class R>
bool orderedParallel(std::size_t count, int threads, std::size_t window,
                     const std::function<bool(std::size_t, R&, std::string&)>& produce,
                     const std::function<bool(std::size_t, R&, std::string&)>& consume, std::string& error) {
    if (count == 0) return true;
    std::mutex mutex;
    std::condition_variable work_cv, done_cv;
    std::size_t next = 0, consumed = 0;
    std::map<std::size_t, R> ready;
    bool failed = false;

    auto worker = [&] {
        for (;;) {
            std::size_t index;
            {
                std::unique_lock lock(mutex);
                work_cv.wait(lock, [&] { return failed || next >= count || next < consumed + window; });
                if (failed || next >= count) return;
                index = next++;
            }
            R result{};
            std::string message;
            const bool ok = produce(index, result, message);
            std::lock_guard lock(mutex);
            if (!ok) {
                if (!failed) error = message;
                failed = true;
                work_cv.notify_all();
                done_cv.notify_all();
                return;
            }
            ready.emplace(index, std::move(result));
            done_cv.notify_all();
        }
    };

    std::vector<std::thread> pool;
    const int n = threadCount(threads, count);
    pool.reserve(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t) pool.emplace_back(worker);

    for (std::size_t i = 0; i < count; ++i) {
        R result{};
        {
            std::unique_lock lock(mutex);
            done_cv.wait(lock, [&] { return failed || ready.count(i) != 0; });
            if (failed) break;
            auto it = ready.find(i);
            result = std::move(it->second);
            ready.erase(it);
            consumed = i + 1;
            work_cv.notify_all();
        }
        std::string message;
        if (!consume(i, result, message)) {
            std::lock_guard lock(mutex);
            if (!failed) error = message;
            failed = true;
            work_cv.notify_all();
            break;
        }
    }
    {
        std::lock_guard lock(mutex);
        if (consumed < count) failed = true;
        work_cv.notify_all();
    }
    for (std::thread& t : pool) t.join();
    return !failed;
}

struct CCtxDeleter {
    void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct DCtxDeleter {
    void operator()(ZSTD_DCtx* c) const { ZSTD_freeDCtx(c); }
};

ZSTD_CCtx* threadCCtx() {
    thread_local std::unique_ptr<ZSTD_CCtx, CCtxDeleter> ctx(ZSTD_createCCtx());
    return ctx.get();
}

ZSTD_DCtx* threadDCtx() {
    thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> ctx(ZSTD_createDCtx());
    return ctx.get();
}

struct Block {
    std::string data;  // comprimido (o en bruto si method == Stored)
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    Method method = Method::Zstd;
};

std::string isoNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

// Lee el trozo [begin, end) del flujo concatenado de `files`.
bool readStream(const std::vector<std::pair<fs::path, std::string>>& inputs, const std::vector<FileEntry>& files,
                std::uint64_t begin, std::uint64_t end, std::string& out, std::string& error) {
    out.resize(static_cast<std::size_t>(end - begin));
    // Primer archivo que llega a `begin`.
    auto it = std::upper_bound(files.begin(), files.end(), begin,
                               [](std::uint64_t value, const FileEntry& f) { return value < f.offset + f.size; });
    std::uint64_t pos = begin;
    while (pos < end && it != files.end()) {
        const FileEntry& f = *it;
        const std::size_t index = static_cast<std::size_t>(it - files.begin());
        if (f.size == 0 || pos >= f.offset + f.size) {
            ++it;
            continue;
        }
        const std::uint64_t from = pos - f.offset;
        const std::uint64_t take = std::min(end, f.offset + f.size) - pos;
        std::ifstream in(inputs[index].first, std::ios::binary);
        if (!in) {
            error = "No se pudo leer " + f.path;
            return false;
        }
        in.seekg(static_cast<std::streamoff>(from));
        in.read(out.data() + (pos - begin), static_cast<std::streamsize>(take));
        if (static_cast<std::uint64_t>(in.gcount()) != take) {
            error = f.path + " cambio mientras se empaquetaba";
            return false;
        }
        pos += take;
        ++it;
    }
    if (pos != end) {
        error = "Los archivos cambiaron mientras se empaquetaban";
        return false;
    }
    return true;
}

}  // namespace

// =============================================================================

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t crc) {
    static const std::array<std::array<std::uint32_t, 256>, 8> table = [] {
        std::array<std::array<std::uint32_t, 256>, 8> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; ++i) {
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        }
        return t;
    }();
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    // Slicing-by-8: 8 bytes por vuelta.
    while (size >= 8) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= crc;
        crc = table[7][lo & 0xFF] ^ table[6][(lo >> 8) & 0xFF] ^ table[5][(lo >> 16) & 0xFF] ^ table[4][lo >> 24] ^
              table[3][hi & 0xFF] ^ table[2][(hi >> 8) & 0xFF] ^ table[1][(hi >> 16) & 0xFF] ^ table[0][hi >> 24];
        p += 8;
        size -= 8;
    }
    while (size--) crc = table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool safeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.front() == '/') return false;
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) {
            if (c < 32) return false;
        }
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

bool validName(const std::string& name) {
    if (name.size() < 2 || name.size() > 64) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) return false;
    }
    return true;
}

namespace {

struct SemVer {
    std::uint64_t major = 0, minor = 0, patch = 0;
    std::string pre;
};

bool parseVersion(const std::string& text, SemVer& v) {
    std::size_t pos = 0;
    auto number = [&](std::uint64_t& out) {
        const std::size_t start = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        if (pos == start || pos - start > 9 || (text[start] == '0' && pos - start > 1)) return false;
        out = std::stoull(text.substr(start, pos - start));
        return true;
    };
    if (!number(v.major) || pos >= text.size() || text[pos++] != '.') return false;
    if (!number(v.minor) || pos >= text.size() || text[pos++] != '.') return false;
    if (!number(v.patch)) return false;
    if (pos == text.size()) return true;
    if (text[pos++] != '-' || pos == text.size() || text.size() - pos > 32) return false;
    v.pre = text.substr(pos);
    for (char c : v.pre) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
    }
    return true;
}

}  // namespace

bool validVersion(const std::string& version) {
    SemVer v;
    return parseVersion(version, v);
}

int compareVersions(const std::string& a, const std::string& b) {
    SemVer x, y;
    const bool okx = parseVersion(a, x), oky = parseVersion(b, y);
    if (!okx || !oky) return okx == oky ? a.compare(b) : (okx ? 1 : -1);
    if (x.major != y.major) return x.major < y.major ? -1 : 1;
    if (x.minor != y.minor) return x.minor < y.minor ? -1 : 1;
    if (x.patch != y.patch) return x.patch < y.patch ? -1 : 1;
    if (x.pre == y.pre) return 0;
    if (x.pre.empty()) return 1;  // 1.0.0 > 1.0.0-beta
    if (y.pre.empty()) return -1;
    // Identificadores separados por '.', los numericos se comparan como numeros.
    std::size_t i = 0, j = 0;
    while (i <= x.pre.size() && j <= y.pre.size()) {
        const std::size_t ei = std::min(x.pre.find('.', i), x.pre.size());
        const std::size_t ej = std::min(y.pre.find('.', j), y.pre.size());
        const std::string pa = x.pre.substr(i, ei - i), pb = y.pre.substr(j, ej - j);
        const bool na = !pa.empty() && pa.find_first_not_of("0123456789") == std::string::npos;
        const bool nb = !pb.empty() && pb.find_first_not_of("0123456789") == std::string::npos;
        int c;
        if (na && nb) {
            c = pa.size() != pb.size() ? (pa.size() < pb.size() ? -1 : 1) : pa.compare(pb);
        } else if (na != nb) {
            c = na ? -1 : 1;
        } else {
            c = pa.compare(pb);
        }
        if (c != 0) return c < 0 ? -1 : 1;
        if (ei == x.pre.size() || ej == y.pre.size()) {
            if (ei == x.pre.size() && ej == y.pre.size()) return 0;
            return ei == x.pre.size() ? -1 : 1;
        }
        i = ei + 1;
        j = ej + 1;
    }
    return 0;
}

// =============================================================================
// Empaquetar
// =============================================================================

bool packFiles(const std::vector<std::pair<fs::path, std::string>>& source_inputs, const fs::path& file,
               nlohmann::json manifest, const PackOptions& options, const Progress& progress, std::string* error) {
    if (!manifest.is_object()) manifest = nlohmann::json::object();
    const std::uint32_t block_size = std::clamp<std::uint32_t>(options.block_size, 64u << 10, 64u << 20);
    const int level = std::clamp(options.level, 1, ZSTD_maxCLevel());

    // Orden estable por ruta y tamanos.
    std::vector<std::pair<fs::path, std::string>> inputs = source_inputs;
    std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<FileEntry> files;
    files.reserve(inputs.size());
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!safeRelativePath(inputs[i].second)) {
            setError(error, "Ruta no valida dentro del paquete: " + inputs[i].second);
            return false;
        }
        if (i > 0 && inputs[i].second == inputs[i - 1].second) {
            setError(error, "Ruta repetida en el paquete: " + inputs[i].second);
            return false;
        }
        std::error_code ec;
        const std::uint64_t size = fs::file_size(inputs[i].first, ec);
        if (ec) {
            setError(error, "No se pudo leer " + pathText(inputs[i].first));
            return false;
        }
        files.push_back({inputs[i].second, total, size});
        total += size;
    }
    if (files.size() > 0xFFFFFFFFu) {
        setError(error, "Demasiados archivos");
        return false;
    }

    manifest["format"] = kFormatVersion;
    manifest["files"] = files.size();
    manifest["rawSize"] = total;
    if (!manifest.contains("created")) manifest["created"] = isoNow();
    const std::string manifest_text = manifest.dump(2);
    if (manifest_text.size() > kMaxManifest) {
        setError(error, "El manifiesto es demasiado grande");
        return false;
    }

    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        setError(error, "No se pudo crear " + pathText(temp));
        return false;
    }
    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.format = kFormatVersion;
    header.block_size = block_size;
    header.file_count = static_cast<std::uint32_t>(files.size());
    header.raw_size = total;
    header.manifest_offset = kHeaderSize;
    header.manifest_size = static_cast<std::uint32_t>(manifest_text.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(manifest_text.data(), static_cast<std::streamsize>(manifest_text.size()));

    const std::size_t block_count = static_cast<std::size_t>((total + block_size - 1) / block_size);
    std::vector<BlockEntry> blocks(block_count);
    std::uint64_t written = kHeaderSize + manifest_text.size();
    std::uint64_t done = 0;
    const int threads = threadCount(options.threads, block_count);

    std::string message;
    const bool ok = orderedParallel<Block>(
        block_count, threads, static_cast<std::size_t>(threads) * 2 + 2,
        [&](std::size_t i, Block& b, std::string& err) {
            std::string raw;
            const std::uint64_t begin = static_cast<std::uint64_t>(i) * block_size;
            if (!readStream(inputs, files, begin, std::min<std::uint64_t>(total, begin + block_size), raw, err)) return false;
            b.size = static_cast<std::uint32_t>(raw.size());
            b.crc = crc32(raw.data(), raw.size());
            b.data.resize(ZSTD_compressBound(raw.size()));
            const std::size_t r = ZSTD_compressCCtx(threadCCtx(), b.data.data(), b.data.size(), raw.data(), raw.size(), level);
            if (ZSTD_isError(r)) {
                err = std::string("zstd: ") + ZSTD_getErrorName(r);
                return false;
            }
            if (r >= raw.size()) {  // no gana nada (ya comprimido: video, ogg, png...)
                b.data = std::move(raw);
                b.method = Method::Stored;
            } else {
                b.data.resize(r);
                b.method = Method::Zstd;
            }
            return true;
        },
        [&](std::size_t i, Block& b, std::string& err) {
            blocks[i] = {written, static_cast<std::uint32_t>(b.data.size()), b.size, b.crc, b.method};
            out.write(b.data.data(), static_cast<std::streamsize>(b.data.size()));
            if (!out) {
                err = "No se pudo escribir el paquete (disco lleno?)";
                return false;
            }
            written += b.data.size();
            done += b.size;
            if (progress && !progress(done, total)) {
                err = "Cancelado";
                return false;
            }
            return true;
        },
        message);
    if (!ok) {
        out.close();
        fs::remove(temp, ec);
        setError(error, message);
        return false;
    }

    Writer index;
    index.raw(kIndexMagic, 4);
    index.put<std::uint32_t>(static_cast<std::uint32_t>(files.size()));
    for (const FileEntry& f : files) {
        index.put<std::uint16_t>(static_cast<std::uint16_t>(f.path.size()));
        index.raw(f.path.data(), f.path.size());
        index.put<std::uint64_t>(f.offset);
        index.put<std::uint64_t>(f.size);
    }
    index.put<std::uint64_t>(blocks.size());
    for (const BlockEntry& b : blocks) {
        index.put<std::uint64_t>(b.offset);
        index.put<std::uint32_t>(b.compressed);
        index.put<std::uint32_t>(b.size);
        index.put<std::uint32_t>(b.crc);
        index.put<std::uint32_t>(static_cast<std::uint32_t>(b.method));
    }
    index.put<std::uint32_t>(crc32(index.bytes.data(), index.bytes.size()));
    out.write(index.bytes.data(), static_cast<std::streamsize>(index.bytes.size()));

    header.block_count = blocks.size();
    header.index_offset = written;
    header.index_size = static_cast<std::uint32_t>(index.bytes.size());
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.close();
    if (!out) {
        fs::remove(temp, ec);
        setError(error, "No se pudo escribir el paquete");
        return false;
    }
    fs::remove(file, ec);
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        setError(error, "No se pudo renombrar el paquete a " + pathText(file));
        return false;
    }
    if (progress && block_count == 0) progress(0, 0);
    return true;
}

bool pack(const fs::path& folder, const fs::path& file, nlohmann::json manifest, const PackOptions& options,
          const Progress& progress, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        setError(error, "No existe la carpeta " + pathText(folder));
        return false;
    }
    const fs::path out_abs = fs::weakly_canonical(file, ec);
    std::vector<std::pair<fs::path, std::string>> inputs;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::error_code e2;
        if (fs::weakly_canonical(it->path(), e2) == out_abs) continue;  // el propio paquete
        const std::string ext = pathText(it->path().extension());
        if (ext == ".tmp") continue;
        const std::u8string generic = fs::relative(it->path(), folder, e2).generic_u8string();
        if (e2) continue;
        std::string rel(generic.begin(), generic.end());
        inputs.emplace_back(it->path(), rel);
    }
    if (ec) {
        setError(error, "No se pudo recorrer " + pathText(folder));
        return false;
    }
    return packFiles(inputs, file, std::move(manifest), options, progress, error);
}

// =============================================================================
// Leer
// =============================================================================

bool readInfo(const fs::path& file, Info& info, std::string* error) {
    info = Info{};
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        setError(error, "No se pudo abrir " + pathText(file));
        return false;
    }
    std::error_code ec;
    const std::uint64_t file_size = fs::file_size(file, ec);
    Header header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (ec || in.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        setError(error, "No es un archivo .crwpack");
        return false;
    }
    if (header.format != kFormatVersion) {
        setError(error, "Version de .crwpack no soportada (" + std::to_string(header.format) +
                            "); actualiza el motor");
        return false;
    }
    if (header.manifest_size == 0 || header.manifest_size > kMaxManifest ||
        header.manifest_offset + header.manifest_size > file_size || header.index_size < 20 ||
        header.index_offset + header.index_size > file_size) {
        setError(error, "El .crwpack esta danado (cabecera)");
        return false;
    }
    std::string text(header.manifest_size, '\0');
    in.seekg(static_cast<std::streamoff>(header.manifest_offset));
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    info.manifest = nlohmann::json::parse(text, nullptr, false);
    if (!in || info.manifest.is_discarded() || !info.manifest.is_object()) {
        setError(error, "El manifiesto del .crwpack esta danado");
        return false;
    }

    std::string index(header.index_size, '\0');
    in.seekg(static_cast<std::streamoff>(header.index_offset));
    in.read(index.data(), static_cast<std::streamsize>(index.size()));
    if (!in) {
        setError(error, "No se pudo leer el indice del .crwpack");
        return false;
    }
    std::uint32_t stored_crc;
    std::memcpy(&stored_crc, index.data() + index.size() - 4, 4);
    if (crc32(index.data(), index.size() - 4) != stored_crc || std::memcmp(index.data(), kIndexMagic, 4) != 0) {
        setError(error, "El indice del .crwpack esta danado");
        return false;
    }
    Reader r{index.data() + 4, index.size() - 8};
    std::uint32_t file_count = 0;
    if (!r.get(file_count) || file_count != header.file_count) {
        setError(error, "El indice del .crwpack esta danado");
        return false;
    }
    info.files.reserve(std::min<std::uint32_t>(file_count, 1u << 20));
    std::uint64_t expected = 0;
    for (std::uint32_t i = 0; i < file_count; ++i) {
        std::uint16_t len = 0;
        FileEntry f;
        if (!r.get(len) || !r.raw(f.path, len) || !r.get(f.offset) || !r.get(f.size) || f.offset != expected ||
            !safeRelativePath(f.path)) {
            setError(error, "El indice del .crwpack esta danado o tiene rutas no validas");
            return false;
        }
        expected += f.size;
        info.files.push_back(std::move(f));
    }
    std::uint64_t block_count = 0;
    if (!r.get(block_count) || block_count != header.block_count || expected != header.raw_size) {
        setError(error, "El indice del .crwpack esta danado");
        return false;
    }
    std::uint64_t raw = 0;
    for (std::uint64_t i = 0; i < block_count; ++i) {
        BlockEntry b;
        std::uint32_t method = 0;
        if (!r.get(b.offset) || !r.get(b.compressed) || !r.get(b.size) || !r.get(b.crc) || !r.get(method) ||
            method > 1 || b.offset + b.compressed > header.index_offset ||
            (i + 1 < block_count && b.size != header.block_size) || b.size > header.block_size || b.size == 0) {
            setError(error, "El indice del .crwpack esta danado (bloques)");
            return false;
        }
        b.method = static_cast<Method>(method);
        raw += b.size;
        info.blocks.push_back(b);
    }
    if (raw != header.raw_size) {
        setError(error, "El indice del .crwpack esta danado (tamanos)");
        return false;
    }
    info.format = header.format;
    info.block_size = header.block_size;
    info.raw_size = header.raw_size;
    info.packed_size = file_size;
    return true;
}

bool readManifest(const fs::path& file, nlohmann::json& manifest, std::string* error) {
    Info info;
    if (!readInfo(file, info, error)) return false;
    manifest = std::move(info.manifest);
    return true;
}

namespace {

bool decodeBlock(const fs::path& file, const BlockEntry& b, std::string& raw, std::string& err) {
    std::ifstream in(file, std::ios::binary);
    std::string packed(b.compressed, '\0');
    in.seekg(static_cast<std::streamoff>(b.offset));
    in.read(packed.data(), static_cast<std::streamsize>(packed.size()));
    if (!in) {
        err = "No se pudo leer el .crwpack";
        return false;
    }
    if (b.method == Method::Stored) {
        if (b.compressed != b.size) {
            err = "El .crwpack esta danado";
            return false;
        }
        raw = std::move(packed);
    } else {
        raw.resize(b.size);
        const std::size_t r = ZSTD_decompressDCtx(threadDCtx(), raw.data(), raw.size(), packed.data(), packed.size());
        if (ZSTD_isError(r) || r != b.size) {
            err = "El .crwpack esta danado (no se pudo descomprimir un bloque)";
            return false;
        }
    }
    if (crc32(raw.data(), raw.size()) != b.crc) {
        err = "El .crwpack esta danado (CRC incorrecto)";
        return false;
    }
    return true;
}

}  // namespace

bool unpack(const fs::path& file, const fs::path& folder, int threads, const Progress& progress, std::string* error) {
    Info info;
    if (!readInfo(file, info, error)) return false;
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        setError(error, "No se pudo crear " + pathText(folder));
        return false;
    }
    // Carpetas y archivos vacios primero.
    for (const FileEntry& f : info.files) {
        const fs::path target = folder / pathFromText(f.path);
        fs::create_directories(target.parent_path(), ec);
        if (f.size == 0) std::ofstream(target, std::ios::binary | std::ios::trunc);
    }

    std::size_t file_index = 0;
    std::uint64_t file_written = 0;
    std::ofstream out;
    std::uint64_t done = 0;
    std::string message;
    const bool ok = orderedParallel<std::string>(
        info.blocks.size(), threads, static_cast<std::size_t>(threadCount(threads, info.blocks.size())) * 2 + 2,
        [&](std::size_t i, std::string& raw, std::string& err) { return decodeBlock(file, info.blocks[i], raw, err); },
        [&](std::size_t, std::string& raw, std::string& err) {
            std::size_t pos = 0;
            while (pos < raw.size()) {
                while (file_index < info.files.size() && info.files[file_index].size == 0) ++file_index;
                if (file_index >= info.files.size()) {
                    err = "El .crwpack esta danado (datos de mas)";
                    return false;
                }
                const FileEntry& f = info.files[file_index];
                if (!out.is_open()) {
                    out.open(folder / pathFromText(f.path), std::ios::binary | std::ios::trunc);
                    if (!out) {
                        err = "No se pudo escribir " + f.path;
                        return false;
                    }
                }
                const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(raw.size() - pos, f.size - file_written));
                out.write(raw.data() + pos, static_cast<std::streamsize>(take));
                pos += take;
                file_written += take;
                if (file_written == f.size) {
                    out.close();
                    if (out.fail()) {
                        err = "No se pudo escribir " + f.path + " (disco lleno?)";
                        return false;
                    }
                    out.clear();
                    ++file_index;
                    file_written = 0;
                }
            }
            done += raw.size();
            if (progress && !progress(done, info.raw_size)) {
                err = "Cancelado";
                return false;
            }
            return true;
        },
        message);
    if (out.is_open()) out.close();
    if (!ok) {
        setError(error, message);
        return false;
    }
    if (progress && info.blocks.empty()) progress(0, 0);
    return true;
}

bool verify(const fs::path& file, int threads, const Progress& progress, std::string* error) {
    Info info;
    if (!readInfo(file, info, error)) return false;
    std::uint64_t done = 0;
    std::string message;
    const bool ok = orderedParallel<std::uint32_t>(
        info.blocks.size(), threads, static_cast<std::size_t>(threadCount(threads, info.blocks.size())) * 4,
        [&](std::size_t i, std::uint32_t& size, std::string& err) {
            std::string raw;
            if (!decodeBlock(file, info.blocks[i], raw, err)) return false;
            size = static_cast<std::uint32_t>(raw.size());
            return true;
        },
        [&](std::size_t, std::uint32_t& size, std::string& err) {
            done += size;
            if (progress && !progress(done, info.raw_size)) {
                err = "Cancelado";
                return false;
            }
            return true;
        },
        message);
    if (!ok) setError(error, message);
    return ok;
}

}  // namespace cramion::crwpack
