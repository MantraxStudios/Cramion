#include "CramionCore/asset/BakedLightingFile.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace cramion::lighting {

using core::Vec3;
using core::Vec4;

namespace {

constexpr char kMagic[4] = {'C', 'R', 'B', 'K'};
constexpr std::uint32_t kLegacyVersion = 3;  // 2: + lightmap de superficie; 3: + luces horneadas (directa)
constexpr std::uint32_t kFlagSunVisibility = 1u << 0;
constexpr std::uint32_t kFlagLightmaps = 1u << 1;
constexpr std::uint32_t kFlagIrradiance = 1u << 2;
constexpr std::uint32_t kChunkZstd = 1u << 0;
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kTocEntryBytes = 40;
constexpr std::size_t kRecordBytes = 16 + 16 + 4 + 4 + 16 + 8 + 48;  // 112
static_assert(kRecordBytes == 112, "registro LMRC de 112 bytes");
// Trozos de mas de esto se comprimen (los pequenos no ganan nada).
constexpr std::size_t kCompressAbove = 256;

std::uint64_t fnv(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

struct Bytes {
    std::vector<std::uint8_t> data;
    template <typename T>
    void put(const T& value) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
        data.insert(data.end(), p, p + sizeof(T));
    }
    void putRaw(const void* p, std::size_t size) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        data.insert(data.end(), b, b + size);
    }
    void putString(const std::string& text) {
        put(static_cast<std::uint32_t>(text.size()));
        putRaw(text.data(), text.size());
    }
};

struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t at = 0;
    template <typename T>
    bool get(T& value) {
        if (at + sizeof(T) > size) return false;
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
    bool getRaw(void* out, std::size_t bytes) {
        if (at + bytes > size) return false;
        if (bytes > 0) std::memcpy(out, data + at, bytes);
        at += bytes;
        return true;
    }
    bool getString(std::string& text) {
        std::uint32_t length = 0;
        if (!get(length) || at + length > size) return false;
        text.assign(reinterpret_cast<const char*>(data + at), length);
        at += length;
        return true;
    }
};

struct TocEntry {
    char id[4] = {};
    std::uint32_t flags = 0;
    std::uint64_t offset = 0, stored = 0, raw = 0, hash = 0;
};

bool sameId(const char* a, const char* b) { return std::memcmp(a, b, 4) == 0; }

// --- v1-v3 ---------------------------------------------------------------------

bool saveLegacy(const std::filesystem::path& temporary, const gfx::BakedLighting& data, gfx::LightingMode mode) {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(kMagic, 4);
    const std::uint32_t header[3] = {kLegacyVersion, static_cast<std::uint32_t>(mode), static_cast<std::uint32_t>(data.volumes.size())};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (const gfx::BakedProbeVolume& v : data.volumes) {
        const float f[7] = {v.min.x, v.min.y, v.min.z, v.size.x, v.size.y, v.size.z, v.intensity};
        const std::uint32_t u[4] = {v.nx, v.ny, v.nz, v.first};
        out.write(reinterpret_cast<const char*>(f), sizeof(f));
        out.write(reinterpret_cast<const char*>(u), sizeof(u));
    }
    const std::uint64_t count = data.probes.size();
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    if (count > 0) out.write(reinterpret_cast<const char*>(data.probes.data()), static_cast<std::streamsize>(count * sizeof(Vec4)));
    // Lightmap de superficie (version 2).
    const gfx::SurfaceLightmap& sl = data.surface;
    const std::uint32_t surface_header[3] = {sl.capacity, sl.count, 0};
    out.write(reinterpret_cast<const char*>(&sl.cell_size), sizeof(float));
    out.write(reinterpret_cast<const char*>(surface_header), sizeof(surface_header));
    if (sl.capacity > 0) {
        out.write(reinterpret_cast<const char*>(sl.keys.data()), static_cast<std::streamsize>(sl.keys.size() * sizeof(std::uint32_t)));
        out.write(reinterpret_cast<const char*>(sl.texels.data()), static_cast<std::streamsize>(sl.texels.size() * sizeof(Vec4)));
    }
    // Luces horneadas (version 3): el sol y los UUID de las locales.
    const std::uint32_t light_header[2] = {data.sun_baked ? 1u : 0u, static_cast<std::uint32_t>(data.baked_lights.size())};
    const float sun_direction[3] = {data.sun_direction.x, data.sun_direction.y, data.sun_direction.z};
    out.write(reinterpret_cast<const char*>(light_header), sizeof(light_header));
    out.write(reinterpret_cast<const char*>(sun_direction), sizeof(sun_direction));
    for (const auto& id : data.baked_lights) out.write(reinterpret_cast<const char*>(id.data()), sizeof(std::uint64_t) * 2);
    return static_cast<bool>(out);
}

bool loadLegacy(std::ifstream& in, std::uint32_t version, std::uint32_t volume_count, gfx::BakedLighting& result,
                const std::filesystem::path& file, std::string* error) {
    for (std::uint32_t i = 0; i < volume_count; ++i) {
        float f[7] = {};
        std::uint32_t u[4] = {};
        in.read(reinterpret_cast<char*>(f), sizeof(f));
        in.read(reinterpret_cast<char*>(u), sizeof(u));
        gfx::BakedProbeVolume v;
        v.min = Vec3{f[0], f[1], f[2]};
        v.size = Vec3{f[3], f[4], f[5]};
        v.intensity = f[6];
        v.nx = u[0];
        v.ny = u[1];
        v.nz = u[2];
        v.first = u[3];
        result.volumes.push_back(v);
    }
    std::uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || count > (1ull << 28)) {
        if (error) *error = "archivo de iluminacion danado: " + file.string();
        return false;
    }
    result.probes.resize(static_cast<std::size_t>(count));
    if (count > 0) in.read(reinterpret_cast<char*>(result.probes.data()), static_cast<std::streamsize>(count * sizeof(Vec4)));
    if (!in) {
        if (error) *error = "archivo de iluminacion incompleto: " + file.string();
        return false;
    }
    if (version >= 2) {
        gfx::SurfaceLightmap& sl = result.surface;
        std::uint32_t surface_header[3] = {};
        in.read(reinterpret_cast<char*>(&sl.cell_size), sizeof(float));
        in.read(reinterpret_cast<char*>(surface_header), sizeof(surface_header));
        if (!in || surface_header[0] > (1u << 26) || (surface_header[0] & (surface_header[0] - 1)) != 0) {
            if (error) *error = "archivo de iluminacion danado: " + file.string();
            return false;
        }
        sl.capacity = surface_header[0];
        sl.count = surface_header[1];
        if (sl.capacity > 0) {
            sl.keys.resize(static_cast<std::size_t>(sl.capacity) * 2);
            sl.texels.resize(static_cast<std::size_t>(sl.capacity) * gfx::kSurfaceTexelVec4);
            in.read(reinterpret_cast<char*>(sl.keys.data()), static_cast<std::streamsize>(sl.keys.size() * sizeof(std::uint32_t)));
            in.read(reinterpret_cast<char*>(sl.texels.data()), static_cast<std::streamsize>(sl.texels.size() * sizeof(Vec4)));
            if (!in) {
                if (error) *error = "archivo de iluminacion incompleto: " + file.string();
                return false;
            }
        }
    }
    if (version >= 3) {
        std::uint32_t light_header[2] = {};
        float sun_direction[3] = {};
        in.read(reinterpret_cast<char*>(light_header), sizeof(light_header));
        in.read(reinterpret_cast<char*>(sun_direction), sizeof(sun_direction));
        if (!in || light_header[1] > (1u << 20)) {
            if (error) *error = "archivo de iluminacion danado: " + file.string();
            return false;
        }
        result.sun_baked = light_header[0] != 0;
        result.sun_direction = Vec3{sun_direction[0], sun_direction[1], sun_direction[2]};
        result.baked_lights.resize(light_header[1]);
        for (auto& id : result.baked_lights) in.read(reinterpret_cast<char*>(id.data()), sizeof(std::uint64_t) * 2);
        if (!in) {
            if (error) *error = "archivo de iluminacion incompleto: " + file.string();
            return false;
        }
    }
    // Lo de siempre: el sol (si se horneo) va entero en las sondas.
    result.version = version;
    result.sun_in_probes = true;
    return true;
}

// --- v4 ------------------------------------------------------------------------

class ChunkWriter {
public:
    explicit ChunkWriter(std::ofstream& out) : out_(out) {}
    // `compress`: zstd si el trozo es grande.
    bool add(const char id[4], const std::vector<std::uint8_t>& raw, bool compress) {
        TocEntry entry;
        std::memcpy(entry.id, id, 4);
        entry.offset = position_;
        entry.raw = raw.size();
        const std::uint8_t* stored = raw.data();
        std::size_t stored_size = raw.size();
        std::vector<std::uint8_t> packed;
        if (compress && raw.size() > kCompressAbove) {
            packed.resize(ZSTD_compressBound(raw.size()));
            const std::size_t size = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 6);
            if (!ZSTD_isError(size) && size < raw.size()) {
                stored = packed.data();
                stored_size = size;
                entry.flags |= kChunkZstd;
            }
        }
        entry.stored = stored_size;
        entry.hash = fnv(stored, stored_size);
        if (stored_size > 0) out_.write(reinterpret_cast<const char*>(stored), static_cast<std::streamsize>(stored_size));
        position_ += stored_size;
        toc_.push_back(entry);
        return static_cast<bool>(out_);
    }
    std::uint64_t position() const { return position_; }
    const std::vector<TocEntry>& toc() const { return toc_; }
    void setPosition(std::uint64_t p) { position_ = p; }

private:
    std::ofstream& out_;
    std::uint64_t position_ = 0;
    std::vector<TocEntry> toc_;
};

std::vector<std::uint8_t> encodeRecords(const std::vector<gfx::LightmapRecord>& records) {
    Bytes b;
    b.put(static_cast<std::uint32_t>(records.size()));
    for (const gfx::LightmapRecord& r : records) {
        b.put(r.entity[0]);
        b.put(r.entity[1]);
        b.put(r.model[0]);
        b.put(r.model[1]);
        b.put(r.part);
        b.put(r.page);
        b.put(r.scale_offset.x);
        b.put(r.scale_offset.y);
        b.put(r.scale_offset.z);
        b.put(r.scale_offset.w);
        b.put(r.layout_hash);
        b.putRaw(r.world.data(), sizeof(float) * 12);
    }
    return std::move(b.data);
}

bool decodeRecords(const std::vector<std::uint8_t>& raw, std::vector<gfx::LightmapRecord>& records) {
    Cursor c{raw.data(), raw.size()};
    std::uint32_t count = 0;
    if (!c.get(count) || static_cast<std::uint64_t>(count) * kRecordBytes + 4 > raw.size()) return false;
    records.resize(count);
    for (gfx::LightmapRecord& r : records) {
        if (!c.get(r.entity[0]) || !c.get(r.entity[1]) || !c.get(r.model[0]) || !c.get(r.model[1]) || !c.get(r.part) ||
            !c.get(r.page) || !c.get(r.scale_offset.x) || !c.get(r.scale_offset.y) || !c.get(r.scale_offset.z) ||
            !c.get(r.scale_offset.w) || !c.get(r.layout_hash) || !c.getRaw(r.world.data(), sizeof(float) * 12)) {
            return false;
        }
    }
    return true;
}

bool saveV4(std::ofstream& out, const gfx::BakedLighting& data, gfx::LightingMode mode, const BakedLightingExtras* extras) {
    // Cabecera provisional (la tabla se apunta al final).
    std::uint8_t header[kHeaderBytes] = {};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    ChunkWriter chunks(out);
    chunks.setPosition(kHeaderBytes);
    {
        Bytes b;
        b.putString(extras != nullptr ? extras->engine_version : std::string());
        b.put(data.scene_uuid[0]);
        b.put(data.scene_uuid[1]);
        b.put(data.world_origin[0]);
        b.put(data.world_origin[1]);
        b.put(data.world_origin[2]);
        b.putString(extras != nullptr ? extras->settings_json : std::string());
        const Vec3 dir = extras != nullptr ? extras->sun_direction : data.sun_direction;
        const Vec3 color = extras != nullptr ? extras->sun_color : Vec3{};
        b.put(dir.x);
        b.put(dir.y);
        b.put(dir.z);
        b.put(color.x);
        b.put(color.y);
        b.put(color.z);
        b.put(extras != nullptr ? extras->sun_intensity : 0.0f);
        b.put(extras != nullptr ? extras->sun_mode : 0u);
        if (!chunks.add("INFO", b.data, false)) return false;
    }
    {
        Bytes b;
        b.put(static_cast<std::uint32_t>(data.volumes.size()));
        for (const gfx::BakedProbeVolume& v : data.volumes) {
            const float f[7] = {v.min.x, v.min.y, v.min.z, v.size.x, v.size.y, v.size.z, v.intensity};
            const std::uint32_t u[4] = {v.nx, v.ny, v.nz, v.first};
            b.putRaw(f, sizeof(f));
            b.putRaw(u, sizeof(u));
        }
        if (!chunks.add("VOLS", b.data, false)) return false;
    }
    {
        Bytes b;
        b.put(static_cast<std::uint64_t>(data.probes.size()));
        b.putRaw(data.probes.data(), data.probes.size() * sizeof(Vec4));
        if (!chunks.add("PRBS", b.data, true)) return false;
    }
    {
        Bytes b;
        b.put(data.lightmaps.sun_visibility ? 1u : 0u);
        b.put(data.sun_direction.x);
        b.put(data.sun_direction.y);
        b.put(data.sun_direction.z);
        b.put(static_cast<std::uint32_t>(data.baked_lights.size()));
        for (const auto& id : data.baked_lights) {
            b.put(id[0]);
            b.put(id[1]);
        }
        b.put(data.sun_baked ? 1u : 0u);
        if (!chunks.add("LGTS", b.data, false)) return false;
    }
    const gfx::LightmapSet& lm = data.lightmaps;
    if (!lm.pages.empty()) {
        Bytes b;
        b.put(static_cast<std::uint32_t>(lm.pages.size()));
        b.put(lm.width);
        b.put(lm.height);
        b.put(0u);  // formato: RGBA16F
        b.put(lm.texels_per_meter);
        const float bounds[6] = {lm.bounds_min.x, lm.bounds_min.y, lm.bounds_min.z,
                                 lm.bounds_max.x, lm.bounds_max.y, lm.bounds_max.z};
        b.putRaw(bounds, sizeof(bounds));
        b.put(lm.has_irradiance ? 1u : 0u);
        if (!chunks.add("LMAP", b.data, false)) return false;
        for (std::uint32_t i = 0; i < lm.pages.size(); ++i) {
            const gfx::LightmapPage& page = lm.pages[i];
            Bytes p;
            p.put(i);
            p.put(page.width);
            p.put(page.height);
            p.putRaw(page.texels.data(), page.texels.size() * sizeof(std::uint16_t));
            if (!chunks.add("PAGE", p.data, true)) return false;
        }
        if (!chunks.add("LMRC", encodeRecords(lm.records), true)) return false;
    }
    if (extras != nullptr) {
        for (const BakedLightingExtras::Layout& l : extras->layouts) {
            if (!l.layout) continue;
            const std::vector<std::uint8_t> raw = encodeLightmapUvLayout(*l.layout);
            std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
            const std::size_t size = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 6);
            if (ZSTD_isError(size)) return false;
            Bytes b;
            b.put(l.model.high);
            b.put(l.model.low);
            b.put(static_cast<std::int32_t>(l.part));
            b.put(static_cast<std::uint64_t>(raw.size()));
            b.putRaw(packed.data(), size);
            // El trozo va sin comprimir entero: dentro, la disposicion ya va
            // en zstd y la cabecera (modelo, pieza) se lee sin descomprimir.
            if (!chunks.add("UVLY", b.data, false)) return false;
        }
    }
    // Tabla de contenido.
    const std::uint64_t toc_offset = chunks.position();
    for (const TocEntry& e : chunks.toc()) {
        Bytes b;
        b.putRaw(e.id, 4);
        b.put(e.flags);
        b.put(e.offset);
        b.put(e.stored);
        b.put(e.raw);
        b.put(e.hash);
        out.write(reinterpret_cast<const char*>(b.data.data()), static_cast<std::streamsize>(b.data.size()));
    }
    // Cabecera de verdad.
    std::uint32_t flags = 0;
    if (lm.sun_visibility) flags |= kFlagSunVisibility;
    if (!lm.pages.empty()) flags |= kFlagLightmaps;
    if (lm.has_irradiance) flags |= kFlagIrradiance;
    Bytes h;
    h.putRaw(kMagic, 4);
    h.put(kBakedLightingVersion);
    h.put(static_cast<std::uint32_t>(mode));
    h.put(flags);
    h.put(toc_offset);
    h.put(static_cast<std::uint32_t>(chunks.toc().size()));
    h.put(0u);
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(h.data.data()), static_cast<std::streamsize>(h.data.size()));
    return static_cast<bool>(out);
}

// Lee un trozo entero (descomprimido si hace falta) y comprueba su huella.
bool readChunk(std::ifstream& in, const TocEntry& e, std::vector<std::uint8_t>& raw) {
    if (e.stored > (1ull << 34) || e.raw > (1ull << 34)) return false;
    std::vector<std::uint8_t> stored(static_cast<std::size_t>(e.stored));
    in.clear();
    in.seekg(static_cast<std::streamoff>(e.offset));
    if (e.stored > 0) in.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(e.stored));
    if (!in || fnv(stored.data(), stored.size()) != e.hash) return false;
    if ((e.flags & kChunkZstd) == 0) {
        raw = std::move(stored);
        return true;
    }
    raw.resize(static_cast<std::size_t>(e.raw));
    const std::size_t got = ZSTD_decompress(raw.data(), raw.size(), stored.data(), stored.size());
    return !ZSTD_isError(got) && got == raw.size();
}

bool readToc(std::ifstream& in, std::uint64_t offset, std::uint32_t count, std::uint64_t file_size, std::vector<TocEntry>& toc) {
    if (count > (1u << 20) || offset + static_cast<std::uint64_t>(count) * kTocEntryBytes > file_size) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count) * kTocEntryBytes);
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) return false;
    Cursor c{bytes.data(), bytes.size()};
    toc.resize(count);
    for (TocEntry& e : toc) {
        if (!c.getRaw(e.id, 4) || !c.get(e.flags) || !c.get(e.offset) || !c.get(e.stored) || !c.get(e.raw) || !c.get(e.hash)) {
            return false;
        }
        if (e.offset + e.stored > file_size) return false;
    }
    return true;
}

struct V4Header {
    std::uint32_t version = 0, mode = 0, flags = 0;
    std::uint64_t toc_offset = 0;
    std::uint32_t toc_count = 0;
};

bool readV4Header(std::ifstream& in, V4Header& h) {
    std::uint8_t bytes[kHeaderBytes] = {};
    in.clear();
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!in || std::memcmp(bytes, kMagic, 4) != 0) return false;
    Cursor c{bytes, sizeof(bytes), 4};
    std::uint32_t zero = 0;
    return c.get(h.version) && c.get(h.mode) && c.get(h.flags) && c.get(h.toc_offset) && c.get(h.toc_count) && c.get(zero);
}

bool loadV4(std::ifstream& in, std::uint64_t file_size, const V4Header& header, gfx::BakedLighting& result,
            const std::filesystem::path& file, LightmapUvSet* uvs, BakedLightingExtras* extras, std::string* error) {
    const auto damaged = [&]() {
        if (error) *error = "archivo de iluminacion danado: " + file.string();
        return false;
    };
    std::vector<TocEntry> toc;
    if (!readToc(in, header.toc_offset, header.toc_count, file_size, toc)) return damaged();
    result.version = header.version;
    result.sun_in_probes = false;
    gfx::LightmapSet& lm = result.lightmaps;
    std::uint32_t page_count = 0;
    std::vector<std::uint8_t> raw;
    for (const TocEntry& e : toc) {
        if (sameId(e.id, "UVLY")) {
            // Solo la cabecera (modelo, pieza): la disposicion se lee al cargar el modelo.
            if (uvs == nullptr && extras == nullptr) continue;
            if (e.stored < 28 || (e.flags & kChunkZstd) != 0) return damaged();
            std::uint8_t head[28] = {};
            in.clear();
            in.seekg(static_cast<std::streamoff>(e.offset));
            in.read(reinterpret_cast<char*>(head), sizeof(head));
            if (!in) return damaged();
            Cursor c{head, sizeof(head)};
            Uuid model{};
            std::int32_t part = 0;
            std::uint64_t raw_size = 0;
            c.get(model.high);
            c.get(model.low);
            c.get(part);
            c.get(raw_size);
            if (uvs != nullptr) {
                LightmapUvSet::Source source;
                source.file = file;
                source.offset = e.offset + 28;
                source.stored = e.stored - 28;
                source.raw = raw_size;
                source.zstd = true;
                uvs->parts[LightmapUvPartKey{model, part}] = source;
            }
            if (extras != nullptr) extras->layouts.push_back(BakedLightingExtras::Layout{model, part, nullptr});
            continue;
        }
        if (!readChunk(in, e, raw)) return damaged();
        Cursor c{raw.data(), raw.size()};
        if (sameId(e.id, "INFO")) {
            std::string engine, settings;
            if (!c.getString(engine) || !c.get(result.scene_uuid[0]) || !c.get(result.scene_uuid[1]) ||
                !c.get(result.world_origin[0]) || !c.get(result.world_origin[1]) || !c.get(result.world_origin[2]) ||
                !c.getString(settings)) {
                return damaged();
            }
            if (extras != nullptr) {
                extras->engine_version = engine;
                extras->settings_json = settings;
                c.get(extras->sun_direction.x);
                c.get(extras->sun_direction.y);
                c.get(extras->sun_direction.z);
                c.get(extras->sun_color.x);
                c.get(extras->sun_color.y);
                c.get(extras->sun_color.z);
                c.get(extras->sun_intensity);
                c.get(extras->sun_mode);
            }
        } else if (sameId(e.id, "VOLS")) {
            std::uint32_t count = 0;
            if (!c.get(count) || count > 1024) return damaged();
            for (std::uint32_t i = 0; i < count; ++i) {
                float f[7] = {};
                std::uint32_t u[4] = {};
                if (!c.getRaw(f, sizeof(f)) || !c.getRaw(u, sizeof(u))) return damaged();
                gfx::BakedProbeVolume v;
                v.min = Vec3{f[0], f[1], f[2]};
                v.size = Vec3{f[3], f[4], f[5]};
                v.intensity = f[6];
                v.nx = u[0];
                v.ny = u[1];
                v.nz = u[2];
                v.first = u[3];
                result.volumes.push_back(v);
            }
        } else if (sameId(e.id, "PRBS")) {
            std::uint64_t count = 0;
            if (!c.get(count) || count > (1ull << 28) || count * sizeof(Vec4) + 8 > raw.size()) return damaged();
            result.probes.resize(static_cast<std::size_t>(count));
            if (!c.getRaw(result.probes.data(), static_cast<std::size_t>(count) * sizeof(Vec4))) return damaged();
        } else if (sameId(e.id, "LGTS")) {
            std::uint32_t visibility = 0, count = 0;
            if (!c.get(visibility) || !c.get(result.sun_direction.x) || !c.get(result.sun_direction.y) ||
                !c.get(result.sun_direction.z) || !c.get(count) || count > (1u << 20)) {
                return damaged();
            }
            result.baked_lights.resize(count);
            for (auto& id : result.baked_lights) {
                if (!c.get(id[0]) || !c.get(id[1])) return damaged();
            }
            std::uint32_t sun_baked = 0;
            if (c.get(sun_baked)) result.sun_baked = sun_baked != 0;
            lm.sun_visibility = visibility != 0;
        } else if (sameId(e.id, "LMAP")) {
            std::uint32_t format = 0, irradiance = 0;
            float bounds[6] = {};
            if (!c.get(page_count) || !c.get(lm.width) || !c.get(lm.height) || !c.get(format) || !c.get(lm.texels_per_meter) ||
                !c.getRaw(bounds, sizeof(bounds)) || !c.get(irradiance) || format != 0 || page_count > 2048 ||
                lm.width > 16384 || lm.height > 16384) {
                return damaged();
            }
            lm.bounds_min = Vec3{bounds[0], bounds[1], bounds[2]};
            lm.bounds_max = Vec3{bounds[3], bounds[4], bounds[5]};
            lm.has_irradiance = irradiance != 0;
            lm.pages.resize(page_count);
        } else if (sameId(e.id, "PAGE")) {
            std::uint32_t index = 0, width = 0, height = 0;
            if (!c.get(index) || !c.get(width) || !c.get(height) || index >= lm.pages.size() || width > lm.width ||
                height > lm.height) {
                return damaged();
            }
            gfx::LightmapPage& page = lm.pages[index];
            page.width = width;
            page.height = height;
            page.texels.resize(static_cast<std::size_t>(width) * height * 4);
            if (!c.getRaw(page.texels.data(), page.texels.size() * sizeof(std::uint16_t))) return damaged();
        } else if (sameId(e.id, "LMRC")) {
            if (!decodeRecords(raw, lm.records)) return damaged();
        }
        // Trozos desconocidos (de una version menor posterior): se saltan.
    }
    for (const gfx::LightmapPage& page : lm.pages) {
        if (page.texels.empty()) return damaged();  // falta alguna pagina
    }
    if (header.flags & kFlagSunVisibility) lm.sun_visibility = true;
    return true;
}

}  // namespace

bool saveBakedLighting(const std::filesystem::path& file, const gfx::BakedLighting& data, gfx::LightingMode mode,
                       std::string* error, const BakedLightingExtras* extras) {
    const std::filesystem::path temporary = file.string() + ".tmp";
    bool ok = false;
    if (data.version <= kLegacyVersion) {
        ok = saveLegacy(temporary, data, mode);
    } else {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        ok = out && saveV4(out, data, mode, extras);
    }
    if (!ok) {
        if (error) *error = "no se pudo escribir " + file.string();
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool loadBakedLighting(const std::filesystem::path& file, gfx::BakedLighting& data, gfx::LightingMode& mode,
                       std::string* error, LightmapUvSet* uvs, BakedLightingExtras* extras) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no existe " + file.string();
        return false;
    }
    char magic[4] = {};
    in.read(magic, 4);
    std::uint32_t header[3] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || header[0] < 1 || header[0] > kBakedLightingVersion) {
        if (error) *error = "archivo de iluminacion danado: " + file.string();
        return false;
    }
    gfx::BakedLighting result;
    mode = header[1] == 1 ? gfx::LightingMode::Baked : gfx::LightingMode::Realtime;
    if (header[0] <= kLegacyVersion) {
        if (header[2] > 1024) {
            if (error) *error = "archivo de iluminacion danado: " + file.string();
            return false;
        }
        if (!loadLegacy(in, header[0], header[2], result, file, error)) return false;
    } else {
        std::error_code ec;
        const std::uint64_t size = std::filesystem::file_size(file, ec);
        V4Header h;
        if (ec || !readV4Header(in, h) || h.version != kBakedLightingVersion) {
            if (error) *error = "archivo de iluminacion danado: " + file.string();
            return false;
        }
        if (uvs != nullptr) uvs->parts.clear();
        if (!loadV4(in, size, h, result, file, uvs, extras, error)) return false;
    }
    data = std::move(result);
    return true;
}

bool loadLightmapRecords(const std::filesystem::path& file, std::vector<gfx::LightmapRecord>& records) {
    records.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    V4Header h;
    if (!readV4Header(in, h)) return false;
    if (h.version < kBakedLightingVersion) return true;  // v1-v3: sin lightmaps
    if (h.version != kBakedLightingVersion) return false;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(file, ec);
    std::vector<TocEntry> toc;
    if (ec || !readToc(in, h.toc_offset, h.toc_count, size, toc)) return false;
    for (const TocEntry& e : toc) {
        if (!sameId(e.id, "LMRC")) continue;
        std::vector<std::uint8_t> raw;
        return readChunk(in, e, raw) && decodeRecords(raw, records);
    }
    return true;
}

bool setBakedLightingMode(const std::filesystem::path& file, gfx::LightingMode mode, std::string* error) {
    std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) {
        if (error) *error = "no se pudo abrir " + file.string();
        return false;
    }
    char magic[4] = {};
    std::uint32_t version = 0;
    io.read(magic, 4);
    io.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!io || std::memcmp(magic, kMagic, 4) != 0 || version < 1 || version > kBakedLightingVersion) {
        if (error) *error = "archivo de iluminacion danado: " + file.string();
        return false;
    }
    const auto value = static_cast<std::uint32_t>(mode);
    io.seekp(8);
    io.write(reinterpret_cast<const char*>(&value), sizeof(value));
    io.flush();
    if (!io) {
        if (error) *error = "no se pudo escribir " + file.string();
        return false;
    }
    return true;
}

}  // namespace cramion::lighting
