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
    return std::max(tunnel, cavern);
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
            if (floor_y < kMinFloor) break;
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

}  // namespace cramion::voxel
