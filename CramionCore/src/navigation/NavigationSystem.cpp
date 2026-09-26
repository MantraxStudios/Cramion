// Sistema de navegacion: Recast (genera la malla), Detour (consultas) y
// DetourCrowd (agentes que se esquivan), como el RecastNavMesh de Unreal.
//
// Malla por baldosas (tiles) en una rejilla fija del mundo (origen 0, lado
// cell_size * tile_size). Cada frame:
//
//   1. Fuentes: cada entidad con colision estatica tiene una firma (matriz de
//      mundo + datos de sus colliders). Si cambia, sus triangulos se
//      recalculan y se marcan sucias las baldosas de su caja vieja y nueva.
//      Igual los volumenes (NavMeshBounds) y los modificadores.
//   2. Baldosas sucias: se juntan sus triangulos (hilo principal, solo los
//      de esa baldosa gracias a los "cubos" por baldosa de cada fuente) y se
//      mandan a los hilos de fondo, que hacen la tuberia de Recast y
//      devuelven los datos de Detour.
//   3. Las terminadas se meten en el dtNavMesh (hilo principal: ahi viven las
//      consultas y los agentes, sin cerrojos).
//   4. Agentes: DetourCrowd, y el resultado a los Transform (o a la
//      velocidad de su Rigidbody).
//
// Una baldosa que se ensucia mientras se genera se vuelve a lanzar al
// terminar (su resultado viejo se tira): siempre gana la ultima version.

#include "CramionCore/navigation/Navigation.h"

#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/asset/Model.h>

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace cramion::navigation {

using core::Mat4;
using core::Vec3;
using core::Vec4;

namespace {

constexpr unsigned char kAreaGround = RC_WALKABLE_AREA;  // 63
constexpr unsigned char kAreaAvoid = 1;
constexpr unsigned short kFlagWalk = 1;
constexpr float kBig = 1.0e7f;
// Mallas grandes: se espera a que dejen de moverse (arrastrarlas con el gizmo
// no relanza sus baldosas en cada frame).
constexpr std::size_t kSettleTriangles = 20000;
constexpr float kSettleSeconds = 0.2f;
// Tiempo del hilo principal por frame para juntar geometria de baldosas.
constexpr double kGatherBudgetMs = 4.0;
// Por encima del suelo al dibujar la malla (como el DrawOffset de Unreal).
constexpr float kDrawOffset = 0.06f;

struct Aabb {
    Vec3 min{kBig, kBig, kBig};
    Vec3 max{-kBig, -kBig, -kBig};

    void add(const Vec3& p) {
        min = Vec3{std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = Vec3{std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    bool overlapsXz(const Aabb& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.z <= o.max.z && max.z >= o.min.z;
    }
    bool overlaps(const Aabb& o) const { return overlapsXz(o) && min.y <= o.max.y && max.y >= o.min.y; }
    bool contains(const Vec3& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
};

using TileKey = std::int64_t;
TileKey tileKey(int x, int z) {
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(z));
}
int tileX(TileKey key) { return static_cast<int>(key >> 32); }
int tileZ(TileKey key) { return static_cast<int>(static_cast<std::uint32_t>(key & 0xFFFFFFFF)); }

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

// FNV-1a sobre bytes: firmas de cambio.
struct Hasher {
    std::uint64_t value = 1469598103934665603ull;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value ^= p[i];
            value *= 1099511628211ull;
        }
    }
    template <typename T>
    void add(const T& v) {
        bytes(&v, sizeof(T));
    }
};

// Triangulos sueltos (3 vertices por triangulo) en el mundo.
struct TriangleSoup {
    std::vector<float> verts;  // x y z por vertice
    std::vector<int> tris;
    void addTriangle(const Vec3& a, const Vec3& b, const Vec3& c) {
        const int base = static_cast<int>(verts.size() / 3);
        for (const Vec3& p : {a, b, c}) {
            verts.push_back(p.x);
            verts.push_back(p.y);
            verts.push_back(p.z);
        }
        tris.push_back(base);
        tris.push_back(base + 1);
        tris.push_back(base + 2);
    }
    std::size_t triangleCount() const { return tris.size() / 3; }
};

// Fuente de geometria: una entidad con colision estatica.
struct Source {
    enum class Kind { Mesh, Terrain, Plane };
    Kind kind = Kind::Mesh;
    std::uint64_t signature = 0;
    Aabb bounds;
    // Mesh: triangulos en el mundo, repartidos por baldosa (con el borde).
    std::vector<float> verts;
    std::vector<int> tris;
    std::unordered_map<TileKey, std::vector<int>> buckets;
    float bucket_tile = 0.0f;
    float bucket_border = 0.0f;
    // Terrain.
    std::shared_ptr<const terrain::TerrainData> terrain;
    Mat4 world = Mat4::identity();
    float terrain_size = 0.0f;
    float terrain_height = 0.0f;
    // Plane: punto y normal en el mundo.
    Vec3 plane_point{};
    Vec3 plane_normal{0.0f, 1.0f, 0.0f};
    // Cambio pendiente (mallas grandes que se estan arrastrando).
    std::uint64_t pending_signature = 0;
    float pending_time = 0.0f;
    std::uint64_t seen_frame = 0;  // ultima pasada en la que seguia en la escena
};

struct Modifier {
    std::uint64_t signature = 0;
    Aabb bounds;
    std::vector<float> footprint;  // poligono convexo en XZ (x, 0, z)...
    NavArea area = NavArea::Block;
};

// --- Trabajo de una baldosa (se copia al hilo) --------------------------------

struct TileJob {
    int x = 0;
    int z = 0;
    std::uint64_t version = 0;
    NavigationSettings settings;
    float bmin[3]{};
    float bmax[3]{};
    TriangleSoup geometry;
    std::vector<Aabb> volumes;
    std::vector<Modifier> modifiers;
};

struct TileResult {
    int x = 0;
    int z = 0;
    std::uint64_t version = 0;
    unsigned char* data = nullptr;  // dtAlloc; nullptr = baldosa vacia
    int size = 0;
    float milliseconds = 0.0f;
};

int borderCells(const NavigationSettings& s) {
    return static_cast<int>(std::ceil(s.agent_radius / s.cell_size)) + 3;
}

// Convex hull 2D (cadena monotona) en XZ.
std::vector<std::pair<float, float>> convexHullXz(std::vector<std::pair<float, float>> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) return points;
    const auto cross = [](const std::pair<float, float>& o, const std::pair<float, float>& a,
                          const std::pair<float, float>& b) {
        return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
    };
    std::vector<std::pair<float, float>> hull(points.size() * 2);
    std::size_t k = 0;
    for (const auto& p : points) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], p) <= 0.0f) --k;
        hull[k++] = p;
    }
    for (std::size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        const auto& p = points[i - 1];
        while (k >= t && cross(hull[k - 2], hull[k - 1], p) <= 0.0f) --k;
        hull[k++] = p;
    }
    hull.resize(k - 1);
    return hull;
}

// La tuberia de Recast para una baldosa (como Sample_TileMesh::buildTileMesh).
TileResult buildTile(const TileJob& job) {
    const auto start = std::chrono::steady_clock::now();
    TileResult result;
    result.x = job.x;
    result.z = job.z;
    result.version = job.version;

    const NavigationSettings& s = job.settings;
    rcContext ctx(false);
    rcConfig cfg{};
    cfg.cs = s.cell_size;
    cfg.ch = s.cell_height;
    cfg.walkableSlopeAngle = std::clamp(s.max_slope, 0.0f, 89.0f);
    cfg.walkableHeight = std::max(3, static_cast<int>(std::ceil(s.agent_height / cfg.ch)));
    cfg.walkableClimb = std::max(0, static_cast<int>(std::floor(s.max_step_height / cfg.ch)));
    cfg.walkableRadius = std::max(0, static_cast<int>(std::ceil(s.agent_radius / cfg.cs)));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = static_cast<int>(std::max(0.0f, s.min_region_area) / (cfg.cs * cfg.cs));
    cfg.mergeRegionArea = static_cast<int>(rcSqr(20));
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.tileSize = s.tile_size;
    cfg.borderSize = borderCells(s);
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = s.detail_sample_distance < 0.9f ? 0.0f : cfg.cs * s.detail_sample_distance;
    cfg.detailSampleMaxError = cfg.ch * s.detail_max_error;
    rcVcopy(cfg.bmin, job.bmin);
    rcVcopy(cfg.bmax, job.bmax);
    cfg.bmin[0] -= static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmin[2] -= static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] += static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[2] += static_cast<float>(cfg.borderSize) * cfg.cs;

    const auto finish = [&]() {
        result.milliseconds =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        return result;
    };
    const int ntris = static_cast<int>(job.geometry.triangleCount());
    if (ntris == 0 || job.volumes.empty()) return finish();
    const int nverts = static_cast<int>(job.geometry.verts.size() / 3);

    rcHeightfield* solid = rcAllocHeightfield();
    if (solid == nullptr || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        rcFreeHeightField(solid);
        return finish();
    }
    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, job.geometry.verts.data(), nverts, job.geometry.tris.data(),
                            ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, job.geometry.verts.data(), nverts, job.geometry.tris.data(), areas.data(), ntris,
                              *solid, cfg.walkableClimb)) {
        rcFreeHeightField(solid);
        return finish();
    }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    const bool compact = chf != nullptr && rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf);
    rcFreeHeightField(solid);
    if (!compact) {
        rcFreeCompactHeightfield(chf);
        return finish();
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) {
        rcFreeCompactHeightfield(chf);
        return finish();
    }

    // Solo dentro de los volumenes (NavMeshBounds).
    for (int cz = 0; cz < chf->height; ++cz) {
        for (int cx = 0; cx < chf->width; ++cx) {
            const rcCompactCell& cell = chf->cells[cx + cz * chf->width];
            const float wx = chf->bmin[0] + (static_cast<float>(cx) + 0.5f) * chf->cs;
            const float wz = chf->bmin[2] + (static_cast<float>(cz) + 0.5f) * chf->cs;
            for (unsigned int i = cell.index, n = cell.index + cell.count; i < n; ++i) {
                if (chf->areas[i] == RC_NULL_AREA) continue;
                const Vec3 p{wx, chf->bmin[1] + static_cast<float>(chf->spans[i].y) * chf->ch, wz};
                bool inside = false;
                for (const Aabb& volume : job.volumes) {
                    if (volume.contains(p)) {
                        inside = true;
                        break;
                    }
                }
                if (!inside) chf->areas[i] = RC_NULL_AREA;
            }
        }
    }
    // Modificadores: primero los de evitar, despues los que bloquean (ganan).
    for (int pass = 0; pass < 2; ++pass) {
        for (const Modifier& m : job.modifiers) {
            if ((pass == 0) != (m.area == NavArea::Avoid)) continue;
            const int n = static_cast<int>(m.footprint.size() / 3);
            if (n < 3) continue;
            rcMarkConvexPolyArea(&ctx, m.footprint.data(), n, m.bounds.min.y, m.bounds.max.y,
                                 m.area == NavArea::Avoid ? kAreaAvoid : RC_NULL_AREA, *chf);
        }
    }

    rcContourSet* cset = rcAllocContourSet();
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    const auto cleanup = [&]() {
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    };
    bool ok = cset != nullptr && pmesh != nullptr && dmesh != nullptr && rcBuildDistanceField(&ctx, *chf) &&
              rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea) &&
              rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
    if (!ok || cset->nconts == 0) {
        cleanup();
        return finish();
    }
    ok = rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh) &&
         rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);
    if (!ok || pmesh->npolys == 0 || pmesh->nverts >= 0xffff) {
        cleanup();
        return finish();
    }
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == kAreaGround || pmesh->areas[i] == kAreaAvoid) pmesh->flags[i] = kFlagWalk;
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = s.agent_height;
    params.walkableRadius = s.agent_radius;
    params.walkableClimb = s.max_step_height;
    params.tileX = job.x;
    params.tileY = job.z;
    params.tileLayer = 0;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (dtCreateNavMeshData(&params, &data, &size)) {
        result.data = data;
        result.size = size;
    }
    cleanup();
    return finish();
}

// Primitivas convexas: triangulos orientados hacia fuera (Recast decide lo
// transitable por la normal).
void addConvex(TriangleSoup& soup, const std::vector<Vec3>& points, const std::vector<int>& indices,
               const Vec3& center) {
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vec3& a = points[static_cast<std::size_t>(indices[i])];
        const Vec3& b = points[static_cast<std::size_t>(indices[i + 1])];
        const Vec3& c = points[static_cast<std::size_t>(indices[i + 2])];
        const Vec3 n = core::cross(b - a, c - a);
        const Vec3 centroid = (a + b + c) * (1.0f / 3.0f);
        if (core::dot(n, centroid - center) < 0.0f) {
            soup.addTriangle(a, c, b);
        } else {
            soup.addTriangle(a, b, c);
        }
    }
}

void boxTriangles(TriangleSoup& soup, const Mat4& world, const Vec3& center, const Vec3& half) {
    std::vector<Vec3> p;
    for (int i = 0; i < 8; ++i) {
        const Vec3 local{center.x + ((i & 1) ? half.x : -half.x), center.y + ((i & 2) ? half.y : -half.y),
                         center.z + ((i & 4) ? half.z : -half.z)};
        p.push_back(transformPoint(world, local));
    }
    static const std::vector<int> kFaces = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                            2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    addConvex(soup, p, kFaces, transformPoint(world, center));
}

// Capsula (o esfera con half_height = 0) a lo largo de Y y girada a `axis`.
void capsuleTriangles(TriangleSoup& soup, const Mat4& world, const Vec3& center, float radius, float half_height,
                      int axis) {
    constexpr int kRings = 6;     // por semiesfera
    constexpr int kSegments = 12;
    const auto orient = [&](Vec3 p) {
        if (axis == 0) p = Vec3{p.y, -p.x, p.z};
        if (axis == 2) p = Vec3{p.x, -p.z, p.y};
        return transformPoint(world, center + p);
    };
    std::vector<Vec3> points;
    std::vector<int> rows;  // inicio de cada anillo
    // Polo inferior, anillos, polo superior.
    points.push_back(orient(Vec3{0.0f, -half_height - radius, 0.0f}));
    for (int ring = 1; ring < kRings * 2; ++ring) {
        const float t = static_cast<float>(ring) / static_cast<float>(kRings * 2) * core::kPi;  // 0..pi
        const float y = -std::cos(t) * radius + (t < core::kPi * 0.5f ? -half_height : half_height);
        const float r = std::sin(t) * radius;
        rows.push_back(static_cast<int>(points.size()));
        for (int s = 0; s < kSegments; ++s) {
            const float a = static_cast<float>(s) / kSegments * 2.0f * core::kPi;
            points.push_back(orient(Vec3{std::cos(a) * r, y, std::sin(a) * r}));
        }
        // Ecuador: el tramo recto de la capsula (otro anillo a la misma r).
        if (ring == kRings && half_height > 0.0f) {
            rows.push_back(static_cast<int>(points.size()));
            for (int s = 0; s < kSegments; ++s) {
                const float a = static_cast<float>(s) / kSegments * 2.0f * core::kPi;
                points.push_back(orient(Vec3{std::cos(a) * radius, half_height, std::sin(a) * radius}));
            }
        }
    }
    const int top = static_cast<int>(points.size());
    points.push_back(orient(Vec3{0.0f, half_height + radius, 0.0f}));
    std::vector<int> idx;
    for (int s = 0; s < kSegments; ++s) {
        const int n = (s + 1) % kSegments;
        idx.insert(idx.end(), {0, rows.front() + s, rows.front() + n});
        idx.insert(idx.end(), {top, rows.back() + n, rows.back() + s});
    }
    for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
        for (int s = 0; s < kSegments; ++s) {
            const int n = (s + 1) % kSegments;
            const int a = rows[r] + s, b = rows[r] + n, c = rows[r + 1] + s, d = rows[r + 1] + n;
            idx.insert(idx.end(), {a, c, d, a, d, b});
        }
    }
    addConvex(soup, points, idx, transformPoint(world, center));
}

float frand() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

}  // namespace

// ================================================================================

struct NavigationSystem::Impl {
    NavigationSettings settings;

    // Origen flotante: la malla vive en su propio espacio ("nav") y no se
    // rehace cuando el mundo se desplaza. nav = local + nav_offset (la suma
    // de los desplazamientos). Se convierte al entrar y al salir: posiciones
    // de las entidades, destinos y resultados de las consultas.
    Vec3 nav_offset{};
    Vec3 toNav(const Vec3& p) const { return p + nav_offset; }
    Vec3 fromNav(const Vec3& p) const { return p - nav_offset; }
    Mat4 navWorld(ecs::Entity e) const {
        Mat4 m = e.worldMatrix();
        m.m[3][0] += nav_offset.x;
        m.m[3][1] += nav_offset.y;
        m.m[3][2] += nav_offset.z;
        return m;
    }
    MeshProvider mesh_provider;
    TerrainProvider terrain_provider;
    physics::PhysicsSystem* physics = nullptr;
    float time = 0.0f;

    // --- Malla ---
    dtNavMesh* navmesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtQueryFilter filter;
    int max_tiles = 0;

    // --- Escena observada (por UUID: al volver de Play el mundo se
    // reconstruye con otros handles y la malla no tiene por que rehacerse) ---
    std::unordered_map<Uuid, Source> sources;
    std::unordered_map<Uuid, Modifier> modifiers;
    std::vector<Aabb> volumes;
    std::uint64_t volumes_signature = 0;
    std::unordered_set<TileKey> active_tiles;  // dentro de algun volumen
    std::vector<entt::entity> candidates;      // reutilizado cada frame
    std::uint64_t scan_frame = 0;
    bool dirty_all = false;

    // --- Baldosas ---
    struct TileState {
        std::uint64_t version = 0;  // sube al ensuciarse
        bool in_flight = false;
        bool has_data = false;
        int polygons = 0;
        std::size_t bytes = 0;
        // Para dibujar (se rehace al meter la baldosa).
        std::vector<Vec3> triangles;
        std::vector<std::uint8_t> areas;
        std::vector<Vec3> edges;
    };
    std::unordered_map<TileKey, TileState> tiles;
    std::deque<TileKey> dirty_queue;
    std::unordered_set<TileKey> dirty_set;
    float last_tile_ms = 0.0f;
    std::uint64_t built_once = 0;  // baldosas terminadas desde el ultimo clear

    // --- Hilos ---
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::unique_ptr<TileJob>> jobs;
    std::vector<TileResult> results;
    bool quit = false;
    int in_flight = 0;

    // --- Dibujo ---
    NavDebugMesh debug;
    bool debug_dirty = true;

    // --- Agentes ---
    dtCrowd* crowd = nullptr;
    struct Agent {
        int index = -1;
        bool has_target = false;
        Vec3 target{};
        bool target_sent = false;
        Vec3 written{};  // posicion que se escribio (para ver si la movieron desde fuera)
        bool written_valid = false;
        NavAgent params{};
    };
    std::unordered_map<entt::entity, Agent> agents;
    ecs::World* world = nullptr;

    Impl() { startWorkers(); }
    ~Impl() {
        stopWorkers();
        freeMesh();
    }

    // ---------------------------------------------------------------- hilos
    void startWorkers() {
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        const int count = settings.build_threads > 0 ? settings.build_threads
                                                     : static_cast<int>(std::clamp(hw / 2u, 1u, 6u));
        quit = false;
        for (int i = 0; i < count; ++i) {
            workers.emplace_back([this]() {
                for (;;) {
                    std::unique_ptr<TileJob> job;
                    {
                        std::unique_lock lock(mutex);
                        wake.wait(lock, [this]() { return quit || !jobs.empty(); });
                        if (quit) return;
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    TileResult r = buildTile(*job);
                    std::lock_guard lock(mutex);
                    results.push_back(r);
                }
            });
        }
    }

    void stopWorkers() {
        {
            std::lock_guard lock(mutex);
            quit = true;
            jobs.clear();
        }
        wake.notify_all();
        for (std::thread& t : workers) t.join();
        workers.clear();
        for (TileResult& r : results) dtFree(r.data);
        results.clear();
        in_flight = 0;
    }

    // ---------------------------------------------------------------- malla
    void freeMesh() {
        if (crowd != nullptr) dtFreeCrowd(crowd);
        crowd = nullptr;
        for (auto& [handle, agent] : agents) {
            agent.index = -1;
            agent.target_sent = false;
        }
        if (query != nullptr) dtFreeNavMeshQuery(query);
        query = nullptr;
        if (navmesh != nullptr) dtFreeNavMesh(navmesh);
        navmesh = nullptr;
        max_tiles = 0;
    }

    // dtNavMesh con sitio para `needed` baldosas (y la consulta y la multitud).
    void createMesh(int needed) {
        freeMesh();
        int capacity = 1024;
        while (capacity < needed * 2) capacity *= 2;
        navmesh = dtAllocNavMesh();
        dtNavMeshParams params{};
        params.orig[0] = params.orig[1] = params.orig[2] = 0.0f;
        params.tileWidth = settings.tileWorldSize();
        params.tileHeight = settings.tileWorldSize();
        params.maxTiles = capacity;
        params.maxPolys = 1 << 16;
        if (dtStatusFailed(navmesh->init(&params))) {
            std::cerr << "[Navegacion] No se pudo crear la malla\n";
            dtFreeNavMesh(navmesh);
            navmesh = nullptr;
            return;
        }
        max_tiles = capacity;
        query = dtAllocNavMeshQuery();
        query->init(navmesh, 4096);
        crowd = dtAllocCrowd();
        crowd->init(std::max(1, settings.max_agents), 4.0f, navmesh);
        applyCosts();
        for (auto& [key, tile] : tiles) {
            tile.has_data = false;
        }
    }

    void applyCosts() {
        filter.setIncludeFlags(kFlagWalk);
        filter.setAreaCost(kAreaGround, 1.0f);
        filter.setAreaCost(kAreaAvoid, std::max(1.0f, settings.avoid_cost));
        if (crowd != nullptr) {
            dtQueryFilter* f = crowd->getEditableFilter(0);
            f->setIncludeFlags(kFlagWalk);
            f->setAreaCost(kAreaGround, 1.0f);
            f->setAreaCost(kAreaAvoid, std::max(1.0f, settings.avoid_cost));
        }
    }

    // Todo desde cero (cambio de ajustes, rebuildAll).
    void resetTiles() {
        {
            std::lock_guard lock(mutex);
            jobs.clear();
            for (TileResult& r : results) dtFree(r.data);
            results.clear();
        }
        // Las que estan en los hilos volveran con una version vieja.
        for (auto& [key, tile] : tiles) {
            ++tile.version;
            tile.has_data = false;
            tile.triangles.clear();
            tile.areas.clear();
            tile.edges.clear();
        }
        if (navmesh != nullptr) {
            for (int i = 0; i < navmesh->getMaxTiles(); ++i) {
                const dtMeshTile* t = static_cast<const dtNavMesh*>(navmesh)->getTile(i);
                if (t != nullptr && t->header != nullptr) navmesh->removeTile(navmesh->getTileRef(t), nullptr, nullptr);
            }
        }
        debug_dirty = true;
        dirty_all = true;
    }

    // ---------------------------------------------------------------- fuentes
    static bool excluded(ecs::Entity e) {
        if (!e.activeInHierarchy()) return true;
        if (const physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>(); rb != nullptr && rb->type == physics::BodyType::Dynamic) {
            return true;
        }
        for (ecs::Entity p = e; p.valid(); p = p.parent()) {
            if (p.has<NavAgent>()) return true;  // los agentes no se tallan a si mismos
        }
        return false;
    }

    // Firma de la geometria de colision de la entidad (0 = no aporta nada).
    std::uint64_t signatureOf(ecs::Entity e, const asset::ModelData*& mesh,
                              std::shared_ptr<const terrain::TerrainData>& terrain_data) {
        Hasher h;
        bool any = false;
        const Mat4 world = navWorld(e);
        h.add(world);
        if (const auto* c = e.tryGet<physics::BoxCollider>(); c && !c->material.is_trigger) {
            h.add(1);
            h.add(c->size);
            h.add(c->center);
            any = true;
        }
        if (const auto* c = e.tryGet<physics::SphereCollider>(); c && !c->material.is_trigger) {
            h.add(2);
            h.add(c->radius);
            h.add(c->center);
            any = true;
        }
        if (const auto* c = e.tryGet<physics::CapsuleCollider>(); c && !c->material.is_trigger) {
            h.add(3);
            h.add(c->radius);
            h.add(c->height);
            h.add(c->axis);
            h.add(c->center);
            any = true;
        }
        if (const auto* c = e.tryGet<physics::MeshCollider>(); c && !c->material.is_trigger && mesh_provider) {
            mesh = mesh_provider(e);
            if (mesh != nullptr) {
                h.add(4);
                h.add(mesh);
                h.add(mesh->vertices.size());
                h.add(mesh->indices.size());
                any = true;
            }
        }
        if (const auto* c = e.tryGet<physics::PlaneCollider>(); c && !c->material.is_trigger) {
            h.add(5);
            any = true;
        }
        if (const auto* t = e.tryGet<terrain::Terrain>(); t && t->collision && terrain_provider) {
            terrain_data = terrain_provider(e);
            if (terrain_data && terrain_data->resolution() >= 3) {
                h.add(6);
                h.add(terrain_data.get());
                h.add(terrain_data->collisionVersion());
                h.add(t->size);
                h.add(t->height);
                any = true;
            }
        }
        return any ? (h.value | 1u) : 0;
    }

    void buildSource(ecs::Entity e, Source& src, const asset::ModelData* mesh,
                     const std::shared_ptr<const terrain::TerrainData>& terrain_data) {
        const Mat4 world = navWorld(e);
        src.verts.clear();
        src.tris.clear();
        src.buckets.clear();
        src.bucket_tile = 0.0f;
        src.bounds = Aabb{};
        src.terrain.reset();
        src.world = world;

        if (terrain_data) {
            const terrain::Terrain& t = e.get<terrain::Terrain>();
            src.kind = Source::Kind::Terrain;
            src.terrain = terrain_data;
            src.terrain_size = t.size;
            src.terrain_height = t.height;
            for (int i = 0; i < 8; ++i) {
                src.bounds.add(transformPoint(world, Vec3{(i & 1) ? t.size : 0.0f, (i & 2) ? t.height : 0.0f,
                                                          (i & 4) ? t.size : 0.0f}));
            }
            // Un terreno con otros colliders en la misma entidad: se suman como malla.
        }
        if (e.has<physics::PlaneCollider>() && !e.get<physics::PlaneCollider>().material.is_trigger) {
            src.kind = Source::Kind::Plane;
            src.plane_point = transformPoint(world, Vec3{});
            const Vec3 n = transformPoint(world, Vec3{0.0f, 1.0f, 0.0f}) - src.plane_point;
            src.plane_normal = core::length(n) > 1e-6f ? core::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
            src.bounds.add(Vec3{-kBig, -kBig, -kBig});
            src.bounds.add(Vec3{kBig, kBig, kBig});
        }

        TriangleSoup soup;
        if (const auto* c = e.tryGet<physics::BoxCollider>(); c && !c->material.is_trigger) {
            boxTriangles(soup, world, c->center,
                         Vec3{std::abs(c->size.x) * 0.5f, std::abs(c->size.y) * 0.5f, std::abs(c->size.z) * 0.5f});
        }
        if (const auto* c = e.tryGet<physics::SphereCollider>(); c && !c->material.is_trigger) {
            capsuleTriangles(soup, world, c->center, std::max(c->radius, 1e-3f), 0.0f, 1);
        }
        if (const auto* c = e.tryGet<physics::CapsuleCollider>(); c && !c->material.is_trigger) {
            const float radius = std::max(c->radius, 1e-3f);
            capsuleTriangles(soup, world, c->center, radius, std::max(c->height * 0.5f - radius, 0.0f),
                             static_cast<int>(c->axis));
        }
        if (mesh != nullptr) {
            const auto& v = mesh->vertices;
            const auto& idx = mesh->indices;
            for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                if (idx[i] >= v.size() || idx[i + 1] >= v.size() || idx[i + 2] >= v.size()) continue;
                const Vec3 a = transformPoint(world, v[idx[i]].position);
                const Vec3 b = transformPoint(world, v[idx[i + 1]].position);
                const Vec3 c = transformPoint(world, v[idx[i + 2]].position);
                // Orientacion por las normales de la malla (el orden de los
                // vertices cambia entre formatos y con escalas negativas).
                const Vec3 ln = v[idx[i]].normal + v[idx[i + 1]].normal + v[idx[i + 2]].normal;
                const Vec3 wn = transformPoint(world, ln) - transformPoint(world, Vec3{});
                if (core::dot(core::cross(b - a, c - a), wn) < 0.0f) {
                    soup.addTriangle(a, c, b);
                } else {
                    soup.addTriangle(a, b, c);
                }
            }
        }
        if (!soup.tris.empty()) {
            if (src.kind != Source::Kind::Terrain && src.kind != Source::Kind::Plane) src.kind = Source::Kind::Mesh;
            src.verts = std::move(soup.verts);
            src.tris = std::move(soup.tris);
            for (std::size_t i = 0; i < src.verts.size(); i += 3) {
                src.bounds.add(Vec3{src.verts[i], src.verts[i + 1], src.verts[i + 2]});
            }
        } else if (!terrain_data && src.kind != Source::Kind::Plane) {
            src.kind = Source::Kind::Mesh;
        }
    }

    // Triangulos de la fuente por baldosa (con el borde de la baldosa).
    void ensureBuckets(Source& src) {
        const float tile = settings.tileWorldSize();
        const float border = static_cast<float>(borderCells(settings)) * settings.cell_size;
        if (src.bucket_tile == tile && src.bucket_border == border) return;
        src.buckets.clear();
        src.bucket_tile = tile;
        src.bucket_border = border;
        for (std::size_t t = 0; t + 2 < src.tris.size(); t += 3) {
            float lo_x = kBig, lo_z = kBig, hi_x = -kBig, hi_z = -kBig;
            for (int k = 0; k < 3; ++k) {
                const std::size_t v = static_cast<std::size_t>(src.tris[t + static_cast<std::size_t>(k)]) * 3;
                lo_x = std::min(lo_x, src.verts[v]);
                hi_x = std::max(hi_x, src.verts[v]);
                lo_z = std::min(lo_z, src.verts[v + 2]);
                hi_z = std::max(hi_z, src.verts[v + 2]);
            }
            const int x0 = static_cast<int>(std::floor((lo_x - border) / tile));
            const int x1 = static_cast<int>(std::floor((hi_x + border) / tile));
            const int z0 = static_cast<int>(std::floor((lo_z - border) / tile));
            const int z1 = static_cast<int>(std::floor((hi_z + border) / tile));
            for (int z = z0; z <= z1; ++z) {
                for (int x = x0; x <= x1; ++x) {
                    if (!active_tiles.contains(tileKey(x, z))) continue;
                    src.buckets[tileKey(x, z)].push_back(static_cast<int>(t / 3));
                }
            }
        }
    }

    // Geometria de una baldosa (area con borde en [lo, hi] del mundo).
    void gather(TileKey key, const Aabb& area, TriangleSoup& out) {
        for (auto& [handle, src] : sources) {
            if (!src.bounds.overlapsXz(area)) continue;
            // Triangulos (primitivas y mallas).
            if (!src.tris.empty()) {
                ensureBuckets(src);
                const auto it = src.buckets.find(key);
                if (it != src.buckets.end()) {
                    for (const int t : it->second) {
                        const std::size_t base = static_cast<std::size_t>(t) * 3;
                        Vec3 p[3];
                        for (int k = 0; k < 3; ++k) {
                            const std::size_t v = static_cast<std::size_t>(src.tris[base + static_cast<std::size_t>(k)]) * 3;
                            p[k] = Vec3{src.verts[v], src.verts[v + 1], src.verts[v + 2]};
                        }
                        out.addTriangle(p[0], p[1], p[2]);
                    }
                }
            }
            if (src.kind == Source::Kind::Terrain && src.terrain) {
                gatherTerrain(src, area, out);
            } else if (src.kind == Source::Kind::Plane) {
                const Vec3& n = src.plane_normal;
                if (std::abs(n.y) < 0.2f) continue;  // plano casi vertical: nada transitable
                const auto y = [&](float x, float z) {
                    return src.plane_point.y - (n.x * (x - src.plane_point.x) + n.z * (z - src.plane_point.z)) / n.y;
                };
                const Vec3 c00{area.min.x, y(area.min.x, area.min.z), area.min.z};
                const Vec3 c01{area.min.x, y(area.min.x, area.max.z), area.max.z};
                const Vec3 c10{area.max.x, y(area.max.x, area.min.z), area.min.z};
                const Vec3 c11{area.max.x, y(area.max.x, area.max.z), area.max.z};
                if (n.y > 0.0f) {
                    out.addTriangle(c00, c01, c10);
                    out.addTriangle(c10, c01, c11);
                } else {
                    out.addTriangle(c00, c10, c01);
                    out.addTriangle(c10, c11, c01);
                }
            }
        }
    }

    void gatherTerrain(const Source& src, const Aabb& area, TriangleSoup& out) {
        const terrain::TerrainData& data = *src.terrain;
        const int res = static_cast<int>(data.resolution());
        if (res < 3 || src.terrain_size <= 0.0f) return;
        const float cell = src.terrain_size / static_cast<float>(res - 1);
        // Muestras mas finas que la celda de la navegacion no aportan nada.
        const int stride = std::max(1, static_cast<int>(std::floor(settings.cell_size / cell)));
        // Zona en coordenadas locales del terreno.
        const Mat4 inverse = core::inverse(src.world);
        Aabb local;
        for (int i = 0; i < 4; ++i) {
            local.add(transformPoint(inverse, Vec3{(i & 1) ? area.max.x : area.min.x, 0.0f, (i & 2) ? area.max.z : area.min.z}));
        }
        const int x0 = std::clamp(static_cast<int>(std::floor(local.min.x / cell)) - stride, 0, res - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(local.max.x / cell)) + stride, 0, res - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor(local.min.z / cell)) - stride, 0, res - 1);
        const int z1 = std::clamp(static_cast<int>(std::ceil(local.max.z / cell)) + stride, 0, res - 1);
        const float inv = 1.0f / static_cast<float>(res - 1);
        const auto point = [&](int i, int j) {
            i = std::min(i, res - 1);
            j = std::min(j, res - 1);
            const float h = data.sample(static_cast<float>(i) * inv, static_cast<float>(j) * inv) * src.terrain_height;
            return transformPoint(src.world, Vec3{static_cast<float>(i) * cell, h, static_cast<float>(j) * cell});
        };
        for (int j = z0; j < z1; j += stride) {
            for (int i = x0; i < x1; i += stride) {
                const Vec3 p00 = point(i, j);
                const Vec3 p01 = point(i, j + stride);
                const Vec3 p10 = point(i + stride, j);
                const Vec3 p11 = point(i + stride, j + stride);
                out.addTriangle(p00, p01, p10);
                out.addTriangle(p10, p01, p11);
            }
        }
    }

    // ---------------------------------------------------------------- cambios
    void markArea(const Aabb& box) {
        if (!box.valid()) return;
        const float tile = settings.tileWorldSize();
        const float border = static_cast<float>(borderCells(settings)) * settings.cell_size;
        // Planos infinitos: todas las baldosas activas.
        if (box.max.x - box.min.x > kBig || box.max.z - box.min.z > kBig) {
            for (const TileKey key : active_tiles) markTile(key);
            return;
        }
        const int x0 = static_cast<int>(std::floor((box.min.x - border) / tile));
        const int x1 = static_cast<int>(std::floor((box.max.x + border) / tile));
        const int z0 = static_cast<int>(std::floor((box.min.z - border) / tile));
        const int z1 = static_cast<int>(std::floor((box.max.z + border) / tile));
        if (static_cast<std::int64_t>(x1 - x0 + 1) * (z1 - z0 + 1) > static_cast<std::int64_t>(active_tiles.size())) {
            for (const TileKey key : active_tiles) {
                const int x = tileX(key), z = tileZ(key);
                if (x >= x0 && x <= x1 && z >= z0 && z <= z1) markTile(key);
            }
            return;
        }
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                if (active_tiles.contains(tileKey(x, z))) markTile(tileKey(x, z));
            }
        }
    }

    void markTile(TileKey key) {
        TileState& tile = tiles[key];
        ++tile.version;
        if (dirty_set.insert(key).second) dirty_queue.push_back(key);
    }

    // Volumenes y baldosas activas.
    void scanVolumes(ecs::World& world) {
        std::vector<Aabb> now;
        Hasher h;
        for (const entt::entity handle : world.registry().view<NavMeshBounds>()) {
            const ecs::Entity e = world.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const NavMeshBounds& b = e.get<NavMeshBounds>();
            Aabb box;
            const Mat4 m = navWorld(e);
            for (int i = 0; i < 8; ++i) {
                box.add(transformPoint(m, Vec3{((i & 1) ? 0.5f : -0.5f) * std::abs(b.size.x),
                                               ((i & 2) ? 0.5f : -0.5f) * std::abs(b.size.y),
                                               ((i & 4) ? 0.5f : -0.5f) * std::abs(b.size.z)}));
            }
            now.push_back(box);
            h.add(box);
        }
        if (h.value == volumes_signature && now.size() == volumes.size()) return;
        volumes_signature = h.value;

        // Baldosas que tocan algun volumen.
        const float tile = settings.tileWorldSize();
        std::unordered_set<TileKey> active;
        for (const Aabb& box : now) {
            const int x0 = static_cast<int>(std::floor(box.min.x / tile));
            const int x1 = static_cast<int>(std::floor(box.max.x / tile));
            const int z0 = static_cast<int>(std::floor(box.min.z / tile));
            const int z1 = static_cast<int>(std::floor(box.max.z / tile));
            const std::int64_t count = static_cast<std::int64_t>(x1 - x0 + 1) * (z1 - z0 + 1);
            if (count > 1'000'000) {
                std::cerr << "[Navegacion] Volumen demasiado grande para cell_size/tile_size (" << count
                          << " baldosas): se ignora\n";
                continue;
            }
            for (int z = z0; z <= z1; ++z) {
                for (int x = x0; x <= x1; ++x) active.insert(tileKey(x, z));
            }
        }
        // Las que dejan de estar: fuera de la malla.
        for (auto it = tiles.begin(); it != tiles.end();) {
            if (!active.contains(it->first)) {
                removeTileData(it->first);
                it = tiles.erase(it);
            } else {
                ++it;
            }
        }
        const bool grew = static_cast<int>(active.size()) > max_tiles;
        active_tiles = std::move(active);
        // Los cubos de las fuentes dependen de las baldosas activas.
        for (auto& [handle, src] : sources) src.bucket_tile = 0.0f;
        if (navmesh == nullptr || grew) {
            createMesh(static_cast<int>(active_tiles.size()));
            for (auto& [key, t] : tiles) t.has_data = false;
            for (const TileKey key : active_tiles) markTile(key);
        } else {
            // Todas las baldosas de los volumenes viejos y nuevos (la altura
            // del volumen cambia la malla de toda su columna).
            for (const Aabb& box : volumes) markArea(box);
            for (const Aabb& box : now) markArea(box);
        }
        volumes = std::move(now);
        debug_dirty = true;
    }

    void scanModifiers(ecs::World& world) {
        std::unordered_set<Uuid> seen;
        for (const entt::entity handle : world.registry().view<NavModifier>()) {
            const ecs::Entity e = world.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const NavModifier& m = e.get<NavModifier>();
            Hasher h;
            h.add(navWorld(e));
            h.add(m.size);
            h.add(m.area);
            seen.insert(e.uuid());
            Modifier& mod = modifiers[e.uuid()];
            if (mod.signature == h.value) continue;
            if (mod.signature != 0) markArea(mod.bounds);
            mod.signature = h.value;
            mod.area = m.area;
            mod.bounds = Aabb{};
            std::vector<std::pair<float, float>> xz;
            const Mat4 w = navWorld(e);
            for (int i = 0; i < 8; ++i) {
                const Vec3 p = transformPoint(w, Vec3{((i & 1) ? 0.5f : -0.5f) * std::abs(m.size.x),
                                                      ((i & 2) ? 0.5f : -0.5f) * std::abs(m.size.y),
                                                      ((i & 4) ? 0.5f : -0.5f) * std::abs(m.size.z)});
                mod.bounds.add(p);
                xz.emplace_back(p.x, p.z);
            }
            mod.footprint.clear();
            for (const auto& [x, z] : convexHullXz(std::move(xz))) {
                mod.footprint.insert(mod.footprint.end(), {x, 0.0f, z});
            }
            markArea(mod.bounds);
        }
        for (auto it = modifiers.begin(); it != modifiers.end();) {
            if (!seen.contains(it->first)) {
                markArea(it->second.bounds);
                it = modifiers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void scanSources(ecs::World& world, float delta_seconds) {
        // Sin conjuntos hash por frame (con miles de colliders se notan).
        candidates.clear();
        auto& reg = world.registry();
        const auto gather = [&](auto view) { candidates.insert(candidates.end(), view.begin(), view.end()); };
        gather(reg.view<physics::BoxCollider>());
        gather(reg.view<physics::SphereCollider>());
        gather(reg.view<physics::CapsuleCollider>());
        gather(reg.view<physics::MeshCollider>());
        gather(reg.view<physics::PlaneCollider>());
        gather(reg.view<terrain::Terrain>());
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        const std::uint64_t frame = ++scan_frame;
        for (const entt::entity handle : candidates) {
            const ecs::Entity e = world.wrap(handle);
            if (excluded(e)) continue;
            const asset::ModelData* mesh = nullptr;
            std::shared_ptr<const terrain::TerrainData> terrain_data;
            const std::uint64_t signature = signatureOf(e, mesh, terrain_data);
            if (signature == 0) continue;
            auto [it, inserted] = sources.try_emplace(e.uuid());
            Source& src = it->second;
            src.seen_frame = frame;
            if (!inserted && src.signature == signature) {
                src.pending_signature = 0;
                continue;
            }
            // Malla grande en movimiento: se espera a que se quede quieta.
            if (!inserted && src.tris.size() / 3 > kSettleTriangles) {
                if (src.pending_signature != signature) {
                    src.pending_signature = signature;
                    src.pending_time = 0.0f;
                    continue;
                }
                src.pending_time += delta_seconds;
                if (src.pending_time < kSettleSeconds) continue;
            }
            if (!inserted) markArea(src.bounds);
            src.signature = signature;
            src.pending_signature = 0;
            buildSource(e, src, mesh, terrain_data);
            markArea(src.bounds);
        }
        for (auto it = sources.begin(); it != sources.end();) {
            if (it->second.seen_frame != frame) {
                markArea(it->second.bounds);
                it = sources.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---------------------------------------------------------------- baldosas
    void removeTileData(TileKey key) {
        if (navmesh != nullptr) {
            const dtTileRef ref = navmesh->getTileRefAt(tileX(key), tileZ(key), 0);
            if (ref != 0) navmesh->removeTile(ref, nullptr, nullptr);
        }
        if (auto it = tiles.find(key); it != tiles.end()) {
            it->second.has_data = false;
            it->second.polygons = 0;
            it->second.bytes = 0;
            it->second.triangles.clear();
            it->second.areas.clear();
            it->second.edges.clear();
        }
        debug_dirty = true;
    }

    void dispatch() {
        if (navmesh == nullptr || volumes.empty()) {
            dirty_queue.clear();
            dirty_set.clear();
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const int limit = static_cast<int>(workers.size()) * 3;
        std::size_t scanned = dirty_queue.size();
        while (!dirty_queue.empty() && scanned-- > 0) {
            if (in_flight >= limit) break;
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > kGatherBudgetMs) {
                break;
            }
            const TileKey key = dirty_queue.front();
            dirty_queue.pop_front();
            if (!active_tiles.contains(key)) {
                dirty_set.erase(key);
                continue;
            }
            TileState& tile = tiles[key];
            if (tile.in_flight) {
                dirty_queue.push_back(key);  // se relanza cuando vuelva
                continue;
            }
            dirty_set.erase(key);

            auto job = std::make_unique<TileJob>();
            job->x = tileX(key);
            job->z = tileZ(key);
            job->version = tile.version;
            job->settings = settings;
            const float size = settings.tileWorldSize();
            float ymin = kBig, ymax = -kBig;
            Aabb column;
            column.add(Vec3{static_cast<float>(job->x) * size, -kBig, static_cast<float>(job->z) * size});
            column.add(Vec3{static_cast<float>(job->x + 1) * size, kBig, static_cast<float>(job->z + 1) * size});
            for (const Aabb& v : volumes) {
                if (!v.overlapsXz(column)) continue;
                job->volumes.push_back(v);
                ymin = std::min(ymin, v.min.y);
                ymax = std::max(ymax, v.max.y);
            }
            if (job->volumes.empty()) continue;
            job->bmin[0] = static_cast<float>(job->x) * size;
            job->bmin[1] = ymin;
            job->bmin[2] = static_cast<float>(job->z) * size;
            job->bmax[0] = static_cast<float>(job->x + 1) * size;
            job->bmax[1] = ymax;
            job->bmax[2] = static_cast<float>(job->z + 1) * size;
            const float border = static_cast<float>(borderCells(settings)) * settings.cell_size;
            Aabb area;
            area.add(Vec3{job->bmin[0] - border, ymin, job->bmin[2] - border});
            area.add(Vec3{job->bmax[0] + border, ymax, job->bmax[2] + border});
            gather(key, area, job->geometry);
            for (const auto& [handle, m] : modifiers) {
                if (m.bounds.overlapsXz(area)) job->modifiers.push_back(m);
            }
            tile.in_flight = true;
            ++in_flight;
            {
                std::lock_guard lock(mutex);
                jobs.push_back(std::move(job));
            }
            wake.notify_one();
        }
    }

    void collect() {
        std::vector<TileResult> done;
        {
            std::lock_guard lock(mutex);
            done.swap(results);
        }
        for (TileResult& r : done) {
            const TileKey key = tileKey(r.x, r.z);
            --in_flight;
            auto it = tiles.find(key);
            if (it == tiles.end()) {
                dtFree(r.data);
                continue;
            }
            TileState& tile = it->second;
            tile.in_flight = false;
            last_tile_ms = r.milliseconds;
            if (r.version != tile.version || navmesh == nullptr) {
                dtFree(r.data);  // vieja: ya hay otra en la cola
                continue;
            }
            removeTileData(key);
            ++built_once;
            if (r.data == nullptr) continue;
            dtTileRef ref = 0;
            if (dtStatusFailed(navmesh->addTile(r.data, r.size, DT_TILE_FREE_DATA, 0, &ref))) {
                dtFree(r.data);
                continue;
            }
            tile.has_data = true;
            tile.bytes = static_cast<std::size_t>(r.size);
            buildTileDebug(ref, tile);
        }
    }

    // Triangulos de detalle y bordes de una baldosa ya metida.
    void buildTileDebug(dtTileRef ref, TileState& state) {
        const dtMeshTile* tile = navmesh->getTileByRef(ref);
        state.triangles.clear();
        state.areas.clear();
        state.edges.clear();
        state.polygons = 0;
        if (tile == nullptr || tile->header == nullptr) return;
        state.polygons = tile->header->polyCount;
        const Vec3 up{0.0f, kDrawOffset, 0.0f};
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            const dtPolyDetail& pd = tile->detailMeshes[i];
            const std::uint8_t area = poly.getArea() == kAreaAvoid ? 1 : 0;
            for (int j = 0; j < pd.triCount; ++j) {
                const unsigned char* t = &tile->detailTris[(pd.triBase + static_cast<unsigned int>(j)) * 4];
                for (int k = 0; k < 3; ++k) {
                    const float* v = t[k] < poly.vertCount
                                         ? &tile->verts[poly.verts[t[k]] * 3]
                                         : &tile->detailVerts[(pd.vertBase + t[k] - poly.vertCount) * 3];
                    state.triangles.push_back(Vec3{v[0], v[1], v[2]} + up);
                }
                state.areas.push_back(area);
            }
            // Bordes: aristas sin vecino (ni en esta baldosa ni en la de al lado).
            for (int j = 0; j < poly.vertCount; ++j) {
                bool boundary = poly.neis[j] == 0;
                if (!boundary && (poly.neis[j] & DT_EXT_LINK)) {
                    boundary = true;
                    for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
                        if (tile->links[l].edge == j) {
                            boundary = false;
                            break;
                        }
                    }
                }
                if (!boundary) continue;
                const float* a = &tile->verts[poly.verts[j] * 3];
                const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                state.edges.push_back(Vec3{a[0], a[1], a[2]} + up * 1.5f);
                state.edges.push_back(Vec3{b[0], b[1], b[2]} + up * 1.5f);
            }
        }
        debug_dirty = true;
    }

    // Las baldosas vecinas no rehacen sus bordes al llegar esta; se refresca
    // el dibujo de todas al rehacer el conjunto (barato: solo copia).
    void rebuildDebug() {
        if (!debug_dirty) return;
        debug_dirty = false;
        debug.triangles.clear();
        debug.triangle_area.clear();
        debug.edges.clear();
        if (navmesh != nullptr) {
            for (auto& [key, tile] : tiles) {
                if (!tile.has_data) continue;
                const dtTileRef ref = navmesh->getTileRefAt(tileX(key), tileZ(key), 0);
                if (ref != 0) buildTileDebug(ref, tile);
                debug.triangles.insert(debug.triangles.end(), tile.triangles.begin(), tile.triangles.end());
                debug.triangle_area.insert(debug.triangle_area.end(), tile.areas.begin(), tile.areas.end());
                debug.edges.insert(debug.edges.end(), tile.edges.begin(), tile.edges.end());
            }
        }
        debug_dirty = false;
        ++debug.version;
    }

    // ---------------------------------------------------------------- agentes
    dtCrowdAgentParams crowdParams(const NavAgent& a) const {
        dtCrowdAgentParams p{};
        p.radius = std::max(a.radius, 0.05f);
        p.height = std::max(a.height, 0.1f);
        p.maxAcceleration = std::max(a.acceleration, 0.1f);
        p.maxSpeed = std::max(a.speed, 0.0f);
        p.collisionQueryRange = p.radius * 12.0f;
        p.pathOptimizationRange = p.radius * 30.0f;
        p.separationWeight = 2.0f;
        p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
        if (a.avoidance) p.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        p.obstacleAvoidanceType = 3;
        p.queryFilterType = 0;
        return p;
    }

    static bool sameParams(const NavAgent& a, const NavAgent& b) {
        return a.speed == b.speed && a.acceleration == b.acceleration && a.radius == b.radius && a.height == b.height &&
               a.avoidance == b.avoidance;
    }

    bool nearest(const Vec3& p, dtPolyRef& ref, float out[3], float extent_xz = 2.0f, float extent_y = 4.0f) const {
        if (query == nullptr) return false;
        const float center[3] = {p.x, p.y, p.z};
        const float extents[3] = {extent_xz, extent_y, extent_xz};
        ref = 0;
        return dtStatusSucceed(query->findNearestPoly(center, extents, &filter, &ref, out)) && ref != 0;
    }

    void sendTarget(Agent& agent) {
        if (crowd == nullptr || agent.index < 0 || !agent.has_target) return;
        dtPolyRef ref = 0;
        float pos[3];
        if (nearest(agent.target, ref, pos)) {
            crowd->requestMoveTarget(agent.index, ref, pos);
        } else {
            agent.has_target = false;
        }
        agent.target_sent = true;
    }

    void updateAgents(ecs::World& w, float dt) {
        std::unordered_set<entt::entity> seen;
        for (const entt::entity handle : w.registry().view<NavAgent>()) {
            const ecs::Entity e = w.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            seen.insert(handle);
            Agent& agent = agents[handle];
            const NavAgent& comp = e.get<NavAgent>();
            const Vec3 feet = toNav(e.worldPosition() - Vec3{0.0f, comp.base_offset, 0.0f});
            if (crowd == nullptr) continue;
            if (agent.index < 0) {
                dtPolyRef ref = 0;
                float pos[3];
                if (!nearest(feet, ref, pos)) continue;  // aun no hay malla bajo el
                const dtCrowdAgentParams p = crowdParams(comp);
                agent.index = crowd->addAgent(pos, &p);
                agent.params = comp;
                agent.written_valid = false;
                agent.target_sent = false;
                if (agent.index < 0) continue;
            }
            if (!sameParams(agent.params, comp)) {
                const dtCrowdAgentParams p = crowdParams(comp);
                crowd->updateAgentParameters(agent.index, &p);
                agent.params = comp;
            }
            if (agent.has_target && !agent.target_sent) sendTarget(agent);

            // Movido desde fuera (script, fisica): la multitud se entera.
            dtCrowdAgent* ag = crowd->getEditableAgent(agent.index);
            if (ag == nullptr || !ag->active) continue;
            if (agent.written_valid) {
                const Vec3 moved = feet - agent.written;
                const float horizontal = std::sqrt(moved.x * moved.x + moved.z * moved.z);
                if (horizontal > 1.0f) {
                    dtPolyRef ref = 0;
                    float pos[3];
                    if (nearest(feet, ref, pos)) {
                        ag->corridor.reset(ref, pos);
                        dtVcopy(ag->npos, pos);
                        ag->boundary.reset();
                        ag->topologyOptTime = 0.0f;
                        agent.target_sent = false;
                        if (agent.has_target) sendTarget(agent);
                    }
                } else if (horizontal > 0.005f) {
                    const float pos[3] = {feet.x, feet.y, feet.z};
                    ag->corridor.movePosition(pos, query, &filter);
                    dtVcopy(ag->npos, ag->corridor.getPos());
                }
            }
        }
        for (auto it = agents.begin(); it != agents.end();) {
            if (!seen.contains(it->first)) {
                if (crowd != nullptr && it->second.index >= 0) crowd->removeAgent(it->second.index);
                it = agents.erase(it);
            } else {
                ++it;
            }
        }
        if (crowd == nullptr) return;
        crowd->update(std::clamp(dt, 0.0f, 0.1f), nullptr);

        for (auto& [handle, agent] : agents) {
            if (agent.index < 0) continue;
            const dtCrowdAgent* ag = crowd->getAgent(agent.index);
            if (ag == nullptr || !ag->active) continue;
            ecs::Entity e = w.wrap(handle);
            const NavAgent& comp = e.get<NavAgent>();
            const Vec3 pos{ag->npos[0], ag->npos[1], ag->npos[2]};
            const Vec3 vel{ag->vel[0], ag->vel[1], ag->vel[2]};

            // Llegada.
            if (agent.has_target) {
                if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED) {
                    agent.has_target = false;
                } else if (ag->targetState == DT_CROWDAGENT_TARGET_VALID && ag->ncorners > 0 &&
                           (ag->cornerFlags[ag->ncorners - 1] & DT_STRAIGHTPATH_END)) {
                    const float* end = &ag->cornerVerts[(ag->ncorners - 1) * 3];
                    const float dx = end[0] - pos.x, dz = end[2] - pos.z;
                    if (std::sqrt(dx * dx + dz * dz) <= std::max(comp.stopping_distance, 0.02f)) {
                        crowd->resetMoveTarget(agent.index);
                        agent.has_target = false;
                        // Se para en el destino (sin la inercia, que a velocidad
                        // alta lo llevaria metros mas alla frenando).
                        dtCrowdAgent* editable = crowd->getEditableAgent(agent.index);
                        dtVset(editable->vel, 0.0f, 0.0f, 0.0f);
                        dtVset(editable->nvel, 0.0f, 0.0f, 0.0f);
                        dtVset(editable->dvel, 0.0f, 0.0f, 0.0f);
                    }
                }
            }

            const bool body = physics != nullptr && physics->running() && physics->hasBody(e) &&
                              e.has<physics::Rigidbody>() &&
                              e.get<physics::Rigidbody>().type == physics::BodyType::Dynamic;
            if (body) {
                const Vec3 current = physics->linearVelocity(e);
                physics->setLinearVelocity(e, Vec3{vel.x, current.y, vel.z});
                agent.written = toNav(e.worldPosition() - Vec3{0.0f, comp.base_offset, 0.0f});
            } else {
                e.setWorldPosition(fromNav(pos) + Vec3{0.0f, comp.base_offset, 0.0f});
                agent.written = pos;
            }
            agent.written_valid = true;

            if (comp.rotate_to_movement && vel.x * vel.x + vel.z * vel.z > 0.01f) {
                const float target = std::atan2(-vel.x, -vel.z) * 57.2957795f;
                Vec3 euler = e.localEulerDegrees();
                float diff = std::fmod(target - euler.y + 540.0f, 360.0f) - 180.0f;
                const float step = comp.turn_speed * dt;
                diff = std::clamp(diff, -step, step);
                euler.y += diff;
                e.setLocalEulerDegrees(euler);
            }
        }
    }

    std::vector<Vec3> cornersOf(const Agent& agent, int max_corners) const {
        std::vector<Vec3> out;
        if (crowd == nullptr || agent.index < 0 || query == nullptr) return out;
        const dtCrowdAgent* ag = crowd->getAgent(agent.index);
        if (ag == nullptr || !ag->active || ag->targetState != DT_CROWDAGENT_TARGET_VALID) return out;
        std::vector<float> verts(static_cast<std::size_t>(max_corners) * 3);
        std::vector<unsigned char> flags(static_cast<std::size_t>(max_corners));
        std::vector<dtPolyRef> refs(static_cast<std::size_t>(max_corners));
        // findCorners no es const: copia del pasillo.
        dtPathCorridor corridor;
        // setCorridor exige npath < capacidad (estricto): un hueco de mas.
        corridor.init(ag->corridor.getPathCount() + 1);
        corridor.reset(ag->corridor.getFirstPoly(), ag->npos);
        corridor.setCorridor(ag->corridor.getTarget(), ag->corridor.getPath(), ag->corridor.getPathCount());
        const int n = corridor.findCorners(verts.data(), flags.data(), refs.data(), max_corners, query, &filter);
        out.push_back(Vec3{ag->npos[0], ag->npos[1], ag->npos[2]});
        for (int i = 0; i < n; ++i) {
            out.push_back(Vec3{verts[static_cast<std::size_t>(i) * 3], verts[static_cast<std::size_t>(i) * 3 + 1],
                               verts[static_cast<std::size_t>(i) * 3 + 2]});
        }
        return out;
    }
};

// ================================================================================

NavigationSystem::NavigationSystem() : impl_(std::make_unique<Impl>()) {}
NavigationSystem::~NavigationSystem() = default;

void NavigationSystem::setSettings(const NavigationSettings& settings) {
    Impl& d = *impl_;
    const bool geometry = !d.settings.sameGeometry(settings);
    const bool threads = d.settings.build_threads != settings.build_threads;
    const bool agents = d.settings.max_agents != settings.max_agents;
    d.settings = settings;
    d.settings.cell_size = std::max(d.settings.cell_size, 0.02f);
    d.settings.cell_height = std::max(d.settings.cell_height, 0.01f);
    d.settings.tile_size = std::clamp(d.settings.tile_size, 16, 256);
    if (threads) {
        d.stopWorkers();
        for (auto& [key, tile] : d.tiles) tile.in_flight = false;
        d.startWorkers();
        d.resetTiles();
    }
    if (geometry || agents) {
        // La rejilla cambia: malla nueva y todo sucio.
        d.resetTiles();
        d.freeMesh();
        d.tiles.clear();
        d.active_tiles.clear();
        d.volumes_signature = 0;
        d.volumes.clear();
        d.dirty_queue.clear();
        d.dirty_set.clear();
        for (auto& [handle, src] : d.sources) src.bucket_tile = 0.0f;
    }
    d.applyCosts();
}

const NavigationSettings& NavigationSystem::settings() const { return impl_->settings; }
void NavigationSystem::setMeshProvider(MeshProvider provider) { impl_->mesh_provider = std::move(provider); }
void NavigationSystem::setTerrainProvider(TerrainProvider provider) { impl_->terrain_provider = std::move(provider); }
void NavigationSystem::setPhysics(physics::PhysicsSystem* physics) { impl_->physics = physics; }

void NavigationSystem::update(ecs::World& world, float delta_seconds, bool simulate_agents, bool rebuild) {
    Impl& d = *impl_;
    if (d.world != &world) {
        d.world = &world;
    }
    d.time += delta_seconds;
    // Sin generacion en tiempo real: solo la primera vez (malla vacia).
    const bool scan = rebuild || d.built_once == 0 || d.navmesh == nullptr;
    if (scan) {
        d.scanVolumes(world);
        d.scanModifiers(world);
        d.scanSources(world, delta_seconds);
        if (d.dirty_all) {
            d.dirty_all = false;
            for (const TileKey key : d.active_tiles) d.markTile(key);
        }
    }
    d.collect();
    d.dispatch();
    if (simulate_agents) d.updateAgents(world, delta_seconds);
}

void NavigationSystem::waitForBuild(ecs::World& world, float timeout_seconds) {
    Impl& d = *impl_;
    const auto start = std::chrono::steady_clock::now();
    update(world, 0.0f, false);
    while (!d.dirty_queue.empty() || d.in_flight > 0) {
        if (std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() > timeout_seconds) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        d.collect();
        d.dispatch();
    }
}

void NavigationSystem::rebuildAll() {
    Impl& d = *impl_;
    d.resetTiles();
    for (auto& [handle, src] : d.sources) src.signature = 0;  // se recalcula su geometria
}

void NavigationSystem::clear() {
    Impl& d = *impl_;
    d.resetTiles();
    d.dirty_all = false;
    d.freeMesh();
    d.agents.clear();
    d.sources.clear();
    d.modifiers.clear();
    d.volumes.clear();
    d.volumes_signature = 0;
    d.active_tiles.clear();
    d.tiles.clear();
    d.dirty_queue.clear();
    d.dirty_set.clear();
    d.built_once = 0;
    d.debug_dirty = true;
    d.rebuildDebug();
}

void NavigationSystem::resetAgents() {
    Impl& d = *impl_;
    if (d.crowd != nullptr) {
        for (auto& [handle, agent] : d.agents) {
            if (agent.index >= 0) d.crowd->removeAgent(agent.index);
        }
    }
    d.agents.clear();
}

bool NavigationSystem::ready() const {
    for (const auto& [key, tile] : impl_->tiles) {
        if (tile.has_data) return true;
    }
    return false;
}

bool NavigationSystem::building() const { return !impl_->dirty_queue.empty() || impl_->in_flight > 0; }

NavStats NavigationSystem::stats() const {
    const Impl& d = *impl_;
    NavStats s;
    for (const auto& [key, tile] : d.tiles) {
        if (!tile.has_data) continue;
        ++s.tiles;
        s.polygons += tile.polygons;
        s.memory_bytes += tile.bytes;
    }
    s.pending_tiles = static_cast<int>(d.dirty_queue.size()) + d.in_flight;
    s.agents = static_cast<int>(d.agents.size());
    s.last_tile_ms = d.last_tile_ms;
    return s;
}

const NavDebugMesh& NavigationSystem::debugMesh() const {
    impl_->rebuildDebug();
    return impl_->debug;
}

bool NavigationSystem::findPath(const Vec3& from, const Vec3& to, std::vector<Vec3>& path, bool* partial) const {
    const Impl& d = *impl_;
    path.clear();
    if (partial != nullptr) *partial = false;
    dtPolyRef start_ref = 0, end_ref = 0;
    float start[3], end[3];
    if (!d.nearest(d.toNav(from), start_ref, start) || !d.nearest(d.toNav(to), end_ref, end)) return false;
    constexpr int kMaxPolys = 1024;
    std::vector<dtPolyRef> polys(kMaxPolys);
    int count = 0;
    const dtStatus status = d.query->findPath(start_ref, end_ref, start, end, &d.filter, polys.data(), &count, kMaxPolys);
    if (dtStatusFailed(status) || count == 0) return false;
    const bool is_partial = polys[static_cast<std::size_t>(count - 1)] != end_ref || dtStatusDetail(status, DT_PARTIAL_RESULT);
    if (partial != nullptr) *partial = is_partial;
    float target[3];
    dtVcopy(target, end);
    if (is_partial) {
        d.query->closestPointOnPoly(polys[static_cast<std::size_t>(count - 1)], end, target, nullptr);
    }
    constexpr int kMaxPoints = 256;
    std::vector<float> points(kMaxPoints * 3);
    int npoints = 0;
    d.query->findStraightPath(start, target, polys.data(), count, points.data(), nullptr, nullptr, &npoints, kMaxPoints);
    for (int i = 0; i < npoints; ++i) {
        path.push_back(d.fromNav(Vec3{points[static_cast<std::size_t>(i) * 3], points[static_cast<std::size_t>(i) * 3 + 1],
                                      points[static_cast<std::size_t>(i) * 3 + 2]}));
    }
    return !path.empty();
}

bool NavigationSystem::projectPoint(const Vec3& point, Vec3& result, float extent) const {
    dtPolyRef ref = 0;
    float out[3];
    if (!impl_->nearest(impl_->toNav(point), ref, out, extent, extent)) return false;
    result = impl_->fromNav(Vec3{out[0], out[1], out[2]});
    return true;
}

bool NavigationSystem::randomPoint(const Vec3& center, float radius, Vec3& result) const {
    const Impl& d = *impl_;
    dtPolyRef ref = 0;
    float start[3];
    if (!d.nearest(d.toNav(center), ref, start, std::max(radius, 2.0f), 4.0f)) return false;
    // Detour elige un poligono que toca el circulo y un punto cualquiera de
    // el: puede caer fuera del radio. Se repite hasta que cae dentro.
    const float r = std::max(radius, 0.01f);
    for (int attempt = 0; attempt < 32; ++attempt) {
        dtPolyRef random_ref = 0;
        float point[3];
        if (dtStatusFailed(d.query->findRandomPointAroundCircle(ref, start, r, &d.filter, frand, &random_ref, point))) {
            return false;
        }
        const float dx = point[0] - start[0], dz = point[2] - start[2];
        if (dx * dx + dz * dz <= r * r) {
            result = d.fromNav(Vec3{point[0], point[1], point[2]});
            return true;
        }
    }
    result = d.fromNav(Vec3{start[0], start[1], start[2]});
    return true;
}

bool NavigationSystem::raycast(const Vec3& from, const Vec3& to, Vec3* hit) const {
    const Impl& d = *impl_;
    dtPolyRef ref = 0;
    float start[3];
    if (!d.nearest(d.toNav(from), ref, start)) {
        if (hit != nullptr) *hit = from;
        return false;
    }
    const Vec3 to_nav = d.toNav(to);
    const float end[3] = {to_nav.x, to_nav.y, to_nav.z};
    float t = 0.0f;
    float normal[3];
    dtPolyRef path[256];
    int count = 0;
    d.query->raycast(ref, start, end, &d.filter, &t, normal, path, &count, 256);
    if (t > 1.0f) return true;  // FLT_MAX: llega sin chocar
    if (hit != nullptr) {
        *hit = d.fromNav(Vec3{start[0] + (end[0] - start[0]) * t, start[1] + (end[1] - start[1]) * t,
                              start[2] + (end[2] - start[2]) * t});
    }
    return false;
}

bool NavigationSystem::moveTo(ecs::Entity agent_entity, const Vec3& target) {
    Impl& d = *impl_;
    if (!agent_entity.valid() || !agent_entity.has<NavAgent>()) return false;
    Impl::Agent& agent = d.agents[agent_entity.handle()];
    agent.has_target = true;
    agent.target = d.toNav(target);
    agent.target_sent = false;
    if (d.crowd != nullptr && agent.index >= 0) {
        d.sendTarget(agent);
        return agent.has_target;
    }
    // Sin malla todavia: se manda en cuanto el agente entre en la multitud.
    if (d.query != nullptr && ready() && !building()) {
        dtPolyRef ref = 0;
        float pos[3];
        return d.nearest(d.toNav(target), ref, pos);
    }
    return true;
}

void NavigationSystem::stop(ecs::Entity agent_entity) {
    Impl& d = *impl_;
    const auto it = d.agents.find(agent_entity.handle());
    if (it == d.agents.end()) return;
    it->second.has_target = false;
    if (d.crowd != nullptr && it->second.index >= 0) d.crowd->resetMoveTarget(it->second.index);
}

bool NavigationSystem::isMoving(ecs::Entity agent_entity) const {
    const auto it = impl_->agents.find(agent_entity.handle());
    return it != impl_->agents.end() && it->second.has_target;
}

float NavigationSystem::remainingDistance(ecs::Entity agent_entity) const {
    const auto it = impl_->agents.find(agent_entity.handle());
    if (it == impl_->agents.end() || !it->second.has_target) return 0.0f;
    const std::vector<Vec3> corners = impl_->cornersOf(it->second, 64);
    if (corners.size() < 2) return core::length(impl_->fromNav(it->second.target) - agent_entity.worldPosition());
    float total = 0.0f;
    for (std::size_t i = 1; i < corners.size(); ++i) total += core::length(corners[i] - corners[i - 1]);
    return total;
}

Vec3 NavigationSystem::agentVelocity(ecs::Entity agent_entity) const {
    const Impl& d = *impl_;
    const auto it = d.agents.find(agent_entity.handle());
    if (it == d.agents.end() || d.crowd == nullptr || it->second.index < 0) return Vec3{};
    const dtCrowdAgent* ag = d.crowd->getAgent(it->second.index);
    return ag != nullptr && ag->active ? Vec3{ag->vel[0], ag->vel[1], ag->vel[2]} : Vec3{};
}

std::vector<Vec3> NavigationSystem::agentPath(ecs::Entity agent_entity) const {
    const auto it = impl_->agents.find(agent_entity.handle());
    if (it == impl_->agents.end() || !it->second.has_target) return {};
    std::vector<Vec3> corners = impl_->cornersOf(it->second, 64);
    for (Vec3& c : corners) c = impl_->fromNav(c);
    return corners;
}

void NavigationSystem::shiftOrigin(const Vec3& offset) {
    // La malla y los agentes siguen en espacio nav: solo cambia la conversion.
    impl_->nav_offset = impl_->nav_offset + offset;
}

Vec3 NavigationSystem::navToLocal() const { return Vec3{} - impl_->nav_offset; }

}  // namespace cramion::navigation
