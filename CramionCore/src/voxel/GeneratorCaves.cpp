// Entradas de cueva del generador Aventura (VoxelWorld::cave_entrances).
//
// Las cuevas naturales (tuneles y cavernas de ruido 3D) casi nunca llegan a la
// superficie: la capa de arriba solo se abre en algunas manchas y nunca junto
// al agua, y donde se abre es un agujero vertical. Aqui se excavan entradas
// que se pueden recorrer andando, una como mucho por celda de 64 x 64 m:
//
//   - Boca en una ladera o acantilado: un tunel que entra en horizontal en la
//     colina y luego baja (1 bloque cada 2) hasta dar con una cueva.
//   - Sima en el llano: un embudo de escalones de 1 bloque y, desde el fondo,
//     el mismo tunel que baja.
//   - Grieta (poco frecuente): un barranco largo con las paredes en
//     escalones de 2 y el fondo que sube suave hacia los extremos; desde el
//     centro, un tunel hacia la cueva.
//
// Todo depende de (semilla, celda) y de las columnas del generador: funcion
// pura, igual en cualquier chunk y en cualquier orden. No se excava junto al
// agua (ni mar, ni lago, ni rio) ni cerca de aldeas, ciudades o caminos.
// Junto a las bocas, setas luminosas y brotes de cristal (se ven de noche).
//
// Y la densidad de las cuevas, con las cuevas inmensas lejos del pueblo de
// inicio (salas, barrancos, pozos, lagos, lava, grutas y minas): mas abajo.

#include "VoxelInternal.h"
#include "Structures.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace cramion::voxel {

namespace {

std::uint32_t hash3(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float rand01(int x, int y, int z, std::uint32_t seed) {
    return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFF) / 16777215.0f;
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }
int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
int ifloor(float v) { return static_cast<int>(std::floor(v)); }

constexpr int kCell = 64;        // una entrada como mucho por celda
constexpr int kReach = 136;      // lo mas lejos que excava una entrada desde su celda
constexpr int kTunnelMax = 80;   // pasos (1 bloque) de un tunel como mucho
constexpr int kMinFloor = 10;    // los tuneles no bajan de aqui

enum PointFlags : std::uint8_t { kBridge = 1, kRavine = 2, kChamber = 4 };

using AB = Generator::AdventureBiome;

}  // namespace

struct Generator::CaveEntrance {
    struct Point {
        float x = 0.0f, z = 0.0f;
        int floor = 0;  // ultimo bloque macizo (se excava desde floor + 1)
        float r = 2.0f;
        std::uint8_t flags = 0;
    };
    CaveEntranceInfo info;
    std::vector<Point> points;
    bool bowl = false;  // sima: embudo de escalones de 1
    float bx = 0.0f, bz = 0.0f, br = 0.0f;
    int bottom = 0;
    std::vector<std::pair<int, int>> glow;  // columnas con seta luminosa o cristal
};

void Generator::setCaveEntrances(int level) {
    cave_entrances_ = std::clamp(level, 0, 2);
    if (entrances_) {
        std::lock_guard lock(entrances_->mutex);
        entrances_->cells.clear();
    }
}

// --- Densidad de las cuevas (la misma que generateAdventure) ----------------------------

float Generator::caveDensity(float wx, float wy, float wz) const {
    // Tuneles anchos: donde dos ruidos se anulan a la vez.
    const float a = noise3(wx / 56.0f, wy / 36.0f, wz / 56.0f, 211);
    const float b = noise3(wx / 56.0f, wy / 36.0f, wz / 56.0f, 223);
    const float tunnel = 0.11f - std::sqrt(a * a + b * b);
    // Cavernas grandes en lo hondo.
    const float cavern = (noise3(wx / 80.0f, wy / 44.0f, wz / 80.0f, 237) - 0.36f) * std::clamp((52.0f - wy) / 28.0f, 0.0f, 1.0f);
    // Lejos del pueblo de inicio, las cuevas inmensas (abajo); cerca, las de siempre.
    const float deep = caveDeep(wx, wz);
    if (deep <= 0.0f) return std::max(tunnel, cavern);
    const float d = std::max({tunnel, cavern, immenseDensity(wx, wy, wz, deep, a, b)});
    return d - liquidRing(wx, wy, wz, deep);
}

float Generator::caveField(int x, int y, int z) const {
    constexpr int kStep = 4;
    const int gx = floorDiv(x, kStep) * kStep, gy = floorDiv(y, kStep) * kStep, gz = floorDiv(z, kStep) * kStep;
    const float fx = static_cast<float>(x - gx) / kStep, fy = static_cast<float>(y - gy) / kStep, fz = static_cast<float>(z - gz) / kStep;
    const auto G = [&](int ix, int iy, int iz) {
        return caveDensity(static_cast<float>(gx + ix * kStep), static_cast<float>(gy + iy * kStep), static_cast<float>(gz + iz * kStep));
    };
    const float c00 = lerp(G(0, 0, 0), G(1, 0, 0), fx);
    const float c10 = lerp(G(0, 0, 1), G(1, 0, 1), fx);
    const float c01 = lerp(G(0, 1, 0), G(1, 1, 0), fx);
    const float c11 = lerp(G(0, 1, 1), G(1, 1, 1), fx);
    return lerp(lerp(c00, c10, fz), lerp(c01, c11, fz), fy);
}

// --- Una entrada por celda --------------------------------------------------------------

std::shared_ptr<const Generator::CaveEntrance> Generator::caveEntrance(int gx, int gz) const {
    if (cave_entrances_ <= 0 || profile_ != VoxelGenerator::Adventure) return nullptr;
    const std::int64_t key = (static_cast<std::int64_t>(gx) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(gz));
    if (entrances_) {
        std::lock_guard lock(entrances_->mutex);
        if (const auto it = entrances_->cells.find(key); it != entrances_->cells.end()) return it->second;
    }

    std::shared_ptr<CaveEntrance> made;
    const auto s = static_cast<std::uint32_t>(seed_) + 7919u;
    const float level = static_cast<float>(cave_entrances_);
    const auto column = [&](float x, float z) { return adventureColumn(ifloor(x), ifloor(z)); };
    const auto heightAt = [&](float x, float z) { return column(x, z).height; };
    // Seco: ni mar, ni lago, ni rio en el punto ni a `margin` de el.
    const auto dry = [&](float x, float z, float margin) {
        for (int k = 0; k < 9; ++k) {
            const float a = static_cast<float>(k) * 0.7853982f;
            const float m = k == 8 ? 0.0f : margin;
            const AdventureColumn c = column(x + std::cos(a) * m, z + std::sin(a) * m);
            if (c.water > c.height || c.lake || c.river > 0.08f || c.height <= sea_ + 1) return false;
        }
        return true;
    };
    const auto blocked = [&](float x, float z, float margin) {
        return structures_ && structures_->blocksTrees(ifloor(x), ifloor(z), margin);
    };

    // Tunel desde (x, z) con el suelo en y0: `flat` pasos en horizontal y luego
    // baja 1 bloque cada 2 hasta dar con una cueva natural (o acaba en una sala).
    const auto walk = [&](CaveEntrance& e, float x, float z, float angle, int y0, int flat, float r, int salt) {
        bool connected = false;
        int extra = 0;
        const std::size_t first = e.points.size();
        for (int i = 0; i < kTunnelMax; ++i) {
            const int floor_y = y0 - std::max(0, i - flat) / 2;
            if (floor_y < std::max(kMinFloor, caveFloorLimit(x, z))) break;  // ni al agua ni a la lava de las cuevas inmensas
            if (extra == 0 && (i % 2) == 0 && (!dry(x, z, r + 3.0f) || blocked(x, z, r + 8.0f))) break;
            e.points.push_back({x, z, floor_y, r, static_cast<std::uint8_t>(extra > 0 ? 0 : kBridge)});
            if (extra > 0) {
                if (--extra == 0) break;
            } else if (i >= flat + 10 && floor_y <= heightAt(x, z) - 9 && caveField(ifloor(x), floor_y + 2, ifloor(z)) > 0.0f &&
                       caveField(ifloor(x), floor_y + 1, ifloor(z)) > 0.0f) {
                connected = true;  // entra en la cueva: unos pasos mas, sin suelo puesto
                extra = 3;
            }
            angle += (rand01(salt, i, gx * 31 + gz, s + 5u) - 0.5f) * 0.3f;
            x += std::cos(angle);
            z += std::sin(angle);
        }
        if (e.points.size() > first && !connected) {
            CaveEntrance::Point& end = e.points.back();
            end.r = 3.3f;
            end.flags |= kChamber;  // una sala al final
        }
        return connected;
    };

    const float px = static_cast<float>(gx * kCell) + 4.0f + rand01(gx, 1, gz, s) * (kCell - 8);
    const float pz = static_cast<float>(gz * kCell) + 4.0f + rand01(gx, 2, gz, s) * (kCell - 8);
    const AdventureColumn c = column(px, pz);
    const bool land = c.height > sea_ + 3 && c.water <= c.height && !c.lake && c.biome != AB::AOcean && c.biome != AB::ARiver &&
                      c.biome != AB::ABeach && c.height < kWorldHeight - 40;
    if (land) {
        const float hx = static_cast<float>(heightAt(px + 4.0f, pz) - heightAt(px - 4.0f, pz));
        const float hz = static_cast<float>(heightAt(px, pz + 4.0f) - heightAt(px, pz - 4.0f));
        const float grad = std::sqrt(hx * hx + hz * hz) / 8.0f;
        const bool mountain = c.biome == AB::AMountains || c.height > sea_ + 35 || grad > 0.5f;
        const float chance = std::min(0.9f, (mountain ? 0.55f : 0.3f) * level);
        if (rand01(gx, 3, gz, s) < chance) {
            const float kind = rand01(gx, 4, gz, s);
            const float r = 1.6f + rand01(gx, 5, gz, s) * 0.7f;
            auto e = std::make_shared<CaveEntrance>();
            bool ok = false;
            if (kind < 0.08f) {
                // Grieta: un barranco largo; el fondo sube suave hacia los dos extremos.
                const float a = rand01(gx, 6, gz, s) * 6.2831853f;
                const float length = 50.0f + rand01(gx, 7, gz, s) * 40.0f;
                const float depth = std::min(10.0f + rand01(gx, 8, gz, s) * 10.0f, length / 4.0f);
                const float w0 = 1.0f + rand01(gx, 9, gz, s) * 0.8f;
                const int n = static_cast<int>(length);
                std::vector<CaveEntrance::Point> rav;
                float x = px, z = pz, ang = a;
                ok = true;
                for (int i = 0; i <= n && ok; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(n);
                    if ((i % 3) == 0 && (!dry(x, z, w0 + depth * 0.5f + 3.0f) || blocked(x, z, w0 + depth * 0.5f + 10.0f))) ok = false;
                    const int f = heightAt(x, z) - static_cast<int>(std::lround(depth * std::sin(3.1415927f * t)));
                    rav.push_back({x, z, std::max(f, kMinFloor), w0, static_cast<std::uint8_t>(kRavine | kBridge)});
                    ang += (rand01(i, 21, gx * 7 + gz, s) - 0.5f) * 0.12f;
                    x += std::cos(ang);
                    z += std::sin(ang);
                }
                if (ok) {
                    // Suelo sin escalones de mas de 1 a lo largo.
                    for (std::size_t i = 1; i < rav.size(); ++i) rav[i].floor = std::min(rav[i].floor, rav[i - 1].floor + 1);
                    for (std::size_t i = rav.size() - 1; i-- > 0;) rav[i].floor = std::min(rav[i].floor, rav[i + 1].floor + 1);
                    e->points = rav;
                    const CaveEntrance::Point mid = rav[rav.size() / 2];
                    const float side = rand01(gx, 10, gz, s) < 0.5f ? 1.5707963f : -1.5707963f;
                    e->info.connected = walk(*e, mid.x, mid.z, a + side, mid.floor, 0, r, 3);
                    e->info.kind = 2;
                    e->info.x = ifloor(rav.front().x);
                    e->info.z = ifloor(rav.front().z);
                    // Setas en el fondo de la grieta.
                    for (std::size_t i = 4; i < rav.size(); i += 9) e->glow.emplace_back(ifloor(rav[i].x), ifloor(rav[i].z));
                }
            } else if (grad >= 0.3f) {
                // Boca en una ladera: entra en horizontal cuesta arriba y luego baja.
                const float a = std::atan2(hz, hx);
                const float sx = px - std::cos(a) * 3.0f, sz = pz - std::sin(a) * 3.0f;
                if (dry(sx, sz, r + 3.0f) && !blocked(sx, sz, r + 8.0f)) {
                    // El suelo de la boca a la altura del pie de la ladera (se entra sin trepar).
                    int y0 = heightAt(sx, sz);
                    for (int k = 1; k <= 3; ++k) {
                        y0 = std::min(y0, heightAt(sx - std::cos(a) * static_cast<float>(k), sz - std::sin(a) * static_cast<float>(k)));
                    }
                    const int flat = 8 + static_cast<int>(rand01(gx, 11, gz, s) * 6.0f);
                    e->info.connected = walk(*e, sx, sz, a, y0, flat, r, 1);
                    e->info.kind = 0;
                    e->info.x = ifloor(sx - std::cos(a) * 2.0f);
                    e->info.z = ifloor(sz - std::sin(a) * 2.0f);
                    ok = e->points.size() >= static_cast<std::size_t>(flat + 10);
                    // Luz a los lados de la boca.
                    for (std::size_t i = 1; i < std::min<std::size_t>(e->points.size(), static_cast<std::size_t>(flat + 6)); i += 3) {
                        const CaveEntrance::Point& p = e->points[i];
                        const float side = (i / 3) % 2 == 0 ? 1.0f : -1.0f;
                        e->glow.emplace_back(ifloor(p.x - std::sin(a) * side * (p.r - 0.6f)), ifloor(p.z + std::cos(a) * side * (p.r - 0.6f)));
                    }
                }
            } else {
                // Sima: embudo de escalones de 1 y, desde el fondo, el tunel.
                const float br = 5.0f + rand01(gx, 12, gz, s) * 3.0f;
                if (dry(px, pz, br + 4.0f) && !blocked(px, pz, br + 10.0f)) {
                    e->bowl = true;
                    e->bx = px;
                    e->bz = pz;
                    e->br = br;
                    e->bottom = c.height - static_cast<int>(br - 1.5f) - 1;
                    const float a = rand01(gx, 13, gz, s) * 6.2831853f;
                    e->info.connected = walk(*e, px, pz, a, e->bottom, 2, r, 2);
                    e->info.kind = 1;
                    e->info.x = ifloor(px - std::cos(a) * (br + 2.0f));
                    e->info.z = ifloor(pz - std::sin(a) * (br + 2.0f));
                    ok = e->points.size() >= 14;
                    for (int k = 0; k < 4; ++k) {
                        const float ga = a + static_cast<float>(k) * 1.5707963f + 0.6f;
                        e->glow.emplace_back(ifloor(px + std::cos(ga) * 2.0f), ifloor(pz + std::sin(ga) * 2.0f));
                    }
                }
            }
            if (ok && !e->points.empty()) {
                // Lo que excava (con el margen de las paredes de la grieta) y donde acaba.
                float x0 = e->bowl ? e->bx - e->br - 4.0f : 1e9f, x1 = e->bowl ? e->bx + e->br + 4.0f : -1e9f;
                float z0 = e->bowl ? e->bz - e->br - 4.0f : 1e9f, z1 = e->bowl ? e->bz + e->br + 4.0f : -1e9f;
                for (const CaveEntrance::Point& p : e->points) {
                    const float m = p.r + ((p.flags & kRavine) ? 13.0f : 1.0f);
                    x0 = std::min(x0, p.x - m);
                    x1 = std::max(x1, p.x + m);
                    z0 = std::min(z0, p.z - m);
                    z1 = std::max(z1, p.z + m);
                }
                e->info.min_x = ifloor(x0);
                e->info.max_x = ifloor(x1) + 1;
                e->info.min_z = ifloor(z0);
                e->info.max_z = ifloor(z1) + 1;
                e->info.y = heightAt(static_cast<float>(e->info.x) + 0.5f, static_cast<float>(e->info.z) + 0.5f) + 1;
                const CaveEntrance::Point& end = e->points.back();
                e->info.end_x = ifloor(end.x);
                e->info.end_y = end.floor + 1;
                e->info.end_z = ifloor(end.z);
                made = std::move(e);
            }
        }
    }
    if (entrances_) {
        std::lock_guard lock(entrances_->mutex);
        if (entrances_->cells.size() > 40000) entrances_->cells.clear();
        entrances_->cells[key] = made;
    }
    return made;
}

std::vector<std::shared_ptr<const Generator::CaveEntrance>> Generator::entrancesTouching(int x0, int z0, int x1, int z1) const {
    std::vector<std::shared_ptr<const CaveEntrance>> list;
    if (cave_entrances_ <= 0 || profile_ != VoxelGenerator::Adventure) return list;
    for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z1 + kReach, kCell); ++gz) {
        for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x1 + kReach, kCell); ++gx) {
            std::shared_ptr<const CaveEntrance> e = caveEntrance(gx, gz);
            if (!e || e->info.max_x < x0 || e->info.min_x > x1 || e->info.max_z < z0 || e->info.min_z > z1) continue;
            list.push_back(std::move(e));
        }
    }
    return list;
}

bool Generator::nearCaveEntrance(const std::vector<std::shared_ptr<const CaveEntrance>>& list, int x, int z, float margin) {
    const float fx = static_cast<float>(x) + 0.5f, fz = static_cast<float>(z) + 0.5f;
    for (const auto& e : list) {
        const int m = static_cast<int>(std::ceil(margin));
        if (x < e->info.min_x - m || x > e->info.max_x + m || z < e->info.min_z - m || z > e->info.max_z + m) continue;
        if (e->bowl) {
            const float dx = fx - e->bx, dz = fz - e->bz, r = e->br + 3.0f + margin;
            if (dx * dx + dz * dz < r * r) return true;
        }
        for (const CaveEntrance::Point& p : e->points) {
            const float dx = fx - p.x, dz = fz - p.z;
            const float r = p.r + margin + ((p.flags & kRavine) ? 12.0f : 0.0f);
            if (dx * dx + dz * dz < r * r) return true;
        }
    }
    return false;
}

std::vector<Generator::CaveEntranceInfo> Generator::caveEntrancesIn(int x0, int z0, int x1, int z1) const {
    std::vector<CaveEntranceInfo> out;
    if (cave_entrances_ <= 0 || profile_ != VoxelGenerator::Adventure) return out;
    for (int gz = floorDiv(z0 - kReach, kCell); gz <= floorDiv(z1 + kReach, kCell); ++gz) {
        for (int gx = floorDiv(x0 - kReach, kCell); gx <= floorDiv(x1 + kReach, kCell); ++gx) {
            const std::shared_ptr<const CaveEntrance> e = caveEntrance(gx, gz);
            if (e && e->info.x >= x0 && e->info.x < x1 && e->info.z >= z0 && e->info.z < z1) out.push_back(e->info);
        }
    }
    return out;
}

// --- Lo que excavan en un chunk ---------------------------------------------------------

void Generator::caveEntranceCarve(int x0, int z0, const std::array<int, kChunkSize * kChunkSize>& heights, EntranceCarve& out) const {
    out.any = false;
    out.lo.fill(kWorldHeight);
    out.hi.fill(-1);
    out.flags.fill(0);
    const auto list = entrancesTouching(x0, z0, x0 + kChunkSize - 1, z0 + kChunkSize - 1);
    if (list.empty()) return;
    constexpr int kTop = kWorldHeight - 1;
    for (const auto& e : list) {
        for (int lz = 0; lz < kChunkSize; ++lz) {
            const int wz = z0 + lz;
            if (wz < e->info.min_z || wz > e->info.max_z) continue;
            for (int lx = 0; lx < kChunkSize; ++lx) {
                const int wx = x0 + lx;
                if (wx < e->info.min_x || wx > e->info.max_x) continue;
                const std::size_t i = static_cast<std::size_t>(lx + lz * kChunkSize);
                const int h = heights[i];
                const float fx = static_cast<float>(wx) + 0.5f, fz = static_cast<float>(wz) + 0.5f;
                int lo = out.lo[i], hi = out.hi[i];
                std::uint8_t flags = out.flags[i];
                if (e->bowl) {
                    const float d = std::hypot(fx - e->bx, fz - e->bz);
                    if (d <= e->br + 3.0f) {
                        const int f = e->bottom + std::max(0, ifloor(d - 1.5f));
                        if (f < h) {
                            lo = std::min(lo, f + 1);
                            hi = kTop;
                            flags |= 1;
                        }
                    }
                }
                for (const CaveEntrance::Point& p : e->points) {
                    const float dx = fx - p.x, dz = fz - p.z;
                    if (p.flags & kRavine) {
                        if (std::abs(dx) > p.r + 13.0f || std::abs(dz) > p.r + 13.0f) continue;
                        const float d = std::sqrt(dx * dx + dz * dz);
                        if (d > p.r + 12.0f) continue;
                        // Paredes en escalones de 2 (cornisas), abierto al cielo.
                        const int f = p.floor + std::max(0, static_cast<int>(std::ceil((d - p.r) * 2.0f)));
                        if (f >= h) continue;
                        lo = std::min(lo, f + 1);
                        hi = kTop;
                        flags |= 1;
                    } else {
                        if (dx * dx + dz * dz > p.r * p.r) continue;
                        lo = std::min(lo, p.floor + 1);
                        // Con 1 bloque de techo o menos, abierto al cielo (sin cornisas finas).
                        hi = std::max(hi, h - p.floor <= 5 ? kTop : p.floor + ((p.flags & kChamber) ? 5 : 4));
                        if (p.flags & kBridge) flags |= 1;
                    }
                }
                out.lo[i] = lo;
                out.hi[i] = hi;
                out.flags[i] = flags;
                if (lo <= hi) out.any = true;
            }
        }
        for (const auto& [gx, gz] : e->glow) {
            const int lx = gx - x0, lz = gz - z0;
            if (lx >= 0 && lz >= 0 && lx < kChunkSize && lz < kChunkSize) out.flags[static_cast<std::size_t>(lx + lz * kChunkSize)] |= 2;
        }
    }
}


// --- Cuevas inmensas -------------------------------------------------------------------
//
// Lejos del pueblo de inicio (a mas de ~400 m del origen; del todo a ~650 m) las
// cuevas crecen: salas de mas de 100 m con pilares, galerias en varios pisos,
// chimeneas verticales, tuneles mas anchos en lo hondo, barrancos que bajan
// desde la superficie hasta la roca madre, pozos abiertos al cielo (los que caen
// en una zona de lagos acaban en agua), lagos subterraneos y mares de lava, grutas
// de setas luminosas, de cristal, de estalactitas y heladas, roca honda
// (Deepstone) con mas menas y galerias de mina con entibado y antorchas.
// Cerca del origen las cuevas son las de siempre (bloque a bloque).
//
// Lagos y lava sin paredes de liquido: el nivel es fijo en cada zona (agua a
// kLakeLevel, lava a kLavaLevel) y alrededor de la zona un anillo de roca
// cierra las cuevas por debajo de ese nivel; el liquido solo rellena el hueco
// dentro de la zona (mas alla de la mitad del anillo).

namespace {

constexpr float kDeepStart = 400.0f;  // hasta aqui, las cuevas de siempre
constexpr float kDeepFull = 650.0f;   // desde aqui, todo
constexpr int kLakeLevel = 26;        // agua de los lagos subterraneos (y de arriba)
constexpr int kLavaLevel = 10;        // lava del fondo
// Zonas de liquido (sobre el valor de la zona, ver liquidZone): el anillo de roca
// va de kRingIn a kRingOut y el liquido rellena desde kFill (en medio).
constexpr float kRingIn = 0.18f, kFill = 0.26f, kRingOut = 0.34f;
constexpr int kShaftCell = 96;    // un pozo como mucho por celda
constexpr int kShaftReach = 30;   // radio maximo de un pozo (con su boca y sus irregularidades)
constexpr int kMineCell = 176;    // una mina como mucho por celda
constexpr int kMineReach = 120;   // lo mas lejos que llega una galeria desde su celda
// La luz de las minas: si otro bloque de luz (farol...) sustituye a la antorcha, cambiar aqui.
constexpr BlockId kMineLight = block::Torch;

float smooth01(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

float Generator::caveDeep(float wx, float wz) const {
    if (profile_ != VoxelGenerator::Adventure || !immense_caves_) return 0.0f;
    const float d2 = wx * wx + wz * wz;
    if (d2 <= kDeepStart * kDeepStart) return 0.0f;
    return smooth01(kDeepStart, kDeepFull, std::sqrt(d2));
}

// Zona de liquido: > 0 agua (lagos), < 0 lava; |valor| frente a kRingIn/kFill/kRingOut.
// 0 cerca del origen.
float Generator::liquidZone(float wx, float wz, float deep) const {
    if (deep <= 0.0f) return 0.0f;
    const float lq = noise2(wx / 240.0f, wz / 240.0f, 271) * 1.45f;
    const float w = lq - (1.0f - deep), v = -lq - (1.0f - deep);
    if (w > 0.0f) return w;
    if (v > 0.0f) return -v;
    return 0.0f;
}

int Generator::caveFloorLimit(float wx, float wz) const {
    const float deep = caveDeep(wx, wz);
    const float z = liquidZone(wx, wz, deep);
    // En el anillo y dentro de la zona, no bajar al nivel del liquido.
    return std::abs(z) > kRingIn ? (z > 0.0f ? kLakeLevel : kLavaLevel) + 2 : 0;
}

float Generator::immenseDensity(float wx, float wy, float wz, float deep, float a, float b) const {
    // Tuneles mas anchos cuanto mas hondo.
    const float widen = 0.06f * std::clamp((64.0f - wy) / 44.0f, 0.0f, 1.0f);
    float d = 0.11f + widen - std::sqrt(a * a + b * b);
    // Salas inmensas entre y ~6 y un techo que varia (~40..60).
    const float roof = 50.0f + 13.0f * noise2(wx / 260.0f, wz / 260.0f, 263);
    const float band = smooth01(4.0f, 13.0f, wy) * (1.0f - smooth01(roof - 18.0f, roof, wy));
    if (band > 0.0f) {
        const float region = noise2(wx / 620.0f, wz / 620.0f, 265);
        const float n = noise3(wx / 210.0f, wy / 70.0f, wz / 210.0f, 251) + 0.28f * noise3(wx / 44.0f, wy / 24.0f, wz / 44.0f, 257);
        float hall = n - (0.24f - 0.2f * region) - (1.0f - band) * 0.9f;
        // Pilares de roca de suelo a techo.
        const float p = noise2(wx / 10.0f, wz / 10.0f, 269);
        if (p > 0.36f) hall -= (p - 0.36f) * 5.0f;
        d = std::max(d, hall);
    }
    // Galerias en pisos: cintas casi horizontales.
    if (wy > 12.0f && wy < 130.0f) {
        const float c = noise3(wx / 64.0f, wy / 12.0f, wz / 64.0f, 241);
        const float e = noise3(wx / 64.0f, wy / 12.0f, wz / 64.0f, 243);
        d = std::max(d, 0.05f - std::sqrt(c * c + e * e) - std::max(0.0f, (wy - 70.0f) / 400.0f));
    }
    // Chimeneas casi verticales que unen los pisos.
    {
        const float c = noise3(wx / 26.0f, wy / 110.0f, wz / 26.0f, 245);
        const float e = noise3(wx / 26.0f, wy / 110.0f, wz / 26.0f, 247);
        d = std::max(d, 0.045f - std::sqrt(c * c + e * e));
    }
    // Fundido con las cuevas de siempre cerca del origen.
    return d - (1.0f - deep) * 0.6f;
}

float Generator::liquidRing(float wx, float wy, float wz, float deep) const {
    const float z = liquidZone(wx, wz, deep);
    const float az = std::abs(z);
    if (az <= kRingIn || az >= kRingOut) return 0.0f;
    const float level = static_cast<float>(z > 0.0f ? kLakeLevel : kLavaLevel);
    return 3.0f * std::clamp((level + 3.0f - wy) / 2.0f, 0.0f, 1.0f);
}

// --- Barrancos y pozos (abiertos al cielo) ----------------------------------------------

// Pozo: uno como mucho por celda de kShaftCell.
struct Generator::CaveShaft {
    float x = 0.0f, z = 0.0f, r = 4.0f;
    int bottom = 8;
};

void Generator::caveShafts(int x0, int z0, int x1, int z1, std::vector<CaveShaft>& out) const {
    out.clear();
    if (profile_ != VoxelGenerator::Adventure) return;
    const auto s = static_cast<std::uint32_t>(seed_) + 1709u;
    for (int gz = floorDiv(z0 - kShaftReach, kShaftCell); gz <= floorDiv(z1 + kShaftReach, kShaftCell); ++gz) {
        for (int gx = floorDiv(x0 - kShaftReach, kShaftCell); gx <= floorDiv(x1 + kShaftReach, kShaftCell); ++gx) {
            if (rand01(gx, 1, gz, s) > 0.55f) continue;
            CaveShaft sh;
            sh.x = static_cast<float>(gx * kShaftCell) + 20.0f + rand01(gx, 2, gz, s) * static_cast<float>(kShaftCell - 40);
            sh.z = static_cast<float>(gz * kShaftCell) + 20.0f + rand01(gx, 3, gz, s) * static_cast<float>(kShaftCell - 40);
            const float size = rand01(gx, 4, gz, s);  // 1 de cada 6, una sima gigante
            sh.r = size < 0.17f ? 13.0f + size * 47.0f : 3.0f + (size - 0.17f) * 8.5f;
            const float reach = sh.r + 6.0f;
            if (sh.x + reach < static_cast<float>(x0) || sh.x - reach > static_cast<float>(x1 + 1) ||
                sh.z + reach < static_cast<float>(z0) || sh.z - reach > static_cast<float>(z1 + 1)) {
                continue;
            }
            if (caveDeep(sh.x, sh.z) < 0.6f) continue;
            sh.bottom = 6 + static_cast<int>(rand01(gx, 5, gz, s) * 14.0f);
            if (structures_ && structures_->blocksTrees(ifloor(sh.x), ifloor(sh.z), sh.r + 12.0f)) continue;
            out.push_back(sh);
        }
    }
}

float Generator::ravineLine(float wx, float wz) const {
    // Ejes girados y deformados en las dos direcciones: el ruido vale 0 a lo largo de
    // las lineas de su rejilla y sin esto saldrian barrancos rectos.
    const float u = (wx * 0.8f + wz * 0.6f) / 380.0f + noise2(wx / 90.0f, wz / 90.0f, 295) * 0.45f;
    const float v = (wz * 0.8f - wx * 0.6f) / 380.0f + noise2(wx / 90.0f, wz / 90.0f, 296) * 0.45f + 0.37f;
    return noise2(u, v, 293);
}

bool Generator::ravineAt(float wx, float wz, float deep, float& dist, float& width, float& strength) const {
    if (deep < 0.4f) return false;
    const float mask = noise2(wx / 700.0f, wz / 700.0f, 291);
    strength = smooth01(-0.2f, 0.0f, mask) * smooth01(0.4f, 0.9f, deep);
    if (strength <= 0.0f) return false;
    const float r0 = ravineLine(wx, wz);
    if (std::abs(r0) > 0.09f) return false;  // lejos de la linea (barato)
    width = (5.0f + 13.0f * std::clamp(noise2(wx / 160.0f, wz / 160.0f, 297) + 0.5f, 0.0f, 1.0f)) * strength;
    const float gx = (ravineLine(wx + 2.0f, wz) - ravineLine(wx - 2.0f, wz)) * 0.25f;
    const float gz = (ravineLine(wx, wz + 2.0f) - ravineLine(wx, wz - 2.0f)) * 0.25f;
    dist = std::abs(r0) / std::max(std::sqrt(gx * gx + gz * gz), 1e-5f);
    return dist < width + 3.0f;
}

bool Generator::caveOpenNear(int x, int z) const {
    const float fx = static_cast<float>(x) + 0.5f, fz = static_cast<float>(z) + 0.5f;
    const float deep = caveDeep(fx, fz);
    if (deep <= 0.0f) return false;
    float d = 0.0f, w = 0.0f, s = 0.0f;
    if (ravineAt(fx, fz, deep, d, w, s) && d < w + 2.5f) return true;
    std::vector<CaveShaft> shafts;
    caveShafts(x, z, x, z, shafts);
    for (const CaveShaft& sh : shafts) {
        if (std::hypot(fx - sh.x, fz - sh.z) < sh.r + 7.0f) return true;
    }
    return false;
}

bool Generator::caveOpen(const CaveColumn& c, int y) {
    if (y < c.open_lo || y > c.surface) return false;
    const float fy = static_cast<float>(y);
    if (c.rav && y > c.rav_floor) {
        const float span = std::max(1.0f, static_cast<float>(c.surface - c.rav_floor));
        const float t = std::clamp((fy - static_cast<float>(c.rav_floor)) / span, 0.0f, 1.0f);
        const float hw = c.rav_w * (0.28f + 0.72f * std::sqrt(t)) + 1.3f * std::sin(fy * 0.31f + c.phase) +
                         0.8f * std::sin(fy * 0.13f + c.phase * 1.7f);
        if (c.rav_d < hw) return true;
    }
    if (c.shaft && y > c.shaft_floor) {
        float r = c.shaft_r + 1.2f * std::sin(fy * 0.4f + c.phase) + 0.6f * std::sin(fy * 0.17f + c.phase * 2.3f);
        if (y > c.surface - 6) r += static_cast<float>(y - (c.surface - 6)) * 0.6f;  // boca en embudo
        if (c.shaft_d < r) return true;
    }
    return false;
}

// --- Lo de cada columna de un chunk -----------------------------------------------------

void Generator::caveColumns(int x0, int z0, const AdventureColumn* cols, int stride,
                            std::array<CaveColumn, kChunkSize * kChunkSize>& out) const {
    std::vector<CaveShaft> shafts;
    bool shafts_done = false;
    for (int lz = 0; lz < kChunkSize; ++lz) {
        for (int lx = 0; lx < kChunkSize; ++lx) {
            CaveColumn& cc = out[static_cast<std::size_t>(lx + lz * kChunkSize)];
            cc = CaveColumn{};
            const AdventureColumn& c = cols[(lx + 1) + (lz + 1) * stride];
            const int wx = x0 + lx, wz = z0 + lz;
            const float fx = static_cast<float>(wx) + 0.5f, fz = static_cast<float>(wz) + 0.5f;
            cc.surface = c.height;
            cc.deep = caveDeep(fx, fz);
            if (cc.deep <= 0.0f) continue;
            cc.dry = c.water <= c.height && c.lava <= c.height;
            // Lagos y lava.
            const float zone = liquidZone(fx, fz, cc.deep);
            const float az = std::abs(zone);
            const int level = zone > 0.0f ? kLakeLevel : kLavaLevel;
            int floor_min = 5;
            if (az >= kFill) {
                cc.liquid_top = level;
                cc.liquid = zone > 0.0f ? block::Water : block::Lava;
            } else if (az > kRingIn) {
                floor_min = level + 1;
            }
            // Roca honda.
            if (cc.deep > 0.5f) cc.deepstone = 16 + static_cast<int>(std::lround(noise2(fx / 40.0f, fz / 40.0f, 285) * 6.0f));
            // Bioma de la cueva.
            if (cc.deep > 0.3f) {
                const float b1 = noise2(fx / 150.0f, fz / 150.0f, 281), b2 = noise2(fx / 95.0f, fz / 95.0f, 283);
                if ((c.biome == AB::ASnow || (c.biome == AB::AMountains && c.height > sea_ + 70)) && b2 > -0.15f) cc.biome = kCaveFrozen;
                else if (b1 > 0.2f) cc.biome = kCaveMushroom;
                else if (b1 < -0.2f) cc.biome = kCaveCrystal;
                else if (b2 > 0.12f) cc.biome = kCaveDripstone;
            }
            // Barrancos y pozos: solo en tierra seca, lejos del agua, de la lava y del relieve suelto.
            const bool ok = cc.dry && !c.lake && c.river < 0.03f && c.feature == 0 && c.height > sea_ + 3 && c.height < kWorldHeight - 2;
            if (!ok) continue;
            cc.phase = noise2(fx / 24.0f, fz / 24.0f, 301) * 9.0f;
            float d = 0.0f, w = 0.0f, s = 0.0f;
            if (ravineAt(fx, fz, cc.deep, d, w, s) && !(structures_ && structures_->blocksTrees(wx, wz, 5.0f))) {
                cc.rav = true;
                cc.rav_d = d;
                cc.rav_w = w;
                const float bottom = 5.0f + 5.0f * std::clamp(noise2(fx / 70.0f, fz / 70.0f, 299) + 0.5f, 0.0f, 1.0f);
                const float f = lerp(static_cast<float>(c.height - 3), bottom, smooth01(0.0f, 0.55f, s));
                cc.rav_floor = std::max(floor_min, static_cast<int>(std::lround(f)));
                cc.open_lo = std::min(cc.open_lo, cc.rav_floor + 1);
            }
            if (!shafts_done) {
                caveShafts(x0, z0, x0 + kChunkSize - 1, z0 + kChunkSize - 1, shafts);
                shafts_done = true;
            }
            for (const CaveShaft& sh : shafts) {
                const float sd = std::hypot(fx - sh.x, fz - sh.z);
                if (sd > sh.r + 6.0f) continue;
                cc.shaft = true;
                cc.shaft_d = sd;
                cc.shaft_r = sh.r;
                cc.shaft_floor = std::max(floor_min, sh.bottom);
                cc.open_lo = std::min(cc.open_lo, cc.shaft_floor + 1);
                break;
            }
        }
    }
}


// --- Galerias de mina ---------------------------------------------------------------------
// Una mina como mucho por celda de kMineCell: una galeria principal recta (en x
// o en z) de 50-120 m y 2-4 ramales perpendiculares, todo a la misma altura
// (y 30-46, por encima de los lagos). 3 de ancho y 3 de alto, con puntales
// (postes de valla y viga de tablones) cada 4 bloques, antorchas, telaranas en
// las esquinas, alguna caja, y puentes de tablones donde cruza una sala.

struct Generator::MineSegment {
    bool along_x = true;
    int fixed = 0;  // z (along_x) o x: el centro de la galeria
    int a0 = 0, a1 = 0;
    int y = 32;  // primer bloque de aire (el suelo es y - 1)
};

void Generator::mineSegments(int x0, int z0, int x1, int z1, std::vector<MineSegment>& out) const {
    out.clear();
    if (profile_ != VoxelGenerator::Adventure) return;
    const auto s = static_cast<std::uint32_t>(seed_) + 4111u;
    for (int gz = floorDiv(z0 - kMineReach, kMineCell); gz <= floorDiv(z1 + kMineReach, kMineCell); ++gz) {
        for (int gx = floorDiv(x0 - kMineReach, kMineCell); gx <= floorDiv(x1 + kMineReach, kMineCell); ++gx) {
            if (rand01(gx, 1, gz, s) > 0.5f) continue;
            const int hx = gx * kMineCell + 30 + static_cast<int>(rand01(gx, 2, gz, s) * static_cast<float>(kMineCell - 60));
            const int hz = gz * kMineCell + 30 + static_cast<int>(rand01(gx, 3, gz, s) * static_cast<float>(kMineCell - 60));
            if (caveDeep(static_cast<float>(hx), static_cast<float>(hz)) < 0.35f) continue;
            const int y = 30 + static_cast<int>(rand01(gx, 4, gz, s) * 16.0f);
            const bool main_x = rand01(gx, 5, gz, s) < 0.5f;
            const int half = 25 + static_cast<int>(rand01(gx, 6, gz, s) * 35.0f);
            const auto add = [&](bool ax, int fixed, int a0, int a1) {
                const int bx0 = ax ? a0 : fixed - 1, bx1 = ax ? a1 : fixed + 1;
                const int bz0 = ax ? fixed - 1 : a0, bz1 = ax ? fixed + 1 : a1;
                if (bx1 < x0 || bx0 > x1 || bz1 < z0 || bz0 > z1) return;
                out.push_back({ax, fixed, a0, a1, y});
            };
            const int c_main = main_x ? hz : hx, c_along = main_x ? hx : hz;
            add(main_x, c_main, c_along - half, c_along + half);
            const int branches = 2 + static_cast<int>(rand01(gx, 7, gz, s) * 3.0f);
            for (int i = 0; i < branches; ++i) {
                const int at = c_along - half + 6 + static_cast<int>(rand01(gx, 10 + i, gz, s) * static_cast<float>(2 * half - 12));
                const int len = 14 + static_cast<int>(rand01(gx, 20 + i, gz, s) * 36.0f);
                const float side = rand01(gx, 30 + i, gz, s);
                const int lo = side < 0.65f ? c_main - len : c_main;
                const int hi = side > 0.35f ? c_main + len : c_main;
                add(!main_x, at, lo, hi);
            }
        }
    }
}

// --- Decoracion de las cuevas inmensas (el chunk ya excavado) ----------------------------

void Generator::decorateCaves(ChunkData& chunk, const std::array<CaveColumn, kChunkSize * kChunkSize>& caves) const {
    const int x0 = chunk.cx * kChunkSize, z0 = chunk.cz * kChunkSize;
    bool any = false;
    for (const CaveColumn& c : caves) any = any || c.deep > 0.0f;
    if (!any) return;
    const auto s = static_cast<std::uint32_t>(seed_) + 5501u;
    const auto at = [&](int x, int y, int z) -> BlockId& { return chunk.blocks[static_cast<std::size_t>(cellIndex(x, y, z))]; };
    const auto rock = [](BlockId id) { return id != block::Bedrock && blockDef(id).solid && blockDef(id).opaque; };
    static constexpr BlockId kCrystals[] = {block::CrystalBlue, block::CrystalViolet, block::CrystalPink, block::CrystalGreen};

    for (int lz = 0; lz < kChunkSize; ++lz) {
        for (int lx = 0; lx < kChunkSize; ++lx) {
            const CaveColumn& cc = caves[static_cast<std::size_t>(lx + lz * kChunkSize)];
            if (cc.deep <= 0.0f) continue;
            const int wx = x0 + lx, wz = z0 + lz;
            const int top = std::min(cc.surface - 1, kWorldHeight - 2);
            const BlockId crystal = kCrystals[static_cast<int>(std::clamp(noise2(static_cast<float>(wx) / 28.0f, static_cast<float>(wz) / 28.0f, 303) + 0.5f, 0.0f, 0.999f) * 4.0f)];
            for (int y = 5; y <= top; ++y) {
                BlockId& b = at(lx, y, lz);
                if (!rock(b)) continue;
                const BlockId above = at(lx, y + 1, lz), below = at(lx, y - 1, lz);
                const bool floor = above == block::Air, ceil = below == block::Air;
                const float r = rand01(wx, y, wz, s), r2 = rand01(wx, y, wz, s + 1u);
                if (block::isWater(above)) {  // fondo de un lago: arcilla y grava
                    if (r < 0.4f) b = block::Clay;
                    else if (r < 0.7f) b = block::Gravel;
                    continue;
                }
                if (above == block::Lava || below == block::Lava) {  // orilla de la lava
                    b = r < 0.3f ? block::Magma : (r < 0.42f ? block::Obsidian : block::Basalt);
                    continue;
                }
                if (floor) {
                    switch (cc.biome) {
                        case kCaveMushroom:
                            b = r < 0.6f ? block::GlowMoss : block::Moss;
                            if (r2 < 0.22f) at(lx, y + 1, lz) = block::GlowGrass;
                            else if (r2 < 0.28f) at(lx, y + 1, lz) = block::GlowMushroom;
                            else if (r2 < 0.31f) at(lx, y + 1, lz) = block::BrownMushroom;
                            break;
                        case kCaveCrystal:
                            if (r < 0.35f) b = block::DuskStone;
                            if (r2 < 0.09f) at(lx, y + 1, lz) = block::CrystalSprout;
                            else if (r2 < 0.11f && at(lx, y + 2, lz) == block::Air) at(lx, y + 1, lz) = crystal;
                            break;
                        case kCaveDripstone:
                            if (r < 0.6f) b = block::Dripstone;
                            if (r2 > 0.988f) {
                                // Columna de suelo a techo si el techo esta cerca.
                                int ceil_y = -1;
                                for (int k = y + 1; k <= std::min(y + 16, kWorldHeight - 1); ++k) {
                                    if (at(lx, k, lz) != block::Air) {
                                        ceil_y = rock(at(lx, k, lz)) ? k : -1;
                                        break;
                                    }
                                }
                                if (ceil_y > y + 2) {
                                    for (int k = y + 1; k < ceil_y; ++k) at(lx, k, lz) = block::Dripstone;
                                }
                            } else if (r2 < 0.1f && at(lx, y + 2, lz) == block::Air) {
                                at(lx, y + 1, lz) = block::Stalagmite;
                            }
                            break;
                        case kCaveFrozen:
                            if (r < 0.45f) b = block::Ice;
                            else if (r < 0.6f) b = block::Snow;
                            break;
                        default:
                            if (y < cc.deepstone - 3 && r < 0.25f) b = block::Basalt;
                            else if (y < cc.deepstone - 3 && r < 0.27f) b = block::Magma;
                            else if (r2 < 0.015f && at(lx, y + 2, lz) == block::Air) at(lx, y + 1, lz) = block::Stalagmite;
                            break;
                    }
                }
                if (ceil) {
                    const float r3 = rand01(wx, y, wz, s + 2u);
                    const auto hang = [&](BlockId id, int len, BlockId tip) {
                        int k = y - 1;
                        for (int i = 0; i < len && k > 5 && at(lx, k, lz) == block::Air; ++i, --k) at(lx, k, lz) = id;
                        if (tip != block::Air && k > 5 && at(lx, k, lz) == block::Air && at(lx, k + 1, lz) != block::Air) at(lx, k, lz) = tip;
                    };
                    switch (cc.biome) {
                        case kCaveMushroom:
                            if (r3 < 0.07f) hang(block::GlowVines, 1 + static_cast<int>(r * 5.0f), block::Air);
                            break;
                        case kCaveCrystal:
                            if (r3 < 0.05f) hang(crystal, 1 + static_cast<int>(r * 3.0f), block::Air);
                            break;
                        case kCaveDripstone:
                            if (r < 0.5f) b = block::Dripstone;
                            if (r3 < 0.13f) hang(block::Stalactite, 1, block::Air);
                            else if (r3 < 0.17f) hang(block::Dripstone, 1 + static_cast<int>(r2 * 2.0f), block::Stalactite);
                            break;
                        case kCaveFrozen:
                            if (r3 < 0.06f) hang(block::Ice, 1 + static_cast<int>(r * 2.0f), block::Air);
                            break;
                        default:
                            if (r3 < 0.03f) hang(block::Stalactite, 1, block::Air);
                            else if (r3 < 0.04f && cc.biome == kCaveNormal) hang(block::GlowVines, 1 + static_cast<int>(r * 3.0f), block::Air);
                            break;
                    }
                }
                if (!floor && !ceil && cc.biome == kCaveCrystal && lx > 0 && lz > 0 && lx < kChunkSize - 1 && lz < kChunkSize - 1) {
                    // Paredes con cristales y menas de cristal.
                    const bool wall = at(lx + 1, y, lz) == block::Air || at(lx - 1, y, lz) == block::Air ||
                                      at(lx, y, lz + 1) == block::Air || at(lx, y, lz - 1) == block::Air;
                    if (wall) {
                        if (r < 0.06f) b = crystal;
                        else if (r < 0.09f) b = block::CrystalOre;
                    }
                }
            }
        }
    }

    // Setas gigantes en las grutas de setas (enteras dentro del chunk).
    for (int cz = 0; cz < 2; ++cz) {
        for (int cx = 0; cx < 2; ++cx) {
            const int gx = chunk.cx * 2 + cx, gz = chunk.cz * 2 + cz;
            if (rand01(gx, 40, gz, s) > 0.55f) continue;
            const int lx = cx * 8 + 3 + static_cast<int>(rand01(gx, 41, gz, s) * 2.0f);
            const int lz = cz * 8 + 3 + static_cast<int>(rand01(gx, 42, gz, s) * 2.0f);
            const CaveColumn& cc = caves[static_cast<std::size_t>(lx + lz * kChunkSize)];
            if (cc.biome != kCaveMushroom) continue;
            const int stem = 4 + static_cast<int>(rand01(gx, 43, gz, s) * 5.0f);
            const int rad = rand01(gx, 44, gz, s) < 0.5f ? 2 : 3;
            const BlockId cap = rand01(gx, 45, gz, s) < 0.5f ? block::ShroomCapBlue : block::ShroomCapViolet;
            for (int y = 6; y + stem + 3 < std::min(cc.surface - 4, kWorldHeight - 1); ++y) {
                const BlockId f = at(lx, y, lz);
                if (!(f == block::GlowMoss || f == block::Moss || rock(f))) continue;
                bool room = true;
                for (int k = 1; k <= stem + 2 && room; ++k) room = at(lx, y + k, lz) == block::Air || !blockDef(at(lx, y + k, lz)).solid;
                if (!room) continue;
                for (int k = 1; k <= stem; ++k) at(lx, y + k, lz) = block::ShroomStem;
                const int cy = y + stem + 1;
                for (int dz = -rad; dz <= rad; ++dz) {
                    for (int dx = -rad; dx <= rad; ++dx) {
                        const int d2 = dx * dx + dz * dz;
                        if (d2 > rad * rad + 1) continue;
                        BlockId& top_cell = at(lx + dx, cy, lz + dz);
                        if (top_cell == block::Air) top_cell = cap;
                        if (d2 >= (rad - 1) * (rad - 1) + 1) {  // borde que cae
                            BlockId& rim = at(lx + dx, cy - 1, lz + dz);
                            if (rim == block::Air) rim = cap;
                        }
                    }
                }
                break;
            }
        }
    }

    // Galerias de mina.
    std::vector<MineSegment> segs;
    mineSegments(x0, z0, x0 + kChunkSize - 1, z0 + kChunkSize - 1, segs);
    if (segs.empty()) return;
    const auto inside = [&](int wx, int wz, std::size_t skip) {
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i == skip) continue;
            const MineSegment& g = segs[i];
            const int a = g.along_x ? wx : wz, o = g.along_x ? wz : wx;
            if (a >= g.a0 - 1 && a <= g.a1 + 1 && std::abs(o - g.fixed) <= 1) return true;
        }
        return false;
    };
    const auto plant_like = [](BlockId id) { return id != block::Air && !blockDef(id).solid && !block::isWater(id) && id != block::Lava; };
    for (std::size_t si = 0; si < segs.size(); ++si) {
        const MineSegment& g = segs[si];
        const int lo = std::max(g.a0, g.along_x ? x0 : z0), hi = std::min(g.a1, (g.along_x ? x0 : z0) + kChunkSize - 1);
        for (int a = lo; a <= hi; ++a) {
            const bool support = ((a - g.a0) % 4) == 2 && a - g.a0 >= 2 && g.a1 - a >= 2;
            const bool torch = !support && ((a - g.a0) % 12) == 4;
            for (int o = -1; o <= 1; ++o) {
                const int wx = g.along_x ? a : g.fixed + o, wz = g.along_x ? g.fixed + o : a;
                const int lx = wx - x0, lz = wz - z0;
                if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize) continue;
                const CaveColumn& cc = caves[static_cast<std::size_t>(lx + lz * kChunkSize)];
                if (cc.deep <= 0.0f || !cc.dry || g.y + 3 >= cc.surface - 4 || g.y <= cc.liquid_top + 1) continue;
                for (int y = g.y; y <= g.y + 2; ++y) {
                    if (at(lx, y, lz) != block::Bedrock) at(lx, y, lz) = block::Air;
                }
                if (plant_like(at(lx, g.y + 3, lz)) && at(lx, g.y + 3, lz) != block::Stalactite) at(lx, g.y + 3, lz) = block::Air;
                BlockId& floor = at(lx, g.y - 1, lz);
                if (!blockDef(floor).solid) floor = block::OakPlanks;  // puente sobre las salas
                const float r = rand01(wx, g.y, wz, s + 7u);
                const bool side_free = o != 0 && !inside(wx, wz, si);
                if (support) {
                    if (side_free) {
                        at(lx, g.y, lz) = block::Fence;
                        at(lx, g.y + 1, lz) = block::Fence;
                    }
                    at(lx, g.y + 2, lz) = block::OakPlanks;
                } else if (torch && side_free && o == ((((a - g.a0) / 12) % 2) == 0 ? 1 : -1)) {
                    at(lx, g.y, lz) = kMineLight;
                } else if (side_free && r < 0.06f) {
                    at(lx, g.y + 2, lz) = block::Cobweb;
                } else if (side_free && r > 0.992f) {
                    at(lx, g.y, lz) = r > 0.996f ? block::Barrel : block::Crate;
                }
                // Via vieja por el centro (rota a trozos). En los extremos, a veces un tope con un barril al
                // lado: ahi el juego (Rieles.cpp) deja una vagoneta abandonada con botin.
                const bool end_dock = (a == g.a0 || a == g.a1) && rand01(g.fixed, g.y, a, s + 17u) < 0.6f;
                if (o == 0 && !inside(wx, wz, si)) {
                    if (end_dock) at(lx, g.y, lz) = block::BufferStop;
                    else if (rand01((a - g.a0) / 5, g.fixed, g.y, s + 13u) > 0.18f) at(lx, g.y, lz) = block::Rail;
                } else if (o == 0) {
                    at(lx, g.y, lz) = block::Rail;  // cruce con otra galeria
                } else if (o == 1 && end_dock && !inside(wx, wz, si)) {
                    at(lx, g.y, lz) = block::Barrel;
                }
            }
        }
    }
}

}  // namespace cramion::voxel
