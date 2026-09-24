#include "CramionFX/asset/ObjLoader.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cramion::asset {
namespace {

using core::Vec2;
using core::Vec3;
using core::Vec4;

// --- Utilidades de texto -------------------------------------------------------

std::vector<char> readWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("No se pudo abrir " + path.string());
    }
    const std::streamsize size = file.tellg();
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), size);
    return bytes;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// Cursor sobre una linea: devuelve tokens separados por espacios.
struct LineReader {
    const char* p;
    const char* end;

    void skipSpaces() {
        while (p < end && isSpace(*p)) {
            ++p;
        }
    }

    std::string_view token() {
        skipSpaces();
        const char* start = p;
        while (p < end && !isSpace(*p)) {
            ++p;
        }
        return {start, static_cast<std::size_t>(p - start)};
    }

    std::string_view rest() {
        skipSpaces();
        const char* last = end;
        while (last > p && isSpace(*(last - 1))) {
            --last;
        }
        return {p, static_cast<std::size_t>(last - p)};
    }

    float number() {
        skipSpaces();
        float value = 0.0f;
        const auto result = std::from_chars(p, end, value);
        p = result.ptr;
        return value;
    }
};

// --- Parseo en paralelo --------------------------------------------------------

// Esquina de un triangulo: indices 0-based a posicion, UV y normal (-1 = no
// tiene). Los indices negativos del OBJ (relativos) se resuelven dentro del
// trozo y se marcan con kLocal para sumarles el desplazamiento del trozo.
struct Corner {
    std::int32_t v = -1;
    std::int32_t vt = -1;
    std::int32_t vn = -1;
};

constexpr std::uint32_t kLocal = 0x40000000u;

// Cambio de material dentro de un trozo: a partir del triangulo `triangle`
// (del trozo) se usa `material`.
struct MaterialSwitch {
    std::size_t triangle = 0;
    std::string material;
};

struct Chunk {
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::vector<Corner> corners;  // 3 por triangulo
    std::vector<MaterialSwitch> switches;
    std::vector<std::string> mtllibs;
};

std::int32_t parseIndex(std::string_view text, std::size_t local_count) {
    if (text.empty()) {
        return -1;
    }
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    if (value > 0) {
        return value - 1;
    }
    if (value < 0) {
        const auto local = static_cast<std::int64_t>(local_count) + value;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(local) | kLocal);
    }
    return -1;
}

Corner parseCorner(std::string_view token, const Chunk& chunk) {
    Corner corner{};
    const std::size_t first = token.find('/');
    corner.v = parseIndex(token.substr(0, first), chunk.positions.size());
    if (first == std::string_view::npos) {
        return corner;
    }
    const std::string_view rest = token.substr(first + 1);
    const std::size_t second = rest.find('/');
    corner.vt = parseIndex(rest.substr(0, second), chunk.texcoords.size());
    if (second != std::string_view::npos) {
        corner.vn = parseIndex(rest.substr(second + 1), chunk.normals.size());
    }
    return corner;
}

void parseChunk(const char* begin, const char* end, Chunk& chunk) {
    const char* line = begin;
    std::vector<Corner> polygon;

    while (line < end) {
        const char* line_end = static_cast<const char*>(
            std::memchr(line, '\n', static_cast<std::size_t>(end - line)));
        if (line_end == nullptr) {
            line_end = end;
        }

        LineReader reader{line, line_end};
        const std::string_view keyword = reader.token();

        if (keyword == "v") {
            const float x = reader.number();
            const float y = reader.number();
            const float z = reader.number();
            chunk.positions.push_back(Vec3{x, y, z});
        } else if (keyword == "vt") {
            const float u = reader.number();
            const float v = reader.number();
            chunk.texcoords.push_back(Vec2{u, v});
        } else if (keyword == "vn") {
            const float x = reader.number();
            const float y = reader.number();
            const float z = reader.number();
            chunk.normals.push_back(Vec3{x, y, z});
        } else if (keyword == "f") {
            polygon.clear();
            for (std::string_view token = reader.token(); !token.empty();
                 token = reader.token()) {
                polygon.push_back(parseCorner(token, chunk));
            }
            // Poligonos en abanico.
            for (std::size_t i = 2; i < polygon.size(); ++i) {
                chunk.corners.push_back(polygon[0]);
                chunk.corners.push_back(polygon[i - 1]);
                chunk.corners.push_back(polygon[i]);
            }
        } else if (keyword == "usemtl") {
            chunk.switches.push_back(
                MaterialSwitch{chunk.corners.size() / 3, std::string(reader.rest())});
        } else if (keyword == "mtllib") {
            chunk.mtllibs.emplace_back(reader.rest());
        }

        line = line_end + 1;
    }
}

// --- MTL ---------------------------------------------------------------------

struct ObjMaterial {
    std::string name;
    Vec3 diffuse{0.8f, 0.8f, 0.8f};
    Vec3 specular{0.04f, 0.04f, 0.04f};
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float shininess = 0.0f;
    float opacity = 1.0f;
    Vec3 transmission{1.0f, 1.0f, 1.0f};  // Tf
    int illumination = 2;                 // illum
    std::string map_diffuse;
    std::string map_bump;
    std::string map_normal;
    std::string map_emissive;
};

// Ultimo token de la linea: el nombre de archivo va detras de las opciones
// ("map_Bump -bm 1.0 archivo.png").
std::string lastToken(LineReader reader) {
    std::string_view last;
    for (std::string_view token = reader.token(); !token.empty(); token = reader.token()) {
        last = token;
    }
    return std::string(last);
}

std::vector<ObjMaterial> parseMtl(const std::filesystem::path& path) {
    std::vector<ObjMaterial> materials;
    std::ifstream file(path);
    if (!file) {
        std::cerr << "[OBJ] No se encontro la biblioteca de materiales " << path.string() << "\n";
        return materials;
    }

    std::string text;
    while (std::getline(file, text)) {
        LineReader reader{text.data(), text.data() + text.size()};
        std::string keyword(reader.token());
        std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (keyword == "newmtl") {
            materials.push_back(ObjMaterial{});
            materials.back().name = std::string(reader.rest());
            continue;
        }
        if (materials.empty()) {
            continue;
        }
        ObjMaterial& m = materials.back();

        if (keyword == "kd") {
            m.diffuse = Vec3{reader.number(), reader.number(), reader.number()};
        } else if (keyword == "ks") {
            m.specular = Vec3{reader.number(), reader.number(), reader.number()};
        } else if (keyword == "ke") {
            m.emissive = Vec3{reader.number(), reader.number(), reader.number()};
        } else if (keyword == "ns") {
            m.shininess = reader.number();
        } else if (keyword == "d") {
            m.opacity = reader.number();
        } else if (keyword == "tr") {
            m.opacity = 1.0f - reader.number();
        } else if (keyword == "tf") {
            const float r = reader.number();
            const float g = reader.number();
            const float b = reader.number();
            m.transmission = Vec3{r, g, b};
        } else if (keyword == "illum") {
            m.illumination = static_cast<int>(reader.number());
        } else if (keyword == "map_kd") {
            m.map_diffuse = lastToken(reader);
        } else if (keyword == "map_bump" || keyword == "bump") {
            m.map_bump = lastToken(reader);
        } else if (keyword == "norm" || keyword == "map_kn") {
            m.map_normal = lastToken(reader);
        } else if (keyword == "map_ke") {
            m.map_emissive = lastToken(reader);
        }
    }
    return materials;
}

// Rugosidad a partir del exponente de Phong: alpha = sqrt(2 / (Ns + 2)),
// rugosidad = sqrt(alpha). Minimo 0.3: los exponentes de estos formatos se
// exageran a menudo (el propio San Miguel avisa: "some surfaces are too
// glossy"). Sin brillo especular, mate.
float roughnessFromPhong(const ObjMaterial& m) {
    if (m.shininess <= 0.0f) {
        return 0.8f;
    }
    if (std::max({m.specular.x, m.specular.y, m.specular.z}) < 0.02f) {
        return 0.9f;
    }
    const float alpha = std::sqrt(2.0f / (m.shininess + 2.0f));
    return std::clamp(std::sqrt(alpha), 0.3f, 1.0f);
}

bool startsWithNormalPrefix(const std::string& path) {
    const std::string file = std::filesystem::path(path).filename().string();
    return file.size() >= 2 && std::toupper(static_cast<unsigned char>(file[0])) == 'N' &&
           file[1] == '_';
}

// --- Construccion de la malla de un material ----------------------------------

// Lado de las celdas en que se parte cada material (metros). Da clusteres
// del tamano de un mueble grande o un tramo de pared: bastante finos para que
// el culling descarte mucho, y no tantos como para disparar las llamadas de
// dibujo.
constexpr float kClusterSize = 5.0f;

struct Cluster {
    std::uint32_t first_index = 0;  // dentro de MaterialMesh::indices
    std::uint32_t index_count = 0;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
};

struct MaterialMesh {
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;  // locales a `vertices`
    std::vector<Cluster> clusters;
};

// Reordena los triangulos de la malla por celdas del espacio (segun su
// centro) y guarda un cluster por celda con su caja.
void buildClusters(MaterialMesh& mesh) {
    struct Triangle {
        std::uint64_t cell;
        std::uint32_t first;
    };
    std::vector<Triangle> triangles;
    triangles.reserve(mesh.indices.size() / 3);

    const auto cell_coordinate = [](float value) {
        return static_cast<std::uint64_t>(
            static_cast<std::int64_t>(std::floor(value / kClusterSize)) + (1 << 20));
    };

    for (std::uint32_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3 center = (mesh.vertices[mesh.indices[i]].position +
                             mesh.vertices[mesh.indices[i + 1]].position +
                             mesh.vertices[mesh.indices[i + 2]].position) *
                            (1.0f / 3.0f);
        const std::uint64_t cell = (cell_coordinate(center.x) << 42) |
                                   (cell_coordinate(center.y) << 21) | cell_coordinate(center.z);
        triangles.push_back(Triangle{cell, i});
    }
    std::stable_sort(triangles.begin(), triangles.end(),
                     [](const Triangle& a, const Triangle& b) { return a.cell < b.cell; });

    std::vector<std::uint32_t> sorted;
    sorted.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        if (t == 0 || triangles[t].cell != triangles[t - 1].cell) {
            Cluster cluster{};
            cluster.first_index = static_cast<std::uint32_t>(sorted.size());
            cluster.bounds_min = Vec3{1e30f, 1e30f, 1e30f};
            cluster.bounds_max = Vec3{-1e30f, -1e30f, -1e30f};
            mesh.clusters.push_back(cluster);
        }
        Cluster& cluster = mesh.clusters.back();
        for (std::uint32_t k = 0; k < 3; ++k) {
            const std::uint32_t index = mesh.indices[triangles[t].first + k];
            sorted.push_back(index);
            const Vec3& p = mesh.vertices[index].position;
            cluster.bounds_min = Vec3{std::min(cluster.bounds_min.x, p.x),
                                      std::min(cluster.bounds_min.y, p.y),
                                      std::min(cluster.bounds_min.z, p.z)};
            cluster.bounds_max = Vec3{std::max(cluster.bounds_max.x, p.x),
                                      std::max(cluster.bounds_max.y, p.y),
                                      std::max(cluster.bounds_max.z, p.z)};
        }
        cluster.index_count += 3;
    }
    mesh.indices = std::move(sorted);
}

struct CornerHash {
    std::size_t operator()(const Corner& c) const {
        std::uint64_t h = static_cast<std::uint32_t>(c.v);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(c.vt);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(c.vn);
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

struct CornerEqual {
    bool operator()(const Corner& a, const Corner& b) const {
        return a.v == b.v && a.vt == b.vt && a.vn == b.vn;
    }
};

MaterialMesh buildMesh(const std::vector<Corner>& corners, const std::vector<Vec3>& positions,
                       const std::vector<Vec2>& texcoords, const std::vector<Vec3>& normals) {
    MaterialMesh mesh;
    mesh.indices.reserve(corners.size());

    // --- Vertices unicos (misma posicion, UV y normal) ---
    std::unordered_map<Corner, std::uint32_t, CornerHash, CornerEqual> unique;
    unique.reserve(corners.size() / 2);

    for (const Corner& corner : corners) {
        const auto [it, inserted] =
            unique.try_emplace(corner, static_cast<std::uint32_t>(mesh.vertices.size()));
        if (inserted) {
            SkinnedVertex vertex{};
            vertex.position = positions[static_cast<std::size_t>(corner.v)];
            if (corner.vn >= 0) {
                vertex.normal = normals[static_cast<std::size_t>(corner.vn)];
            }
            if (corner.vt >= 0) {
                const Vec2 uv = texcoords[static_cast<std::size_t>(corner.vt)];
                // Origen de las UV arriba: las imagenes se suben empezando por
                // la fila de arriba (lo mismo que FlipUVs de assimp).
                vertex.uv = Vec2{uv.x, 1.0f - uv.y};
            }
            vertex.joints[0] = 0;
            vertex.weights[0] = 1.0f;
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.push_back(it->second);
    }

    // --- Normales que falten: las de las caras, acumuladas ---
    const bool needs_normals = std::any_of(corners.begin(), corners.end(),
                                           [](const Corner& c) { return c.vn < 0; });

    // --- Tangentes: derivadas de la posicion respecto a las UV por triangulo,
    // acumuladas por vertice y ortonormalizadas. Mismo convenio que
    // aiProcess_CalcTangentSpace despues de FlipUVs (lo que espera
    // skinned.frag). ---
    std::vector<Vec3> tangents(mesh.vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
    std::vector<Vec3> bitangents(mesh.vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
    std::vector<Vec3> face_normals;
    if (needs_normals) {
        face_normals.assign(mesh.vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
    }

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t a = mesh.indices[i];
        const std::uint32_t b = mesh.indices[i + 1];
        const std::uint32_t c = mesh.indices[i + 2];
        const SkinnedVertex& va = mesh.vertices[a];
        const SkinnedVertex& vb = mesh.vertices[b];
        const SkinnedVertex& vc = mesh.vertices[c];

        const Vec3 e1 = vb.position - va.position;
        const Vec3 e2 = vc.position - va.position;

        if (needs_normals) {
            const Vec3 n = core::cross(e1, e2);
            face_normals[a] += n;
            face_normals[b] += n;
            face_normals[c] += n;
        }

        const float du1 = vb.uv.x - va.uv.x;
        const float dv1 = vb.uv.y - va.uv.y;
        const float du2 = vc.uv.x - va.uv.x;
        const float dv2 = vc.uv.y - va.uv.y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-12f) {
            continue;
        }
        const float r = 1.0f / det;
        const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
        const Vec3 bt = (e2 * du1 - e1 * du2) * r;
        for (const std::uint32_t v : {a, b, c}) {
            tangents[v] += t;
            bitangents[v] += bt;
        }
    }

    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        SkinnedVertex& vertex = mesh.vertices[v];
        if (needs_normals && core::dot(vertex.normal, vertex.normal) < 1e-12f) {
            vertex.normal = face_normals[v];
        }
        const float normal_length = core::length(vertex.normal);
        vertex.normal = (normal_length > 1e-12f) ? vertex.normal * (1.0f / normal_length)
                                                 : Vec3{0.0f, 1.0f, 0.0f};

        // Gram-Schmidt contra la normal.
        Vec3 t = tangents[v] - vertex.normal * core::dot(vertex.normal, tangents[v]);
        const float t_length = core::length(t);
        if (t_length < 1e-12f) {
            vertex.tangent = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
            continue;
        }
        t = t * (1.0f / t_length);
        const float handedness =
            core::dot(core::cross(vertex.normal, t), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = Vec4{t.x, t.y, t.z, handedness};
    }

    buildClusters(mesh);
    return mesh;
}

template <typename Function>
void parallelFor(std::size_t count, Function&& function) {
    std::atomic<std::size_t> next{0};
    const unsigned int thread_count = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&]() {
            for (std::size_t i = next++; i < count; i = next++) {
                function(i);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

}  // namespace

ModelData loadObj(const std::filesystem::path& path) {
    const std::filesystem::path directory = path.parent_path();
    std::vector<char> text = readWholeFile(path);

    // --- 1) Trozos que acaban en fin de linea, parseados en paralelo ---
    const unsigned int thread_count = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    const std::size_t chunk_count = std::max<std::size_t>(thread_count * 4, 1);
    std::vector<const char*> bounds{text.data()};
    for (std::size_t i = 1; i < chunk_count; ++i) {
        const char* guess = text.data() + text.size() * i / chunk_count;
        const char* end = text.data() + text.size();
        const char* newline = static_cast<const char*>(
            std::memchr(guess, '\n', static_cast<std::size_t>(end - guess)));
        const char* split = (newline != nullptr) ? newline + 1 : end;
        if (split > bounds.back()) {
            bounds.push_back(split);
        }
    }
    bounds.push_back(text.data() + text.size());

    std::vector<Chunk> chunks(bounds.size() - 1);
    parallelFor(chunks.size(), [&](std::size_t i) { parseChunk(bounds[i], bounds[i + 1], chunks[i]); });
    text = {};  // El texto (1 GB en el San Miguel) ya no hace falta.

    // --- 2) Union: arrays globales y resolucion de indices relativos ---
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::size_t corner_total = 0;
    for (const Chunk& chunk : chunks) {
        corner_total += chunk.corners.size();
    }

    // Materiales del MTL (el primer mtllib que aparezca).
    std::vector<ObjMaterial> obj_materials;
    for (const Chunk& chunk : chunks) {
        if (!chunk.mtllibs.empty()) {
            obj_materials = parseMtl(directory / chunk.mtllibs.front());
            break;
        }
    }
    std::unordered_map<std::string, std::uint32_t> material_index;
    for (std::uint32_t i = 0; i < obj_materials.size(); ++i) {
        material_index.try_emplace(obj_materials[i].name, i);
    }
    // Material por defecto para las caras sin usemtl o con uno desconocido.
    const auto default_material = static_cast<std::uint32_t>(obj_materials.size());

    // Triangulos agrupados por material.
    std::vector<std::vector<Corner>> by_material(obj_materials.size() + 1);
    std::uint32_t current = default_material;

    for (Chunk& chunk : chunks) {
        const auto v_offset = static_cast<std::uint32_t>(positions.size());
        const auto vt_offset = static_cast<std::uint32_t>(texcoords.size());
        const auto vn_offset = static_cast<std::uint32_t>(normals.size());
        const auto resolve = [](std::int32_t index, std::uint32_t offset) {
            if (index < 0) {
                return index;
            }
            const auto raw = static_cast<std::uint32_t>(index);
            return (raw & kLocal) != 0 ? static_cast<std::int32_t>((raw & ~kLocal) + offset)
                                       : index;
        };

        std::size_t next_switch = 0;
        const std::size_t triangles = chunk.corners.size() / 3;
        for (std::size_t t = 0; t < triangles; ++t) {
            while (next_switch < chunk.switches.size() &&
                   chunk.switches[next_switch].triangle == t) {
                const auto it = material_index.find(chunk.switches[next_switch].material);
                current = (it != material_index.end()) ? it->second : default_material;
                ++next_switch;
            }
            for (std::size_t k = 0; k < 3; ++k) {
                Corner corner = chunk.corners[t * 3 + k];
                corner.v = resolve(corner.v, v_offset);
                corner.vt = resolve(corner.vt, vt_offset);
                corner.vn = resolve(corner.vn, vn_offset);
                by_material[current].push_back(corner);
            }
        }
        // Un usemtl al final del trozo afecta al siguiente.
        while (next_switch < chunk.switches.size()) {
            const auto it = material_index.find(chunk.switches[next_switch].material);
            current = (it != material_index.end()) ? it->second : default_material;
            ++next_switch;
        }

        positions.insert(positions.end(), chunk.positions.begin(), chunk.positions.end());
        texcoords.insert(texcoords.end(), chunk.texcoords.begin(), chunk.texcoords.end());
        normals.insert(normals.end(), chunk.normals.begin(), chunk.normals.end());
        chunk = Chunk{};  // Libera la memoria del trozo cuanto antes.
    }

    // Esquinas que apuntan fuera de los arrays (archivo danado): se descarta
    // el triangulo entero.
    for (std::vector<Corner>& corners : by_material) {
        std::vector<Corner> valid;
        valid.reserve(corners.size());
        for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
            bool ok = true;
            for (std::size_t k = 0; k < 3; ++k) {
                const Corner& c = corners[i + k];
                ok = ok && c.v >= 0 && static_cast<std::size_t>(c.v) < positions.size() &&
                     (c.vt < 0 || static_cast<std::size_t>(c.vt) < texcoords.size()) &&
                     (c.vn < 0 || static_cast<std::size_t>(c.vn) < normals.size());
            }
            if (ok) {
                valid.insert(valid.end(), corners.begin() + static_cast<std::ptrdiff_t>(i),
                             corners.begin() + static_cast<std::ptrdiff_t>(i + 3));
            }
        }
        corners = std::move(valid);
    }

    // --- 3) Una malla por material, en paralelo ---
    std::vector<MaterialMesh> meshes(by_material.size());
    parallelFor(by_material.size(), [&](std::size_t m) {
        if (!by_material[m].empty()) {
            meshes[m] = buildMesh(by_material[m], positions, texcoords, normals);
            by_material[m] = {};
        }
    });

    // --- 4) ModelData ---
    ModelData model{};
    model.name = path.filename().string();
    model.nodes.push_back(Node{"root", -1, core::Mat4::identity()});
    model.bones.push_back(Bone{"root", 0, core::Mat4::identity()});

    std::size_t vertex_total = 0;
    std::size_t index_total = 0;
    for (const MaterialMesh& mesh : meshes) {
        vertex_total += mesh.vertices.size();
        index_total += mesh.indices.size();
    }
    model.vertices.reserve(vertex_total);
    model.indices.reserve(index_total);

    // Texturas: se leen comprimidas, una vez por archivo.
    std::unordered_map<std::string, std::int32_t> texture_index;
    const auto load_texture = [&](std::string file, bool height_map = false) -> std::int32_t {
        if (file.empty()) {
            return -1;
        }
        std::replace(file.begin(), file.end(), '\\', '/');
        // Clave aparte para los mapas de alturas: el mismo archivo usado como
        // color no debe compartir la version convertida en normal map.
        const std::string key = height_map ? file + "#altura" : file;
        if (const auto it = texture_index.find(key); it != texture_index.end()) {
            return it->second;
        }
        TextureData texture{};
        texture.name = std::filesystem::path(file).filename().string();
        texture.height_map = height_map;
        // Solo la ruta: se lee despues, en paralelo con las demas.
        std::error_code error;
        if (std::filesystem::is_regular_file(directory / file, error)) {
            texture.source_path = (directory / file).string();
        }
        if (texture.source_path.empty()) {
            std::cerr << "[OBJ] No se pudo leer la textura " << file << "\n";
            texture_index.emplace(key, -1);
            return -1;
        }
        const auto index = static_cast<std::int32_t>(model.textures.size());
        model.textures.push_back(std::move(texture));
        texture_index.emplace(key, index);
        return index;
    };

    for (std::size_t m = 0; m < meshes.size(); ++m) {
        MaterialMesh& mesh = meshes[m];
        if (mesh.indices.empty()) {
            continue;
        }

        MaterialData material{};
        if (m < obj_materials.size()) {
            const ObjMaterial& source = obj_materials[m];
            material.name = source.name;
            material.albedo_texture = load_texture(source.map_diffuse);
            // Con textura, el Kd suele ser 1 o un gris por defecto: manda la
            // textura.
            if (material.albedo_texture < 0) {
                material.base_color =
                    Vec4{source.diffuse.x, source.diffuse.y, source.diffuse.z, 1.0f};
            }
            material.roughness = roughnessFromPhong(source);
            // Normal map: "norm", o un "map_Bump" que en realidad es un normal
            // map (en el San Miguel llevan el prefijo "N_"). Cualquier otro
            // "bump" es un mapa de alturas y se convierte en normal map al
            // decodificarlo (Sibenik).
            material.normal_texture = load_texture(source.map_normal);
            if (material.normal_texture < 0 && !source.map_bump.empty()) {
                material.normal_texture = startsWithNormalPrefix(source.map_bump)
                                              ? load_texture(source.map_bump)
                                              : load_texture(source.map_bump, true);
            }
            material.emissive_texture = load_texture(source.map_emissive);
            material.emissive = source.emissive;
            if (material.emissive_texture >= 0 && std::max({material.emissive.x, material.emissive.y,
                                                           material.emissive.z}) <= 0.0f) {
                material.emissive = Vec3{1.0f, 1.0f, 1.0f};
            }
            // Vidrio y agua: el diferido no los puede mezclar. Se reconocen
            // por la opacidad (d / Tr), por la transmision (Tf) o por los
            // modelos de iluminacion de vidrio de MTL (4, 6, 7, 9).
            const bool transmits = std::min({source.transmission.x, source.transmission.y,
                                             source.transmission.z}) < 0.99f;
            const bool glass_model = source.illumination == 4 || source.illumination == 6 ||
                                     source.illumination == 7 || source.illumination == 9;
            material.transparent = source.opacity < 0.99f || transmits || glass_model;
        } else {
            material.name = "Default";
        }

        // Una submalla por cluster, todas con el mismo material (seguidas: el
        // renderizador solo cambia de material al pasar de un grupo a otro).
        const auto first_index = static_cast<std::uint32_t>(model.indices.size());
        const auto material_slot = static_cast<std::uint32_t>(model.materials.size());
        for (const Cluster& cluster : mesh.clusters) {
            SubMesh submesh{};
            submesh.first_index = first_index + cluster.first_index;
            submesh.index_count = cluster.index_count;
            submesh.material = material_slot;
            submesh.bounds_min = cluster.bounds_min;
            submesh.bounds_max = cluster.bounds_max;
            model.submeshes.push_back(submesh);
        }

        const auto base = static_cast<std::uint32_t>(model.vertices.size());
        model.vertices.insert(model.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (const std::uint32_t index : mesh.indices) {
            model.indices.push_back(base + index);
        }
        mesh = MaterialMesh{};

        model.materials.push_back(std::move(material));
    }

    if (model.materials.empty()) {
        MaterialData fallback{};
        fallback.name = "Default";
        model.materials.push_back(fallback);
    }

    std::cout << "[OBJ] " << model.name << ": " << corner_total / 3 << " triangulos, "
              << model.vertices.size() << " vertices unicos, " << model.materials.size()
              << " materiales, " << model.submeshes.size() << " clusteres, "
              << model.textures.size() << " texturas\n";
    return model;
}

}  // namespace cramion::asset
