// Mallador de secciones (16^3): una cara por cada lado de bloque que se ve.
//
//   - Caras en sentido antihorario vistas desde fuera (el VoxelPass quita
//     las de atras). Esquinas: abajo-izquierda, abajo-derecha, arriba-
//     derecha, arriba-izquierda con los ejes u (derecha) y v (abajo) de
//     kFaces, los mismos que usa voxel.frag para el normal map.
//   - Oclusion por esquina (los tres vecinos de delante de la esquina), y la
//     diagonal del cuadrado se gira si hace falta para que no se vea el corte.
//   - Luz suave: por esquina, la media de las celdas de aire que la tocan.
//   - Plantas y antorchas: dos cuadrados en cruz, por las dos caras.
//   - Vallas y faroles (BlockDef::model): cajas mas pequenas que la celda,
//     en dieciseisavos (voxel.vert: bit 30 del segundo uint = vertice fino,
//     con el desplazamiento de 0..15/16 por eje en los bits 23-26 (x), 27-30
//     (y) y 18-21 (z) del primero, en lugar de uv y oclusion). Sus uv salen
//     de la posicion (voxel.frag), asi la textura de 16 x 16 se lee en el
//     trozo que ocupa cada pieza (el poste en el centro, los travesanos...).
//   - Puertas de 2 de alto: la mitad de arriba usa BlockDef::side_upper.
//   - Caras fusionadas (greedy meshing): las caras de bloques opacos con la
//     misma textura, tinte, luz y oclusion en las cuatro esquinas se juntan
//     en rectangulos de hasta 16 x 16 (bit 31 del segundo uint = uv desde la
//     posicion, como los vertices finos). En terreno abierto quita la mayor
//     parte de los cuadrados: en un movil la geometria era el pase mas caro.

#include "VoxelInternal.h"
#include "CramionCore/cvar/CVar.h"
#include "Rails.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

namespace cramion::voxel {

namespace {
// Luz suave (como la de Minecraft): oclusion y luz por esquina. Apagada, cada
// cara lleva la luz de la celda de delante y sin sombra en las esquinas: se ve
// mas plano, pero casi todas las caras se fusionan (en un terreno real, con la
// luz suave, solo un ~15 %). El movil en Baja la apaga (PlayerMain). Se lee al
// mallar: vale para las secciones que se mallan despues.
cvar::CVar<bool> g_fluffy_leaves("voxel.mesh.FluffyLeaves", true,
                                  "Hojas esponjosas: lamina de hojas recortadas por fuera de las caras al aire (rompe el cubo)");
// MENAS-HZ: pepitas de mineral en relieve (en una zona de cuevas con muchas menas sube bastante la geometria: el
// movil en Baja puede apagarlo como las hojas esponjosas).
cvar::CVar<bool> g_ore_relief("voxel.mesh.OreRelief", true,
                              "Menas en relieve: pepitas de mineral que sobresalen de las caras al aire (solo dibujo)");
cvar::CVar<bool> g_smooth_light("voxel.mesh.SmoothLight", true,
                                "Luz suave y sombra en las esquinas de los bloques (apagada: una luz por cara, muchos "
                                "menos triangulos; para moviles flojos)",
                                cvar::None);
// MALLAS-DETALLE: menos geometria de adorno lejos (moviles flojos). Ver meshTierFor (VoxelInternal.h).
cvar::CVar<int> g_mesh_detail("voxel.mesh.Detail", 2,
                              "Detalle de las mallas de bloques: 0 Bajo (adornos sencillos desde 16 m y sin adornos ni "
                              "oclusion de esquina desde 40 m), 1 Medio (desde 48 / 96 m), 2 Alto (todo, como siempre). "
                              "Solo dibujo: los bloques y la colision no cambian",
                              cvar::None, 0, 2);
}  // namespace

int meshDetailLevel() { return std::clamp(g_mesh_detail.get(), 0, 2); }

int meshTierFor(int level, float distance) {
    if (level >= 2) return 2;
    const float full = level == 1 ? 48.0f : 16.0f, light = level == 1 ? 96.0f : 40.0f;
    return distance < full ? 2 : distance < light ? 1 : 0;
}

// n, u (derecha), v (abajo en la textura). Ver voxel_common.glsl.
const std::array<FaceAxes, 6> kFaces = {{
    {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},   // +X
    {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},   // -X
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},     // +Y
    {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},   // -Y
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},    // +Z
    {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},  // -Z
}};

namespace {

std::uint32_t packA(int x, int y, int z, int face, int u, int v, int ao, bool waves) {
    return static_cast<std::uint32_t>(x) | (static_cast<std::uint32_t>(y) << 5) | (static_cast<std::uint32_t>(z) << 10) |
           (static_cast<std::uint32_t>(face) << 15) | (static_cast<std::uint32_t>(u) << 18) |
           (static_cast<std::uint32_t>(v) << 19) | (static_cast<std::uint32_t>(ao) << 20) | (waves ? (1u << 22) : 0u);
}

std::uint32_t packB(int layer, int sky, int torch, std::uint16_t tint) {
    return static_cast<std::uint32_t>(layer & 1023) | (static_cast<std::uint32_t>(sky & 15) << 10) |
           (static_cast<std::uint32_t>(torch & 15) << 14) | (static_cast<std::uint32_t>(tint & 0xFFF) << 18);
}

constexpr std::uint16_t kWhite = 0xFFF;

// Vertice fino (vallas, faroles): posicion entera + desplazamiento en dieciseisavos.
std::uint32_t packFine(int x, int y, int z, int face, int fx, int fy, int fz) {
    return static_cast<std::uint32_t>(x) | (static_cast<std::uint32_t>(y) << 5) | (static_cast<std::uint32_t>(z) << 10) |
           (static_cast<std::uint32_t>(face) << 15) | (static_cast<std::uint32_t>(fz & 15) << 18) |
           (static_cast<std::uint32_t>(fx & 15) << 23) | (static_cast<std::uint32_t>(fy & 15) << 27);
}

constexpr std::uint32_t kFineBit = 1u << 30;  // en el segundo uint (packB usa los bits 0..29)
constexpr std::uint32_t kPlanarBit = 1u << 31;  // cara fusionada: uv desde la posicion (voxel.vert/.frag)
// HOJAS: en un vertice FINO el bit 31 (planar no se usa ahi) = la posicion entera va +1 (se pueden poner piezas que
// sobresalen de la seccion por abajo / la izquierda: voxel.vert le resta 1).
constexpr std::uint32_t kFineBiasBit = 1u << 31;

// HOJAS: lamina de hojas recortadas delante de la cara f de la celda (x, y, z): `e` dieciseisavos por fuera y otros
// tantos mas ancha por cada lado. Con su textura _fluff, tinte, luz de la celda de delante y viento.
// COPAS-HZ: `es` el ancho de mas por cada lado (antes = e) y (du, dv) un desplazamiento a lo largo de la cara.
void emitFluff(std::vector<std::uint32_t>& out, int x, int y, int z, int f, int layer, int sky, int torch, std::uint16_t tint,
               bool waves, int e, int es, int du, int dv) {
    const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
    const int cell[3] = {x, y, z};
    const std::uint32_t b = (static_cast<std::uint32_t>(layer & 1023) | (static_cast<std::uint32_t>(sky & 15) << 10) |
                             (static_cast<std::uint32_t>(torch & 15) << 14) | (static_cast<std::uint32_t>(tint & 0xFFF) << 18)) |
                            kFineBit | kFineBiasBit;
    static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int c = 0; c < 4; ++c) {
        const int su = kCorner[c][0], sv = kCorner[c][1];
        int whole[3], fine[3];
        for (int k = 0; k < 3; ++k) {
            int p;
            if (axes.n[k] != 0) p = axes.n[k] > 0 ? 16 + e : -e;
            else if (axes.u[k] != 0) p = (axes.u[k] * su > 0 ? 16 + es : -es) + du;
            else p = (axes.v[k] * sv > 0 ? 16 + es : -es) + dv;
            const int q = p + 16;  // siempre >= 0: la parte entera lleva el +1 del bit de desplazamiento
            whole[k] = cell[k] + q / 16;
            fine[k] = q % 16;
        }
        out.push_back(packFine(whole[0], whole[1], whole[2], f, fine[0], fine[1], fine[2]) | (waves ? (1u << 22) : 0u));
        out.push_back(b);
    }
}

// Caja [lo, hi] (dieciseisavos, 0..16) dentro de la celda (x, y, z). skip[f]:
// no poner esa cara (tapada por un vecino o por otra pieza).
// `layers` (arriba, abajo, lados): capas de textura en lugar de las del bloque
// (las piezas con la segunda textura de las luces); nullptr = las del bloque.
void emitBox(std::vector<std::uint32_t>& out, int x, int y, int z, const int lo[3], const int hi[3], const bool skip[6],
             const BlockDef& def, int sky, int torch, const int* layers = nullptr) {
    const int cell[3] = {x, y, z};
    for (int f = 0; f < 6; ++f) {
        if (skip[f]) continue;
        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
        const std::string& texture = f == 2 ? def.top : (f == 3 ? def.bottom : def.side);
        const int layer = layers != nullptr ? layers[f == 2 ? 0 : (f == 3 ? 1 : 2)] : textureLayer(texture);
        const std::uint32_t b = packB(layer, sky, torch, kWhite) | kFineBit;
        static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
        for (int c = 0; c < 4; ++c) {
            const int su = kCorner[c][0], sv = kCorner[c][1];
            int whole[3], fine[3];
            for (int k = 0; k < 3; ++k) {
                bool high;
                if (axes.n[k] != 0) high = axes.n[k] > 0;
                else if (axes.u[k] != 0) high = axes.u[k] * su > 0;
                else high = axes.v[k] * sv > 0;
                const int p = high ? hi[k] : lo[k];
                whole[k] = cell[k] + p / 16;
                fine[k] = p % 16;
            }
            out.push_back(packFine(whole[0], whole[1], whole[2], f, fine[0], fine[1], fine[2]));
            out.push_back(b);
        }
    }
}

// Hash de una celda del mundo (solo para el dibujo: no cambia bloques).
std::uint32_t cellHash(int x, int y, int z, std::uint32_t salt) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u ^ salt * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

// Caja [lo, hi] en dieciseisavos relativos a la celda (x, y, z), de -16 a 31 (puede salir de la celda: bit de
// desplazamiento de HOJAS), toda con la capa `layer`. skip[f]: sin esa cara.
void emitBoxBiased(std::vector<std::uint32_t>& out, int x, int y, int z, const int lo[3], const int hi[3], const bool skip[6],
                   int layer, int sky, int torch, std::uint16_t tint, bool waves = false) {
    const int cell[3] = {x, y, z};
    const std::uint32_t b = packB(layer, sky, torch, tint) | kFineBit | kFineBiasBit;
    static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int f = 0; f < 6; ++f) {
        if (skip != nullptr && skip[f]) continue;
        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
        for (int c = 0; c < 4; ++c) {
            const int su = kCorner[c][0], sv = kCorner[c][1];
            int whole[3], fine[3];
            for (int k = 0; k < 3; ++k) {
                bool high;
                if (axes.n[k] != 0) high = axes.n[k] > 0;
                else if (axes.u[k] != 0) high = axes.u[k] * su > 0;
                else high = axes.v[k] * sv > 0;
                const int q = (high ? hi[k] : lo[k]) + 16;
                whole[k] = cell[k] + q / 16;
                fine[k] = q % 16;
            }
            out.push_back(packFine(whole[0], whole[1], whole[2], f, fine[0], fine[1], fine[2]) | (waves ? (1u << 22) : 0u));
            out.push_back(b);
        }
    }
}

// Lo que cada bloque necesita del dibujo nuevo, calculado una vez por id (sin buscar cadenas al mallar).
struct ShapeInfo {
    bool tuft = false;  // PASTO-HZ
    int gem = -1;       // MENAS-HZ: capa <mena>_gem (o -1)
    int bark = -1;      // RAMAS-HZ: tronco (*_log): capa de su corteza
    int leaf_bark = -1; // RAMAS-HZ: hojas "<arbol>_leaves": la corteza de "<arbol>_log" (si existe)
};
const std::array<ShapeInfo, block::Count>& shapeInfo() {
    static const std::array<ShapeInfo, block::Count> table = [] {
        std::array<ShapeInfo, block::Count> t{};
        const auto& list = blocks();
        for (std::size_t i = 0; i < list.size() && i < t.size(); ++i) {
            const BlockDef& d = list[i];
            // PASTO-HZ: plantas que se dibujan como mata (laminas giradas) en lugar de la X: las hierbas, helechos y
            // juncos (las apiladas de PLANTAS-ALTAS y las de tinte de hierba). Flores, setas, lianas... siguen en X.
            t[i].tuft = d.shape == BlockShape::Cross && d.waves && !isHangPlant(d) &&
                        (isStackPlant(d) || d.tint == BlockTint::Grass || d.side.find("grass") != std::string::npos);
            if (isOreBlock(d)) t[i].gem = textureLayer(d.side + "_gem");
            const auto ends = [&](const char* suffix) {
                const std::size_t k = std::char_traits<char>::length(suffix);
                return d.name.size() > k && d.name.compare(d.name.size() - k, k, suffix) == 0;
            };
            if (ends("_log") && d.shape == BlockShape::Cube && d.model == BlockModel::Full) t[i].bark = textureLayer(d.side);
        }
        for (std::size_t i = 0; i < list.size() && i < t.size(); ++i) {
            const std::string& n = list[i].name;
            if (n.size() <= 7 || n.compare(n.size() - 7, 7, "_leaves") != 0) continue;
            const BlockId log = blockId(n.substr(0, n.size() - 7) + "_log");
            if (log != block::Air && t[log].bark >= 0) t[i].leaf_bark = t[log].bark;
        }
        return t;
    }();
    return table;
}

// MENAS-HZ: pepitas de mineral que sobresalen 1-3/16 de una cara al aire de una mena (solo dibujo: no chocan).
// 2-3 cajas por cara, colocadas por un hash de la celda y la cara, dentro de la huella de la cara (no tapan a las
// vecinas), con la textura <mena>_gem (el mineral de la textura de la mena: vale con los paquetes de texturas).
void emitNuggets(std::vector<std::uint32_t>& out, int x, int y, int z, int f, std::uint32_t h, int layer, int sky, int torch,
                 bool simple = false) {
    const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
    int kn = 0, ku = 0, kv = 0;
    for (int k = 0; k < 3; ++k) {
        if (axes.n[k] != 0) kn = k;
        if (axes.u[k] != 0) ku = k;
        if (axes.v[k] != 0) kv = k;
    }
    const bool positive = axes.n[kn] > 0;
    bool skip[6] = {false, false, false, false, false, false};
    skip[f ^ 1] = true;  // la que toca la cara de la mena
    // Las de 1/16 solo con la cara de delante (los lados de 1/16 casi no se ven): 1 cuadrado en lugar de 5.
    bool flat[6] = {true, true, true, true, true, true};
    flat[f] = false;
    // 2 (a veces 3): unos 8 cuadrados por cara. MALLAS-DETALLE: sencillas (lejos), una sola y sin punta.
    const int count = simple ? 1 : 2 + ((h & 3u) == 0u ? 1 : 0);
    for (int i = 0; i < count; ++i) {
        const std::uint32_t r = cellHash(static_cast<int>(h), f, i, 0x6E67u);
        const int wu = 2 + static_cast<int>(r % 3u), wv = 2 + static_cast<int>((r >> 2) % 3u);  // 2..4
        const int depth = 1 + static_cast<int>((r >> 4) % 3u);                                    // 1..3
        const int u0 = 2 + static_cast<int>((r >> 6) % static_cast<std::uint32_t>(13 - wu));      // dentro de [2, 14]
        const int v0 = 2 + static_cast<int>((r >> 10) % static_cast<std::uint32_t>(13 - wv));
        int lo[3], hi[3];
        lo[ku] = u0;
        hi[ku] = u0 + wu;
        lo[kv] = v0;
        hi[kv] = v0 + wv;
        lo[kn] = positive ? 16 : -depth;
        hi[kn] = positive ? 16 + depth : 0;
        emitBoxBiased(out, x, y, z, lo, hi, depth == 1 ? flat : skip, layer, sky, torch, kWhite);
        // A veces una punta mas pequena encima (un cristal o una pepita escalonada).
        if (!simple && depth >= 2 && wu >= 3 && wv >= 3 && ((r >> 14) & 3u) == 0u) {
            lo[ku] += 1;
            hi[ku] -= 1;
            lo[kv] += 1;
            hi[kv] -= 1;
            lo[kn] = positive ? 16 + depth : -depth - 1;
            hi[kn] = positive ? 16 + depth + 1 : -depth;
            emitBoxBiased(out, x, y, z, lo, hi, skip, layer, sky, torch, kWhite);
        }
    }
}

bool isTuftPlant(BlockId id) { return shapeInfo()[id].tuft; }

// RAMAS-HZ: la rama (block::Branch) se une a los troncos (*_log) y ramas de las 26 celdas de alrededor: palos de
// t/16 de grueso desde el centro hasta el borde compartido, rectos hacia las caras y escalonados (cubitos) en
// diagonal (una diagonal solo si no hay ya un camino por una vecina de cara o de arista). Grosor: 8 si sigue a un
// tronco por debajo (la punta del tronco se afina), 6 si sale de un tronco, 4 el resto. Sobre suelo macizo y sin
// madera encima es una RAIZ: tendida en el suelo, con un contrafuerte que sube por el tronco. La corteza es la del
// tronco al que llega (siguiendo las ramas dentro de la seccion), o la del arbol de las hojas de al lado, o roble.
template <class BlockAt>
void meshBranch(std::vector<std::uint32_t>& out, const BlockAt& block_at, int x, int y, int z, int sky, int torch) {
    const auto& info = shapeInfo();
    const auto woody = [&](BlockId b) { return b == block::Branch || info[b].bark >= 0; };
    const auto at = [&](int dx, int dy, int dz) { return block_at(x + dx, y + dy, z + dz); };
    struct Conn {
        int d[3];
        bool log;
    };
    std::array<Conn, 26> conns{};
    int n = 0;
    const BlockId below = at(0, -1, 0);
    const BlockDef& below_def = blockDef(below);
    const bool root = below_def.solid && below_def.opaque && !woody(below) && !woody(at(0, 1, 0));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                if (root && dy != 0) continue;  // las raices van a ras de suelo
                const BlockId b = at(dx, dy, dz);
                if (!woody(b)) continue;
                const int d[3] = {dx, dy, dz};
                bool via = false;  // ya hay un camino por una celda intermedia
                for (int mask = 1; mask < 7 && !via; ++mask) {
                    int e[3];
                    bool zero = true, same = true;
                    for (int k = 0; k < 3; ++k) {
                        e[k] = (mask >> k) & 1 ? d[k] : 0;
                        zero = zero && e[k] == 0;
                        same = same && e[k] == d[k];
                    }
                    if (zero || same) continue;
                    if (root && e[1] != 0) continue;
                    via = woody(at(e[0], e[1], e[2]));
                }
                if (via) continue;
                conns[static_cast<std::size_t>(n++)] = Conn{{dx, dy, dz}, b != block::Branch};
            }
        }
    }
    bool any_log = false;
    for (int i = 0; i < n; ++i) any_log = any_log || conns[static_cast<std::size_t>(i)].log;
    const bool log_below = info[below].bark >= 0;
    const int t = root ? (any_log ? 6 : 4) : (log_below ? 8 : (any_log ? 6 : 4));
    const int h = t / 2;
    const int c[3] = {8, root ? h : 8, 8};

    // Corteza: el tronco vecino; si no, siguiendo ramas dentro de la seccion; si no, por las hojas; si no, roble.
    int layer = -1;
    for (int dy = -1; dy <= 1 && layer < 0; ++dy)
        for (int dz = -1; dz <= 1 && layer < 0; ++dz)
            for (int dx = -1; dx <= 1 && layer < 0; ++dx) layer = info[at(dx, dy, dz)].bark;
    if (layer < 0) {
        std::array<std::array<int, 3>, 64> queue{};
        int head = 0, tail = 0;
        queue[static_cast<std::size_t>(tail++)] = {x, y, z};
        for (int depth = 0; depth < 5 && layer < 0 && head < tail; ++depth) {
            const int end = tail;
            for (; head < end && layer < 0; ++head) {
                const auto p = queue[static_cast<std::size_t>(head)];
                for (int dy = -1; dy <= 1 && layer < 0; ++dy)
                    for (int dz = -1; dz <= 1 && layer < 0; ++dz)
                        for (int dx = -1; dx <= 1 && layer < 0; ++dx) {
                            const int qx = p[0] + dx, qy = p[1] + dy, qz = p[2] + dz;
                            if (qx < -1 || qy < -1 || qz < -1 || qx > kChunkSize || qy > kChunkSize || qz > kChunkSize) continue;
                            const BlockId b = block_at(qx, qy, qz);
                            if (info[b].bark >= 0) {
                                layer = info[b].bark;
                            } else if (b == block::Branch && tail < static_cast<int>(queue.size()) &&
                                       std::max({std::abs(qx - x), std::abs(qy - y), std::abs(qz - z)}) == depth + 1) {
                                queue[static_cast<std::size_t>(tail++)] = {qx, qy, qz};
                            }
                        }
            }
        }
    }
    for (int dy = -1; dy <= 1 && layer < 0; ++dy)
        for (int dz = -1; dz <= 1 && layer < 0; ++dz)
            for (int dx = -1; dx <= 1 && layer < 0; ++dx) layer = info[at(dx, dy, dz)].leaf_bark;
    if (layer < 0) layer = textureLayer("oak_log");

    // Cajas sin las caras que no se ven: la de abajo de las raices (contra el suelo) y las del borde de la celda
    // hacia lo que esta unido (la otra rama o el tronco tapan esa cara).
    const auto box = [&](int x0, int y0, int z0, int x1, int y1, int z1, const bool* skip) {
        const int lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
        bool s[6] = {false, false, false, false, false, false};
        if (skip != nullptr)
            for (int f = 0; f < 6; ++f) s[f] = skip[f];
        if (root && y0 == 0) s[3] = true;
        emitBoxBiased(out, x, y, z, lo, hi, s, layer, sky, torch, kWhite);
    };
    if (n == 0) {
        if (root) box(3, 0, 8 - h, 13, t, 8 + h, nullptr);  // palo tendido
        else box(8 - h, 0, 8 - h, 8 + h, 16, 8 + h, nullptr);
        return;
    }
    // Hacia las caras: una caja por eje (de lado a lado si se une por los dos), que ya cubre el centro.
    bool face_conn[6] = {false, false, false, false, false, false};  // kFaces: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
    bool log_conn[6] = {false, false, false, false, false, false};
    for (int i = 0; i < n; ++i) {
        const Conn& k = conns[static_cast<std::size_t>(i)];
        if (std::abs(k.d[0]) + std::abs(k.d[1]) + std::abs(k.d[2]) != 1) continue;
        const int f = k.d[0] > 0 ? 0 : k.d[0] < 0 ? 1 : k.d[1] > 0 ? 2 : k.d[1] < 0 ? 3 : k.d[2] > 0 ? 4 : 5;
        face_conn[f] = true;
        log_conn[f] = k.log;
    }
    bool any_face = false;
    for (int a = 0; a < 3; ++a) {
        const bool pos = face_conn[a * 2], neg = face_conn[a * 2 + 1];
        if (!pos && !neg) continue;
        any_face = true;
        int lo[3] = {c[0] - h, c[1] - h, c[2] - h}, hi[3] = {c[0] + h, c[1] + h, c[2] + h};
        if (pos) hi[a] = 16;
        if (neg) lo[a] = 0;
        bool skip[6] = {false, false, false, false, false, false};
        // Se quita la cara del borde si lo de al lado la tapa entera: un tronco, o una rama (todas de 4/16 o mas).
        skip[a * 2] = pos && (log_conn[a * 2] || t == 4);
        skip[a * 2 + 1] = neg && (log_conn[a * 2 + 1] || t == 4);
        box(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], skip);
        if (root && a != 1) {
            for (int sgn = 0; sgn < 2; ++sgn) {
                if (!log_conn[a * 2 + sgn]) continue;
                // Contrafuerte: sube por el lado del tronco.
                const int other = a == 0 ? 2 : 0;
                int flo[3], fhi[3];
                flo[1] = 0;
                fhi[1] = t + 5;
                flo[other] = 8 - h - 1;
                fhi[other] = 8 + h + 1;
                flo[a] = sgn == 0 ? 16 - t : 0;
                fhi[a] = sgn == 0 ? 16 : t;
                bool fskip[6] = {false, false, false, false, false, false};
                fskip[a * 2 + sgn] = true;
                box(flo[0], flo[1], flo[2], fhi[0], fhi[1], fhi[2], fskip);
            }
        }
    }
    if (!any_face) box(c[0] - h, c[1] - h, c[2] - h, c[0] + h, c[1] + h, c[2] + h, nullptr);
    // En diagonal: cubitos escalonados del centro al borde compartido (el del borde lo pone solo una de las dos).
    for (int i = 0; i < n; ++i) {
        const Conn& k = conns[static_cast<std::size_t>(i)];
        if (std::abs(k.d[0]) + std::abs(k.d[1]) + std::abs(k.d[2]) == 1) continue;
        int m[3];
        for (int a = 0; a < 3; ++a) m[a] = (root && a == 1) ? c[1] : 8 + 8 * k.d[a];
        bool first_positive = false;
        for (int a = 0; a < 3; ++a)
            if (k.d[a] != 0) {
                first_positive = k.d[a] > 0;
                break;
            }
        const int steps = t <= 4 ? 3 : 2;
        for (int s = 1; s <= steps; ++s) {
            if (s == steps && !first_positive) break;
            int p[3];
            for (int a = 0; a < 3; ++a) p[a] = c[a] + ((m[a] - c[a]) * s * 2 + (m[a] >= c[a] ? steps : -steps)) / (2 * steps);
            box(p[0] - h, p[1] - h, p[2] - h, p[0] + h, p[1] + h, p[2] + h, nullptr);
        }
    }
}

// PASTO-HZ: vertice de brizna (cara 7 en voxel.vert; se dibuja como planta, cara 6). Posicion = celda + octavos en x
// y z (3 bits cada uno: a 27-29 y a 30-31 + b 31), la y baja `drop` * 3/16 (a 20-21, donde las plantas no usan la
// oclusion). uv (a 18-19), viento (22) y CUERDA (23-26) como las plantas de siempre.
void pushBlade(std::vector<std::uint32_t>& out, int x, int y, int z, int px8, int pz8, int drop, int u, int v, bool waves,
               std::uint32_t level, std::uint32_t b) {
    const int wx = x + px8 / 8, wz = z + pz8 / 8, fx = px8 % 8, fz = pz8 % 8;
    const std::uint32_t a = static_cast<std::uint32_t>(wx) | (static_cast<std::uint32_t>(y) << 5) | (static_cast<std::uint32_t>(wz) << 10) |
                            (7u << 15) | (static_cast<std::uint32_t>(u) << 18) | (static_cast<std::uint32_t>(v) << 19) |
                            (static_cast<std::uint32_t>(drop & 3) << 20) | (waves ? (1u << 22) : 0u) | level |
                            (static_cast<std::uint32_t>(fx) << 27) | (static_cast<std::uint32_t>(fz & 3) << 30);
    out.push_back(a);
    out.push_back(b | (static_cast<std::uint32_t>((fz >> 2) & 1) << 31));
}

// PASTO-HZ: mata de 3 laminas (por sus dos caras: 6 cuadrados, la X eran 4) giradas al azar por columna, con el
// centro desplazado, inclinadas hacia un lado, de alturas distintas y con un tono algo distinto cada una. La base va
// mas oscura (voxel.vert). `top_piece`: el ultimo bloque de la planta (en las apiladas los de abajo van rectos para
// que el de encima siga la misma lamina). La misma textura que la X: vale con los paquetes de texturas.
void emitTuft(std::vector<std::uint32_t>& out, int x, int y, int z, int wx, int wz, int layer, int sky, int torch,
              std::uint16_t tint, bool waves, bool top_piece, std::uint32_t lvl0, std::uint32_t lvl1, int blades = 3) {
    static const auto kDir = [] {
        std::array<std::array<float, 2>, 48> t{};
        for (int i = 0; i < 48; ++i) {
            const double a = static_cast<double>(i) * 3.14159265358979 / 24.0;
            t[static_cast<std::size_t>(i)] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
        return t;
    }();
    const std::uint32_t h = cellHash(wx, 0, wz, 0x7A57u);
    const float cx = 6.0f + static_cast<float>(h % 5u), cz = 6.0f + static_cast<float>((h >> 3) % 5u);  // dieciseisavos
    const int base_dir = static_cast<int>((h >> 6) % 16u);  // 0..15 pasos de 7.5 grados (las 3 laminas a 60)
    const auto q8 = [](float p) { return std::clamp(static_cast<int>(std::lround(p * 0.5f)), 0, 8); };  // a octavos
    for (int i = 0; i < blades; ++i) {  // MALLAS-DETALLE: 3, o 2 / 1 lejos (meshTierFor)
        const std::uint32_t hi = cellHash(wx, i + 1, wz, 0x7A58u);
        const int dir = (base_dir + i * 8 + static_cast<int>(hi % 3u) - 1 + 48) % 48;
        const float dx = kDir[static_cast<std::size_t>(dir)][0], dz = kDir[static_cast<std::size_t>(dir)][1];
        const float half = 6.5f + static_cast<float>((hi >> 2) % 3u) * 0.75f;
        // Base (siempre recta: el bloque de debajo de una planta apilada acaba en ella).
        const float bx0 = cx - dx * half, bz0 = cz - dz * half, bx1 = cx + dx * half, bz1 = cz + dz * half;
        float tx0 = bx0, tz0 = bz0, tx1 = bx1, tz1 = bz1;
        int drop = 0;
        if (top_piece) {
            // Inclinada hacia un lado (perpendicular a la lamina) y algo abierta por arriba.
            const float lean = (((hi >> 5) & 1u) != 0u ? 1.0f : -1.0f) * (2.0f + static_cast<float>((hi >> 6) % 3u));
            const float spread = 1.5f + static_cast<float>((hi >> 8) % 3u) * 0.75f;
            tx0 += -dz * lean - dx * spread;
            tz0 += dx * lean - dz * spread;
            tx1 += -dz * lean + dx * spread;
            tz1 += dx * lean + dz * spread;
            drop = i == 0 ? 0 : static_cast<int>((hi >> 9) % 3u);  // una entera, las otras hasta 6/16 mas bajas
        }
        // Tono de cada lamina: un paso (de 15) mas clara u oscura, a veces algo mas amarilla.
        int r = (tint >> 8) & 15, g = (tint >> 4) & 15, bl = tint & 15;
        const int shade = static_cast<int>((hi >> 11) % 3u) - 1;
        r = std::clamp(r + shade + (((hi >> 13) & 3u) == 0u ? 1 : 0), 0, 15);
        g = std::clamp(g + shade, 0, 15);
        bl = std::clamp(bl + shade, 0, 15);
        const std::uint32_t b = packB(layer, sky, torch, static_cast<std::uint16_t>((r << 8) | (g << 4) | bl));
        const int p[4][2] = {{q8(bx0), q8(bz0)}, {q8(bx1), q8(bz1)}, {q8(tx1), q8(tz1)}, {q8(tx0), q8(tz0)}};
        // Cara de delante (0,1,2,3) y de detras (1,0,3,2), como las dos caras de la X.
        for (int side = 0; side < 2; ++side) {
            const int a0 = side == 0 ? 0 : 1, a1 = side == 0 ? 1 : 0, a2 = side == 0 ? 2 : 3, a3 = side == 0 ? 3 : 2;
            pushBlade(out, x, y, z, p[a0][0], p[a0][1], 0, 0, 1, waves, lvl0, b);
            pushBlade(out, x, y, z, p[a1][0], p[a1][1], 0, 1, 1, waves, lvl0, b);
            pushBlade(out, x, y + 1, z, p[a2][0], p[a2][1], drop, 1, 0, waves, lvl1, b);
            pushBlade(out, x, y + 1, z, p[a3][0], p[a3][1], drop, 0, 0, waves, lvl1, b);
        }
    }
}

// --- Agua (pase del agua: water_voxel.vert) ---
// Caras del agua que se ven: contra el aire, plantas o cristal; entre aguas
// solo el escalon de la mas alta sobre la mas baja (las de dentro no). La cara
// de arriba a 14/16 (como Minecraft) salvo con agua o un bloque encima.
void meshWaterBlock(const SectionSnapshot& s, int x, int y, int z, std::vector<std::uint32_t>& out) {
    // Las plantas dentro del agua (kelp, praderas, abanicos) cuentan como agua quieta:
    // sin caras del agua alrededor de ellas.
    const auto block_at = [&](int bx, int by, int bz) {
        const BlockId b = s.blocks[static_cast<std::size_t>(SectionSnapshot::index(bx, by, bz))];
        return block::isAquatic(b) ? block::Water : b;
    };
    const auto light_at = [&](int bx, int by, int bz) { return s.light[static_cast<std::size_t>(SectionSnapshot::index(bx, by, bz))]; };
    // Altura (dieciseisavos) de la superficie del agua de una celda; -1 = no es agua.
    const auto top_of = [&](int bx, int by, int bz) {
        const int level = block::waterLevel(block_at(bx, by, bz));
        if (level < 0) return -1;
        const BlockId up = by + 1 <= kChunkSize ? block_at(bx, by + 1, bz) : block::Air;
        if (block::isWater(up) || blockDef(up).opaque) return 16;
        return waterHeight16(level);
    };
    const BlockId id = block_at(x, y, z);
    const int level = block::waterLevel(id);
    const int top = top_of(x, y, z);
    const bool falling = block::isWater(block_at(x, y + 1, z));

    // Corriente: hacia los vecinos mas bajos (o abiertos).
    float fx = 0.0f, fz = 0.0f;
    static constexpr int kSides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : kSides) {
        const BlockId nb = block_at(x + d[0], y, z + d[1]);
        const int nl = block::waterLevel(nb);
        float diff = 0.0f;
        if (nl >= 0) diff = static_cast<float>(nl - level);
        else if (!blockDef(nb).solid) diff = 2.0f;
        fx += static_cast<float>(d[0]) * diff;
        fz += static_cast<float>(d[1]) * diff;
    }
    const float flen = std::sqrt(fx * fx + fz * fz);
    int flow_x = 8, flow_z = 8;
    if (flen > 0.01f) {
        const float k = std::min(1.0f, flen / 2.0f) / flen;
        flow_x = std::clamp(static_cast<int>(std::lround(fx * k * 7.0f)) + 8, 1, 15);
        flow_z = std::clamp(static_cast<int>(std::lround(fz * k * 7.0f)) + 8, 1, 15);
    }

    for (int f = 0; f < 6; ++f) {
        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
        const int nx = x + axes.n[0], ny = y + axes.n[1], nz = z + axes.n[2];
        const BlockId nb = block_at(nx, ny, nz);
        const BlockDef& nd = blockDef(nb);
        if (nd.opaque) continue;
        int bottom = 0, high = top;
        bool bottom_surface = false;
        if (f == 2) {
            if (block::isWater(nb)) continue;
        } else if (f == 3) {
            if (block::isWater(nb)) continue;
        } else if (block::isWater(nb)) {
            const int nb_top = top_of(nx, ny, nz);
            if (nb_top >= top) continue;
            bottom = nb_top;  // el escalon que asoma sobre el vecino
            bottom_surface = nb_top < 16;
        }
        if (f != 2 && f != 3 && high <= bottom) continue;
        const std::uint8_t l0 = light_at(x, y, z), l1 = light_at(nx, ny, nz);
        const int sky = std::max(l0 >> 4, l1 >> 4), torch = std::max(l0 & 15, l1 & 15);
        const std::uint32_t b = static_cast<std::uint32_t>(sky) | (static_cast<std::uint32_t>(torch) << 4) | (falling ? (1u << 8) : 0u);
        static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
        for (int c = 0; c < 4; ++c) {
            const int su = kCorner[c][0], sv = kCorner[c][1];
            const auto coord = [&](int base, int n, int u, int v) {
                return base + (n > 0 ? 1 : 0) + (u * su > 0 ? 1 : 0) + (v * sv > 0 ? 1 : 0);
            };
            const int px = coord(x, axes.n[0], axes.u[0], axes.v[0]);
            const int py = coord(y, axes.n[1], axes.u[1], axes.v[1]);
            const int pz = coord(z, axes.n[2], axes.u[2], axes.v[2]);
            const bool upper = py > y;
            const int frac = upper ? high : bottom;
            const bool surface = upper ? high < 16 : (bottom_surface && f != 3);
            const std::uint32_t a = static_cast<std::uint32_t>(px) | (static_cast<std::uint32_t>(y) << 5) |
                                    (static_cast<std::uint32_t>(pz) << 10) | (static_cast<std::uint32_t>(f) << 15) |
                                    (static_cast<std::uint32_t>(frac) << 18) | (surface ? (1u << 23) : 0u) |
                                    (static_cast<std::uint32_t>(flow_x) << 24) | (static_cast<std::uint32_t>(flow_z) << 28);
            out.push_back(a);
            out.push_back(b);
        }
    }
}

// --- Luces (BlockModel Torch, Post, StreetLamp, Campfire, Chandelier, StandingTorch) ---
// Piezas en dieciseisavos. Las texturas de los lados van por franjas: cada
// pieza lee la fila de su altura (v = 1 - y), asi la llama (filas de arriba,
// con emision: titila en voxel.frag) y el palo o el hierro salen solos.
const bool kNoSkip[6] = {false, false, false, false, false, false};
const bool kSkipTop[6] = {false, false, true, false, false, false};
const bool kSkipBottom[6] = {false, false, false, true, false, false};
const bool kSkipTopBottom[6] = {false, false, true, true, false, false};

template <class BlockAt, class Box, class BoxL>
void meshLightModel(const BlockDef& def, const BlockAt& block_at, const Box& box, const BoxL& boxL, int x, int y, int z,
                    std::vector<std::uint32_t>& out, int sky, int torch) {
    const int second = textureLayer(def.side_upper.empty() ? def.side : def.side_upper);
    switch (def.model) {
        case BlockModel::Torch: {
            // En el suelo si debajo hay algo solido; si no, contra la primera pared
            // maciza al lado (palo mas corto, desplazado y con una escuadra).
            int ox = 0, oz = 0, y0 = 0, wall = -1;
            if (!blockDef(block_at(x, y - 1, z)).solid) {
                for (const int f : {0, 1, 4, 5}) {
                    const FaceAxes& a = kFaces[static_cast<std::size_t>(f)];
                    const BlockDef& nd = blockDef(block_at(x + a.n[0], y, z + a.n[2]));
                    if (nd.solid && nd.opaque) {
                        wall = f;
                        break;
                    }
                }
            }
            if (wall >= 0) {
                const FaceAxes& a = kFaces[static_cast<std::size_t>(wall)];
                ox = a.n[0] * 4;
                oz = a.n[2] * 4;
                y0 = 3;
                // Escuadra del palo a la pared.
                const int bx0 = a.n[0] > 0 ? 9 + ox : (a.n[0] < 0 ? 0 : 7);
                const int bx1 = a.n[0] > 0 ? 16 : (a.n[0] < 0 ? 7 + ox : 9);
                const int bz0 = a.n[2] > 0 ? 9 + oz : (a.n[2] < 0 ? 0 : 7);
                const int bz1 = a.n[2] > 0 ? 16 : (a.n[2] < 0 ? 7 + oz : 9);
                box(bx0, 5, bz0, bx1, 7, bz1, kNoSkip);
            }
            box(7 + ox, y0, 7 + oz, 9 + ox, 10, 9 + oz, kSkipTop);     // palo
            box(6 + ox, 10, 6 + oz, 10 + ox, 13, 10 + oz, kNoSkip);    // llama
            box(7 + ox, 13, 7 + oz, 9 + ox, 14, 9 + oz, kSkipBottom);  // punta
            break;
        }
        case BlockModel::Post: {
            // Poste de farola: base si empieza aqui, remate si acaba aqui.
            const BlockId below = block_at(x, y - 1, z), above = block_at(x, y + 1, z);
            const bool base = below != block::LampPost;
            const bool cap = above != block::LampPost && above != block::StreetLamp;
            if (base) box(5, 0, 5, 11, 2, 11, kNoSkip);
            if (cap) box(5, 14, 5, 11, 16, 11, kNoSkip);
            box(6, base ? 2 : 0, 6, 10, cap ? 14 : 16, 10, kSkipTopBottom);
            break;
        }
        case BlockModel::StreetLamp: {
            // Cabeza de farola: cuello, plato, cristal que brilla, tejadillo y remate.
            box(7, 0, 7, 9, 3, 9, kSkipTop);
            box(4, 3, 4, 12, 4, 12, kNoSkip);
            box(5, 4, 5, 11, 11, 11, kSkipTopBottom);
            box(3, 11, 3, 13, 13, 13, kNoSkip);
            box(5, 13, 5, 11, 14, 11, kSkipBottom);
            box(7, 14, 7, 9, 16, 9, kSkipBottom);
            break;
        }
        case BlockModel::Campfire: {
            // Cuatro troncos cruzados (dos abajo a lo largo de X, dos encima a lo
            // largo de Z) y las llamas en dos planos en cruz (segunda textura).
            box(1, 0, 3, 15, 3, 6, kNoSkip);
            box(1, 0, 10, 15, 3, 13, kNoSkip);
            box(3, 3, 1, 6, 6, 15, kNoSkip);
            box(10, 3, 1, 13, 6, 15, kNoSkip);
            const int layers[3] = {second, second, second};
            const bool only_z[6] = {true, true, true, true, false, false};
            const bool only_x[6] = {false, false, true, true, true, true};
            const int z_lo[3] = {3, 1, 8}, z_hi[3] = {13, 15, 8};
            const int x_lo[3] = {8, 1, 3}, x_hi[3] = {8, 15, 13};
            emitBox(out, x, y, z, z_lo, z_hi, only_z, def, sky, torch, layers);
            emitBox(out, x, y, z, x_lo, x_hi, only_x, def, sky, torch, layers);
            break;
        }
        case BlockModel::Chandelier: {
            // Cadena al techo, cubo central, dos brazos en cruz y una vela con su
            // llama en cada punta (las velas con la segunda textura).
            box(7, 10, 7, 9, 16, 9, kSkipBottom);
            box(6, 6, 6, 10, 10, 10, kNoSkip);
            box(2, 6, 7, 14, 7, 9, kNoSkip);
            box(7, 6, 2, 9, 7, 14, kNoSkip);
            const int layers[3] = {textureLayer("torch_flame_top"), second, second};
            static constexpr int kEnds[4][2] = {{2, 7}, {12, 7}, {7, 2}, {7, 12}};
            for (const auto& e : kEnds) {
                boxL(layers, e[0], 7, e[1], e[0] + 2, 11, e[1] + 2, kSkipTopBottom);  // vela
                boxL(layers, e[0], 11, e[1], e[0] + 2, 13, e[1] + 2, kSkipBottom);    // llama
            }
            break;
        }
        case BlockModel::StandingTorch: {
            // Antorcha alta: palo, cesta de hierro con brasas y una llama grande.
            box(7, 0, 7, 9, 10, 9, kSkipTop);
            box(5, 10, 5, 11, 12, 11, kNoSkip);
            box(6, 12, 6, 10, 15, 10, kSkipBottom);
            box(7, 15, 7, 9, 16, 9, kSkipBottom);
            break;
        }
        default:
            box(0, 0, 0, 16, 16, 16, kNoSkip);
            break;
    }
}

// --- Rieles y senales (BlockModel Rail, Buffer, Trace, Lever, Button, Plate, Rune, Relay) ---
// Piezas en dieciseisavos (Rails.h: la forma de la via y del polvo sale de las
// vecinas). Via: traviesas (top) de 1/16 y dos carriles (side) de 2/16 de ancho
// a 4/16 de cada borde; las curvas son un cuarto de circulo de radio 8/16 con
// centro en la esquina (pixel a pixel, juntando filas); las rampas, 8 escalones.
template <class BlockAt, class BoxL>
void meshRailModel(const BlockDef& def, BlockId id, const BlockAt& block_at, const BoxL& boxL, int x, int y, int z) {
    const bool none[6] = {false, false, false, false, false, false};
    const int tie = textureLayer(def.top);
    const int iron = textureLayer(def.side);
    const int own = textureLayer(def.side_upper.empty() ? def.side : def.side_upper);
    const int L_tie[3] = {tie, tie, tie};
    const int L_iron[3] = {iron, iron, iron};
    const int L_own[3] = {own, own, own};
    const int* L_rail = id == block::SurgeRail ? L_own : L_iron;
    // Caja con ejes de via: a lo largo (along), de lado (cross).
    const auto axisBox = [&](bool along_x, int a0, int a1, int c0, int c1, int y0, int y1, const int* layers) {
        if (a0 >= a1 || c0 >= c1 || y0 >= y1) return;
        if (along_x) boxL(layers, a0, y0, c0, a1, y1, c1, none);
        else boxL(layers, c0, y0, a0, c1, y1, a1, none);
    };

    if (def.model == BlockModel::Rail || def.model == BlockModel::Buffer) {
        const rail::RailShape shape = rail::railShape(block_at, x, y, z);
        const int mask = shape.mask;
        if (shape.up >= 0) {
            // Rampa: 8 escalones de 2/16 que suben hacia `up`.
            const bool ax = shape.up < 2;
            const bool pos = shape.up == 0 || shape.up == 2;
            for (int k = 0; k < 8; ++k) {
                const int a0 = pos ? 2 * k : 14 - 2 * k, a1 = a0 + 2, h = 2 * k;
                if ((k & 1) == 0) axisBox(ax, a0, a1, 1, 15, h, h + 1, L_tie);
                axisBox(ax, a0, a1, 3, 5, h + 1, std::min(16, h + 3), L_rail);
                axisBox(ax, a0, a1, 11, 13, h + 1, std::min(16, h + 3), L_rail);
            }
            return;
        }
        const auto straight = [&](bool ax, int a0, int a1) {  // traviesas y carriles en [a0, a1]
            for (const int t : {1, 5, 9, 13})
                if (t >= a0 && t + 2 <= a1) axisBox(ax, t, t + 2, 1, 15, 0, 1, L_tie);
            axisBox(ax, a0, a1, 3, 5, 1, 2, L_rail);
            axisBox(ax, a0, a1, 11, 13, 1, 2, L_rail);
        };
        const int n = rail::bitCount(mask);
        const bool curve = n == 2 && (mask & 3) != 0 && (mask & 12) != 0;
        if (def.model == BlockModel::Buffer) {
            // Tope: la via entera y una viga con dos postes al otro lado de la conexion.
            const int d = rail::firstBit(mask);
            const bool ax = d < 2;
            const bool pos = d == 0 || d == 2;
            straight(ax, 0, 16);
            const int b0 = pos ? 1 : 12, b1 = b0 + 3;
            axisBox(ax, b0, b1, 3, 5, 1, 5, L_iron);
            axisBox(ax, b0, b1, 11, 13, 1, 5, L_iron);
            axisBox(ax, b0, b1, 1, 15, 5, 10, L_own);
            return;
        }
        if (curve) {
            const int cx = (mask & 1) != 0 ? 16 : 0, cz = (mask & 4) != 0 ? 16 : 0;
            for (int pass = 0; pass < 2; ++pass) {  // 0 traviesas, 1 carriles
                for (int j = 0; j < 16; ++j) {
                    int run = -1;
                    for (int i = 0; i <= 16; ++i) {
                        bool in = false;
                        if (i < 16) {
                            const float dx = static_cast<float>(i) + 0.5f - static_cast<float>(cx);
                            const float dz = static_cast<float>(j) + 0.5f - static_cast<float>(cz);
                            const float r = std::sqrt(dx * dx + dz * dz);
                            if (pass == 1) {
                                in = (r >= 3.0f && r < 5.0f) || (r >= 11.0f && r < 13.0f);
                            } else if (r >= 1.5f && r < 14.5f) {
                                const float ang = std::atan2(std::abs(dz), std::abs(dx)) * 57.29578f;
                                for (const float t : {11.0f, 34.0f, 56.0f, 79.0f})
                                    if (std::abs(ang - t) * r * 0.01745f < 1.0f) in = true;
                            }
                        }
                        if (in && run < 0) run = i;
                        if (!in && run >= 0) {
                            if (pass == 0) boxL(L_tie, run, 0, j, i, 1, j + 1, none);
                            else boxL(L_rail, run, 1, j, i, 2, j + 1, none);
                            run = -1;
                        }
                    }
                }
            }
        } else {
            // Rectas: un eje entero si tiene los dos lados; si no, medias hasta el centro.
            for (const bool ax : {true, false}) {
                const int lo = ax ? 1 : 3, hi = ax ? 0 : 2;  // bits -X/+X o -Z/+Z
                const bool has_hi = (mask & (1 << hi)) != 0, has_lo = (mask & (1 << lo)) != 0;
                if (has_hi && has_lo) straight(ax, 0, 16);
                else if (has_hi) straight(ax, 8, 16);
                else if (has_lo) straight(ax, 0, 8);
            }
        }
        const bool main_x = (mask & 3) != 0;
        if (id == block::DragRail) {
            axisBox(main_x, 2, 14, 6, 7, 1, 2, L_own);  // zapatas de freno
            axisBox(main_x, 2, 14, 9, 10, 1, 2, L_own);
        } else if (id == block::SensorRail) {
            axisBox(main_x, 4, 12, 6, 10, 1, 2, L_own);  // placa con la runa
        } else if (id == block::SwitchRail) {
            boxL(L_iron, 13, 0, 1, 15, 5, 3, none);  // poste y cabeza de la palanca del cambio
            boxL(L_own, 12, 5, 0, 16, 8, 4, none);
        }
        return;
    }

    if (def.model == BlockModel::Trace) {
        const int L[3] = {tie, tie, tie};  // todo con la textura del polvo
        const rail::TraceShape t = rail::traceShape(block_at, x, y, z);
        boxL(L, 6, 0, 6, 10, 1, 10, none);
        for (int d = 0; d < 4; ++d) {
            const int bit = 1 << d;
            if (((t.flat | t.up | t.down) & bit) == 0) continue;
            const bool ax = d < 2, pos = d == 0 || d == 2;
            axisBox(ax, pos ? 10 : 0, pos ? 16 : 6, 7, 9, 0, 1, L);
            if ((t.up & bit) != 0) axisBox(ax, pos ? 15 : 0, pos ? 16 : 1, 7, 9, 1, 16, L);  // sube por la pared
        }
        return;
    }

    // Montaje (palanca, boton): en el suelo si hay algo solido debajo; si no, contra la primera pared.
    int wall = -1;
    if ((def.model == BlockModel::Lever || def.model == BlockModel::Button) && !blockDef(block_at(x, y - 1, z)).solid) {
        for (const int f : {0, 1, 4, 5}) {
            const FaceAxes& a = kFaces[static_cast<std::size_t>(f)];
            const BlockDef& nd = blockDef(block_at(x + a.n[0], y, z + a.n[2]));
            if (nd.solid && nd.opaque) {
                wall = f;
                break;
            }
        }
    }
    const int base_layer = textureLayer(def.side);
    const int L_base[3] = {base_layer, base_layer, base_layer};
    // Caja en el marco del montaje (fx a lo largo, fy separacion de la superficie, fz) -> celda.
    const auto mounted = [&](const int* layers, int fx0, int fy0, int fz0, int fx1, int fy1, int fz1) {
        const auto map = [&](int fx, int fy, int fz, int out[3]) {
            switch (wall) {
                case 0: out[0] = 16 - fy; out[1] = fz; out[2] = fx; break;
                case 1: out[0] = fy; out[1] = fz; out[2] = fx; break;
                case 4: out[0] = fx; out[1] = fz; out[2] = 16 - fy; break;
                case 5: out[0] = fx; out[1] = fz; out[2] = fy; break;
                default: out[0] = fx; out[1] = fy; out[2] = fz; break;
            }
        };
        int a[3], b[3];
        map(fx0, fy0, fz0, a);
        map(fx1, fy1, fz1, b);
        boxL(layers, std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2]), std::max(a[0], b[0]),
             std::max(a[1], b[1]), std::max(a[2], b[2]), none);
    };
    switch (def.model) {
        case BlockModel::Lever: {
            const int s = id == block::RuneLeverOn ? 1 : -1;  // hacia donde se inclina
            mounted(L_base, 4, 0, 5, 12, 2, 11);
            mounted(L_own, 7, 2, 7, 9, 5, 9);
            mounted(L_own, 7, 5, 7 + s, 9, 8, 9 + s);
            mounted(L_own, 7, 8, 7 + 2 * s, 9, 10, 9 + 2 * s);
            mounted(L_own, 6, 10, 6 + 2 * s, 10, 12, 10 + 2 * s);
            break;
        }
        case BlockModel::Button:
            mounted(L_base, 4, 0, 5, 12, 1, 11);
            mounted(L_own, 5, 1, 6, 11, id == block::RuneButtonOn ? 2 : 3, 10);
            break;
        case BlockModel::Plate:
            boxL(L_tie, 1, 0, 1, 15, 1, 15, none);
            break;
        case BlockModel::Rune:
            boxL(L_base, 4, 0, 4, 12, 2, 12, none);
            boxL(L_base, 6, 2, 6, 10, 10, 10, none);
            boxL(L_own, 5, 10, 5, 11, 15, 11, none);
            break;
        case BlockModel::Relay: {
            const int L_slab[3] = {tie, base_layer, base_layer};
            boxL(L_slab, 1, 0, 1, 15, 2, 15, none);
            boxL(L_own, 3, 2, 6, 6, 6, 10, none);
            boxL(L_own, 10, 2, 6, 13, 6, 10, none);
            break;
        }
        default:
            break;
    }
}

// --- Muebles (BlockModel Table, Chair, Bench, Brazier) ---
// Antes eran cubos enteros con textura recortada: la mesa, las sillas y los
// bancos de un comedor se veian como un bloque de celosia y el brasero como una
// mesa con un tablero oscuro flotando sobre el cuenco. Ahora son piezas en
// dieciseisavos con texturas de madera / hierro enteras (sin recorte). Caras
// (kFaces): 0 +X, 1 -X, 4 +Z, 5 -Z.
template <class BlockAt, class BoxL>
void meshFurnitureModel(const BlockDef& def, BlockId id, const BlockAt& block_at, const BoxL& boxL, int x, int y, int z,
                        std::vector<std::uint32_t>& out, int sky, int torch) {
    static const bool kNone[6] = {false, false, false, false, false, false};
    const auto nb = [&](int f) {
        const FaceAxes& a = kFaces[static_cast<std::size_t>(f)];
        return block_at(x + a.n[0], y, z + a.n[2]);
    };
    const auto wall = [&](int f) {
        const BlockDef& d = blockDef(nb(f));
        return d.solid && d.opaque;
    };
    switch (def.model) {
        case BlockModel::Table: {
            // Tablero de 2/16 y una pata en cada esquina libre: mesas juntas forman un
            // tablero largo (las caras entre ellas se quitan) con patas solo en los extremos.
            const int wood = textureLayer("table_top");
            const int L[3] = {wood, wood, wood};
            boxL(L, 0, 14, 0, 16, 16, 16, kNone);
            for (const int cx : {0, 1}) {
                for (const int cz : {0, 1}) {
                    if (nb(cx ? 0 : 1) == id || nb(cz ? 4 : 5) == id) continue;
                    const int x0 = cx ? 13 : 1, z0 = cz ? 13 : 1;
                    boxL(L, x0, 0, z0, x0 + 2, 14, z0 + 2, kSkipTop);
                }
            }
            break;
        }
        case BlockModel::Chair: {
            // Respaldo lejos de la mesa de al lado; si no hay mesa, contra la pared; si no, a -Z.
            int back = 5;
            bool found = false;
            for (const int f : {0, 1, 4, 5}) {
                if (blockDef(nb(f)).model == BlockModel::Table) {
                    back = f ^ 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (const int f : {0, 1, 4, 5}) {
                    if (wall(f)) {
                        back = f;
                        break;
                    }
                }
            }
            const int wood = textureLayer("bench_top");
            const int L[3] = {wood, wood, wood};
            // Piezas con el respaldo a -Z; se giran segun `back`.
            const auto part = [&](int x0, int y0, int z0, int x1, int y1, int z1, const bool* skip) {
                const auto map = [&](int px, int pz, int& ox, int& oz) {
                    switch (back) {
                        case 4: ox = px; oz = 16 - pz; break;
                        case 1: ox = pz; oz = px; break;
                        case 0: ox = 16 - pz; oz = px; break;
                        default: ox = px; oz = pz; break;
                    }
                };
                int ax, az, bx, bz;
                map(x0, z0, ax, az);
                map(x1, z1, bx, bz);
                boxL(L, std::min(ax, bx), y0, std::min(az, bz), std::max(ax, bx), y1, std::max(az, bz), skip);
            };
            part(3, 7, 3, 13, 9, 13, kNone);  // asiento
            for (const int px : {3, 11})
                for (const int pz : {3, 11}) part(px, 0, pz, px + 2, 7, pz + 2, kSkipTop);  // patas
            part(3, 9, 3, 5, 16, 5, kSkipBottom);    // postes del respaldo
            part(11, 9, 3, 13, 16, 5, kSkipBottom);
            part(5, 13, 3, 11, 15, 5, kNone);        // travesano de arriba
            part(7, 9, 3, 9, 13, 5, kSkipTopBottom);  // tablilla
            break;
        }
        case BlockModel::Bench: {
            // Banco: asiento a lo largo de su eje (junto a otros bancos o a lo largo del
            // borde de la mesa) y patas solo en los extremos libres.
            bool along_x = true;
            if (nb(0) == id || nb(1) == id) along_x = true;
            else if (nb(4) == id || nb(5) == id) along_x = false;
            else if (blockDef(nb(0)).model == BlockModel::Table || blockDef(nb(1)).model == BlockModel::Table) along_x = false;
            const int wood = textureLayer("bench_top");
            const int L[3] = {wood, wood, wood};
            if (along_x) {
                boxL(L, 0, 6, 3, 16, 8, 13, kNone);
                if (nb(1) != id) boxL(L, 1, 0, 4, 3, 6, 12, kSkipTop);
                if (nb(0) != id) boxL(L, 13, 0, 4, 15, 6, 12, kSkipTop);
            } else {
                boxL(L, 3, 6, 0, 13, 8, 16, kNone);
                if (nb(5) != id) boxL(L, 4, 0, 1, 12, 6, 3, kSkipTop);
                if (nb(4) != id) boxL(L, 4, 0, 13, 12, 6, 15, kSkipTop);
            }
            break;
        }
        case BlockModel::Brazier: {
            // Brasero: pie, columna, cuenco de hierro con brasas encima y llamas en cruz.
            const int iron = textureLayer("anvil_top");
            const int I[3] = {iron, iron, iron};
            const int bowl[3] = {textureLayer(def.top), iron, iron};
            boxL(I, 4, 0, 4, 12, 1, 12, kNone);
            boxL(I, 7, 1, 7, 9, 6, 9, kSkipTopBottom);
            boxL(bowl, 3, 6, 3, 13, 10, 13, kNone);
            const int fl = textureLayer(def.side);
            const int F[3] = {fl, fl, fl};
            const bool only_z[6] = {true, true, true, true, false, false};
            const bool only_x[6] = {false, false, true, true, true, true};
            const int z_lo[3] = {3, 10, 8}, z_hi[3] = {13, 16, 8};
            const int x_lo[3] = {8, 10, 3}, x_hi[3] = {8, 16, 13};
            emitBox(out, x, y, z, z_lo, z_hi, only_z, def, sky, torch, F);
            emitBox(out, x, y, z, x_lo, x_hi, only_x, def, sky, torch, F);
            break;
        }
        default:
            break;
    }
}

// Puerta (BlockModel::Door): la hoja va a lo largo de X si hay mas paredes
// (macizas y opacas) a +-X que a +-Z; empate: hacia la otra hoja de una puerta
// doble; si no, a lo largo de X. Puertas.cpp (juego) usa la misma regla.
template <class BlockAt>
bool doorSpansX(const BlockAt& block_at, int x, int y, int z) {
    const auto wall = [&](int bx, int bz) {
        const BlockDef& d = blockDef(block_at(bx, y, bz));
        return d.solid && d.opaque ? 1 : 0;
    };
    const int sx = wall(x + 1, z) + wall(x - 1, z), sz = wall(x, z + 1) + wall(x, z - 1);
    if (sx != sz) return sx > sz;
    const BlockId id = block_at(x, y, z);
    const int dx = (block_at(x + 1, y, z) == id ? 1 : 0) + (block_at(x - 1, y, z) == id ? 1 : 0);
    const int dz = (block_at(x, y, z + 1) == id ? 1 : 0) + (block_at(x, y, z - 1) == id ? 1 : 0);
    if (dx != dz) return dx > dz;
    return true;
}

}  // namespace

int waterHeight16(int level) {
    static constexpr int kHeights[8] = {14, 12, 11, 9, 8, 6, 5, 3};
    return kHeights[std::clamp(level, 0, 7)];
}

void meshSection(const SectionSnapshot& s, std::vector<std::uint32_t>& out, std::vector<std::uint32_t>* water,
                 std::uint8_t* tier_flags) {
    out.clear();
    if (water != nullptr) water->clear();
    std::uint8_t flags = 0;  // MALLAS-DETALLE: kTierDecor / kTierFaces
    const auto block_at = [&](int x, int y, int z) { return s.blocks[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))]; };
    const auto light_at = [&](int x, int y, int z) { return s.light[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))]; };
    const auto opaque = [&](int x, int y, int z) { return blockDef(block_at(x, y, z)).opaque; };
    // MALLAS-DETALLE: nivel de adornos de la seccion (2 = como siempre).
    const int tier = std::clamp(static_cast<int>(s.mesh_tier), 0, 2);
    const bool smooth_setting = g_smooth_light.get();
    const bool smooth_light = smooth_setting && tier > 0;
    const bool fluffy_setting = g_fluffy_leaves.get();  // HOJAS
    const bool fluffy = fluffy_setting && tier > 0;
    const bool hanging_leaves = tier > 1;
    const bool ore_setting = g_ore_relief.get();  // MENAS-HZ
    const bool ore_relief = ore_setting && tier > 0;
    const int tuft_blades = tier == 2 ? 3 : tier == 1 ? 2 : 1;

    // Caras que se pueden fusionar: por cara (6), capa a lo largo de la normal
    // (16) y celda en el plano (16 x 16), la clave (textura, tinte, luz,
    // oclusion) con el bit 63 de "hay cara".
    constexpr int kN = kChunkSize;
    thread_local std::vector<std::uint64_t> merge;
    merge.assign(static_cast<std::size_t>(6 * kN * kN * kN), 0);
    const auto merge_index = [&](int f, int bx, int by, int bz) {
        const int b[3] = {bx, by, bz};
        const int d = f / 2;
        const int slice = b[d], p = b[(d + 1) % 3], q = b[(d + 2) % 3];
        return static_cast<std::size_t>(((f * kN + slice) * kN + q) * kN + p);
    };

    for (int y = 0; y < kChunkSize; ++y) {
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                const BlockId id = block_at(x, y, z);
                const BlockDef& def = blockDef(id);
                if (def.shape == BlockShape::Liquid) {
                    if (water != nullptr && block::isWater(id)) meshWaterBlock(s, x, y, z, *water);
                    continue;
                }
                if (def.shape == BlockShape::Air) continue;
                const std::size_t column = static_cast<std::size_t>((x + 1) + (z + 1) * SectionSnapshot::kSize);

                if (def.shape == BlockShape::Cross) {
                    const std::uint8_t l = light_at(x, y, z);
                    const int sky = l >> 4, torch = std::max<int>(l & 15, def.light);
                    const std::uint16_t tint = def.tint == BlockTint::Grass ? s.grass_tint[column]
                                               : def.tint == BlockTint::Foliage ? s.foliage_tint[column] : kWhite;
                    // PLANTAS-ALTAS: con la misma planta encima, el tramo de abajo (sin remate)
                    // LIANAS: colgando, con la misma liana debajo, el tramo sin puntas
                    const bool tramo = (isStackPlant(def) && block_at(x, y + 1, z) == id) || (isHangPlant(def) && block_at(x, y - 1, z) == id);
                    const int layer = tramo ? textureLayer(def.side + "_low") : textureLayer(def.side);
                    // CUERDA: plantas de varios bloques (cana, algas, lianas...): cuantos bloques de la misma planta hay
                    // debajo (la seccion ve 1 de borde). Va en los bits 23-26 del vertice (libres en las plantas): abajo
                    // `below`, arriba `below + 1`. voxel.vert mece toda la columna junta (como una cuerda), base fija.
                    int below = 0;
                    if (def.waves) {
                        for (int yy = y - 1; yy >= -1 && below < 14; --yy) {
                            const BlockDef& bd = blockDef(block_at(x, yy, z));
                            if (bd.shape != BlockShape::Cross || !bd.waves) break;
                            ++below;
                        }
                    }
                    std::uint32_t lvl0 = static_cast<std::uint32_t>(below) << 23;
                    std::uint32_t lvl1 = static_cast<std::uint32_t>(below + 1) << 23;
                    // CUELGA: lianas que cuelgan del techo / de las hojas: la cuerda se ancla ARRIBA. Se cuentan los
                    // tramos de encima (hasta el techo): arriba `above` (0 = pegado al techo, quieto), abajo `above + 1`.
                    if (def.waves && (isHangPlant(def) || def.side == "jungle_vines")) {
                        int above = 0;
                        for (int yy = y + 1; yy <= kChunkSize && above < 14; ++yy) {
                            const BlockDef& bd = blockDef(block_at(x, yy, z));
                            if (bd.shape != BlockShape::Cross || !bd.waves) break;
                            ++above;
                        }
                        lvl0 = static_cast<std::uint32_t>(above + 1) << 23;
                        lvl1 = static_cast<std::uint32_t>(above) << 23;
                    }
                    // PASTO-HZ: hierba, helechos, hierba seca, juncos: mata de laminas giradas en lugar de la X.
                    if (isTuftPlant(id)) {
                        flags |= kTierDecor;
                        const bool top_piece = !(isStackPlant(def) && block_at(x, y + 1, z) == id);
                        emitTuft(out, x, y, z, s.cx * kChunkSize + x, s.cz * kChunkSize + z, layer, sky, torch, tint, def.waves,
                                 top_piece, lvl0, lvl1, tuft_blades);
                        continue;
                    }
                    // Dos planos diagonales, cada uno por sus dos caras.
                    const int planes[2][4] = {{0, 0, 1, 1}, {1, 0, 0, 1}};  // (x0, z0, x1, z1)
                    for (const auto& p : planes) {
                        for (int side = 0; side < 2; ++side) {
                            const int ax = side == 0 ? p[0] : p[2], az = side == 0 ? p[1] : p[3];
                            const int bx = side == 0 ? p[2] : p[0], bz = side == 0 ? p[3] : p[1];
                            const std::uint32_t b = packB(layer, sky, torch, tint);
                            out.push_back(packA(x + ax, y, z + az, 6, 0, 1, 3, def.waves) | lvl0);
                            out.push_back(b);
                            out.push_back(packA(x + bx, y, z + bz, 6, 1, 1, 3, def.waves) | lvl0);
                            out.push_back(b);
                            out.push_back(packA(x + bx, y + 1, z + bz, 6, 1, 0, 3, def.waves) | lvl1);
                            out.push_back(b);
                            out.push_back(packA(x + ax, y + 1, z + az, 6, 0, 0, 3, def.waves) | lvl1);
                            out.push_back(b);
                        }
                    }
                    continue;
                }

                if (def.model != BlockModel::Full) {
                    const std::uint8_t l = light_at(x, y, z);
                    const int sky = l >> 4, torch = std::max<int>(l & 15, def.light);
                    // Vecino de cada cara: tapa (opaco) o es del mismo tipo.
                    bool covered[6], same[6];
                    for (int f = 0; f < 6; ++f) {
                        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                        const BlockId nb = block_at(x + axes.n[0], y + axes.n[1], z + axes.n[2]);
                        covered[f] = blockDef(nb).opaque;
                        same[f] = nb == id;
                    }
                    // Cara en el borde de la celda: se quita si el vecino la tapa.
                    const auto boxL = [&](const int* layers, int x0, int y0, int z0, int x1, int y1, int z1,
                                          const bool inner_skip[6]) {
                        const int lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
                        bool skip[6];
                        for (int f = 0; f < 6; ++f) {
                            const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                            int k = 0;
                            while (axes.n[k] == 0) ++k;
                            const bool at_edge = axes.n[k] > 0 ? hi[k] == 16 : lo[k] == 0;
                            skip[f] = inner_skip[f] || (at_edge && (covered[f] || same[f]));
                        }
                        emitBox(out, x, y, z, lo, hi, skip, def, sky, torch, layers);
                    };
                    const auto box = [&](int x0, int y0, int z0, int x1, int y1, int z1, const bool inner_skip[6]) {
                        boxL(nullptr, x0, y0, z0, x1, y1, z1, inner_skip);
                    };
                    const bool none[6] = {false, false, false, false, false, false};
                    if (def.model == BlockModel::Fence) {
                        box(6, 0, 6, 10, 16, 10, none);  // poste
                        // Travesanos (dos alturas) hacia vallas y bloques macizos: caras 0 +X, 1 -X, 4 +Z, 5 -Z.
                        for (const int f : {0, 1, 4, 5}) {
                            const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                            const BlockId nb = block_at(x + axes.n[0], y, z + axes.n[2]);
                            const BlockDef& nd = blockDef(nb);
                            if (nb != id && !(nd.opaque && nd.solid && nd.shape == BlockShape::Cube)) continue;
                            bool inner[6] = {false, false, false, false, false, false};
                            inner[f ^ 1] = true;  // la punta que entra en el poste (0<->1, 4<->5)
                            for (const int y0 : {5, 11}) {
                                if (f == 0) box(10, y0, 7, 16, y0 + 2, 9, inner);
                                if (f == 1) box(0, y0, 7, 6, y0 + 2, 9, inner);
                                if (f == 4) box(7, y0, 10, 9, y0 + 2, 16, inner);
                                if (f == 5) box(7, y0, 0, 9, y0 + 2, 6, inner);
                            }
                        }
                    } else if (def.model == BlockModel::Lantern) {
                        // Farol: cuerpo y tapa; cadena si hay algo encima; pie si esta apoyado en algo
                        // solido; si no, colgado de un brazo de hierro a la pared maciza de al lado (los
                        // faroles de pared junto a las puertas). Sin nada: cadena (antes flotaba con su pie).
                        box(5, 2, 5, 11, 10, 11, none);
                        box(6, 10, 6, 10, 12, 10, none);
                        int wall = -1;
                        const bool hang = block_at(x, y + 1, z) != block::Air;
                        const bool stand = !hang && blockDef(block_at(x, y - 1, z)).solid;
                        if (!hang && !stand) {
                            for (const int f : {0, 1, 4, 5}) {
                                const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                                const BlockDef& nd = blockDef(block_at(x + axes.n[0], y, z + axes.n[2]));
                                if (nd.solid && nd.opaque) {
                                    wall = f;
                                    break;
                                }
                            }
                        }
                        if (stand) {
                            box(6, 0, 6, 10, 2, 10, none);
                        } else if (wall < 0) {
                            box(7, 12, 7, 9, 16, 9, none);
                        } else {
                            // Eslabon, brazo (de la cadena a la pared) y placa contra la pared.
                            box(7, 12, 7, 9, 13, 9, none);
                            static constexpr int kArm[4][2][6] = {
                                {{7, 13, 7, 16, 15, 9}, {15, 10, 6, 16, 16, 10}},  // +X
                                {{0, 13, 7, 9, 15, 9}, {0, 10, 6, 1, 16, 10}},     // -X
                                {{7, 13, 7, 9, 15, 16}, {6, 10, 15, 10, 16, 16}},  // +Z
                                {{7, 13, 0, 9, 15, 9}, {6, 10, 0, 10, 16, 1}},     // -Z
                            };
                            const int k = wall < 2 ? wall : wall - 2;
                            for (const auto& q : kArm[k]) box(q[0], q[1], q[2], q[3], q[4], q[5], none);
                        }
                    } else if (def.model == BlockModel::Door) {
                        // Hoja de puerta (4/16 de grueso, en el centro) a lo largo del eje con paredes a
                        // los lados. La mitad de arriba (debajo otra puerta y encima no) con side_upper.
                        // Puertas.cpp (juego) repite esta regla para colocar la hoja que se abre.
                        const bool along_x = doorSpansX(block_at, x, y, z);
                        const bool upper = !def.side_upper.empty() && block_at(x, y - 1, z) == id && block_at(x, y + 1, z) != id;
                        const int side = textureLayer(upper ? def.side_upper : def.side);
                        const int layers[3] = {side, side, side};
                        if (along_x) boxL(layers, 0, 0, 6, 16, 16, 10, none);
                        else boxL(layers, 6, 0, 0, 10, 16, 16, none);
                    } else if (def.model == BlockModel::Table || def.model == BlockModel::Chair ||
                               def.model == BlockModel::Bench || def.model == BlockModel::Brazier) {
                        meshFurnitureModel(def, id, block_at, boxL, x, y, z, out, sky, torch);
                    } else if (def.model == BlockModel::Branch) {
                        meshBranch(out, block_at, x, y, z, sky, torch);  // RAMAS-HZ
                    } else if (def.model >= BlockModel::Rail) {
                        meshRailModel(def, id, block_at, boxL, x, y, z);
                    } else {
                        meshLightModel(def, block_at, box, boxL, x, y, z, out, sky, torch);
                    }
                    continue;
                }

                // HOJAS: la lamina esponjosa de las caras al aire (no hacia otras hojas ni hacia un bloque macizo).
                if (fluffy_setting && isFluffyLeaves(def)) flags |= kTierDecor;
                if (fluffy && isFluffyLeaves(def)) {
                    const int fl = textureLayer(def.side + "_fluff");
                    const std::uint16_t ftint = def.tint == BlockTint::Foliage ? s.foliage_tint[column] : kWhite;
                    for (int f = 0; f < 6; ++f) {
                        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                        const int nx = x + axes.n[0], ny = y + axes.n[1], nz = z + axes.n[2];
                        const BlockDef& ndef = blockDef(block_at(nx, ny, nz));
                        if (ndef.opaque || isFluffyLeaves(ndef) || ndef.shape == BlockShape::Liquid) continue;
                        const std::uint8_t l = light_at(nx, ny, nz);
                        // COPAS-HZ: cada lamina con su propio vuelo (2-5/16), ancho y desplazamiento (antes todas a 4:
                        // la copa se leia como una caja inflada). Debajo, a veces hojas que cuelgan.
                        const std::uint32_t fh = cellHash(s.cx * kChunkSize + x, s.sy * kChunkSize + y, s.cz * kChunkSize + z,
                                                          0xF1u + static_cast<std::uint32_t>(f));
                        const int eo = 2 + static_cast<int>(fh % 4u), es = 2 + static_cast<int>((fh >> 2) % 4u);
                        const int du = static_cast<int>((fh >> 4) % 5u) - 2, dv = static_cast<int>((fh >> 7) % 5u) - 2;
                        emitFluff(out, x, y, z, f, fl, l >> 4, std::max<int>(l & 15, def.light), ftint, def.waves, eo, es, du, dv);
                        if (hanging_leaves && f == 3 && ((fh >> 10) % 5u) < 3u && block_at(nx, ny, nz) == block::Air) {
                            // Hojas que cuelgan: dos laminas verticales en cruz de 5 a 10/16 por debajo de la cara.
                            const int len = 5 + static_cast<int>((fh >> 13) % 6u);
                            const int px = 4 + static_cast<int>((fh >> 16) % 9u), pz = 4 + static_cast<int>((fh >> 20) % 9u);
                            const int a0 = 2 + static_cast<int>((fh >> 24) % 3u), a1 = 14 - static_cast<int>((fh >> 26) % 3u);
                            const bool only_x[6] = {false, false, true, true, true, true};  // lamina en el plano YZ
                            const bool only_z[6] = {true, true, true, true, false, false};  // lamina en el plano XY
                            const int lo_x[3] = {px, -len, a0}, hi_x[3] = {px, 2, a1};
                            const int lo_z[3] = {a0, -len, pz}, hi_z[3] = {a1, 2, pz};
                            emitBoxBiased(out, x, y, z, lo_x, hi_x, only_x, fl, l >> 4, std::max<int>(l & 15, def.light), ftint, def.waves);
                            emitBoxBiased(out, x, y, z, lo_z, hi_z, only_z, fl, l >> 4, std::max<int>(l & 15, def.light), ftint, def.waves);
                        }
                    }
                }
                // MENAS-HZ: pepitas que sobresalen de las caras al aire (no hacia bloques opacos ni hacia el agua).
                if (shapeInfo()[id].gem >= 0 && ore_setting) flags |= kTierDecor;
                if (const int gem = shapeInfo()[id].gem; gem >= 0 && ore_relief) {
                    const int wx = s.cx * kChunkSize + x, wy = s.sy * kChunkSize + y, wz = s.cz * kChunkSize + z;
                    for (int f = 0; f < 6; ++f) {
                        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                        const int nx = x + axes.n[0], ny = y + axes.n[1], nz = z + axes.n[2];
                        const BlockDef& ndef = blockDef(block_at(nx, ny, nz));
                        if (ndef.opaque || ndef.shape == BlockShape::Liquid) continue;
                        const std::uint8_t l = light_at(nx, ny, nz);
                        emitNuggets(out, x, y, z, f, cellHash(wx, wy, wz, 0x4D45u + static_cast<std::uint32_t>(f)), gem, l >> 4,
                                    std::max<int>(l & 15, def.light), tier < 2);
                    }
                }

                for (int f = 0; f < 6; ++f) {
                    const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                    const int nx = x + axes.n[0], ny = y + axes.n[1], nz = z + axes.n[2];
                    const BlockId neighbor = block_at(nx, ny, nz);
                    const BlockDef& ndef = blockDef(neighbor);
                    if (ndef.opaque) continue;
                    if (neighbor == id && def.cutout) continue;  // cristal con cristal, hojas con hojas
                    if (smooth_setting) flags |= kTierFaces;
                    // Mitad de arriba de una puerta (abajo el mismo bloque y encima otro).
                    const bool upper = f != 2 && f != 3 && !def.side_upper.empty() && block_at(x, y - 1, z) == id &&
                                       block_at(x, y + 1, z) != id;
                    const std::string& texture =
                        f == 2 ? def.top : (f == 3 ? def.bottom : (upper ? def.side_upper : def.side));
                    const int layer = textureLayer(texture);
                    std::uint16_t tint = kWhite;
                    if (def.tint == BlockTint::Grass && f == 2) tint = s.grass_tint[column];
                    if (def.tint == BlockTint::Foliage) tint = s.foliage_tint[column];

                    // Esquinas: (su, sv) = signo en u y en v. Orden BL, BR, TR, TL.
                    static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
                    int ao[4];
                    int sky[4];
                    int torch[4];
                    int px[4], py[4], pz[4];
                    for (int c = 0; c < 4; ++c) {
                        const int su = kCorner[c][0], sv = kCorner[c][1];
                        const int ux = axes.u[0] * su, uy = axes.u[1] * su, uz = axes.u[2] * su;
                        const int vx = axes.v[0] * sv, vy = axes.v[1] * sv, vz = axes.v[2] * sv;
                        const bool side1 = opaque(nx + ux, ny + uy, nz + uz);
                        const bool side2 = opaque(nx + vx, ny + vy, nz + vz);
                        const bool corner = opaque(nx + ux + vx, ny + uy + vy, nz + uz + vz);
                        ao[c] = (side1 && side2) ? 0 : 3 - (static_cast<int>(side1) + static_cast<int>(side2) + static_cast<int>(corner));
                        // Luz: media de las celdas no opacas de delante de la esquina.
                        int sum_sky = 0, sum_torch = 0, count = 0;
                        const auto add = [&](int lx, int ly, int lz, bool blocked) {
                            if (blocked) return;
                            const std::uint8_t l = light_at(lx, ly, lz);
                            sum_sky += l >> 4;
                            sum_torch += l & 15;
                            ++count;
                        };
                        add(nx, ny, nz, false);
                        add(nx + ux, ny + uy, nz + uz, side1);
                        add(nx + vx, ny + vy, nz + vz, side2);
                        add(nx + ux + vx, ny + uy + vy, nz + uz + vz, corner || (side1 && side2));
                        sky[c] = count > 0 ? (sum_sky + count / 2) / count : 0;
                        torch[c] = count > 0 ? (sum_torch + count / 2) / count : 0;
                        torch[c] = std::max(torch[c], static_cast<int>(def.light));
                        if (!smooth_light) {
                            // Luz plana: la de la celda de delante, sin sombra de esquina.
                            const std::uint8_t l = light_at(nx, ny, nz);
                            ao[c] = 3;
                            sky[c] = l >> 4;
                            torch[c] = std::max<int>(l & 15, def.light);
                        }
                        // Posicion de la esquina (enteros 0..16 dentro de la seccion).
                        const auto coord = [&](int base, int n, int u, int v) {
                            return base + (n > 0 ? 1 : 0) + (u * su > 0 ? 1 : 0) + (v * sv > 0 ? 1 : 0);
                        };
                        px[c] = coord(x, axes.n[0], axes.u[0], axes.v[0]);
                        py[c] = coord(y, axes.n[1], axes.u[1], axes.v[1]);
                        pz[c] = coord(z, axes.n[2], axes.u[2], axes.v[2]);
                    }
                    // Bloque opaco y quieto: la cara se fusiona al final con sus vecinas iguales
                    // (misma textura, tinte y la misma luz y oclusion en cada esquina). La clave
                    // lleva las cuatro esquinas: una tira al pie de una pared (oscura abajo, clara
                    // arriba) se junta a lo largo, donde no cambia.
                    if (def.opaque && !def.cutout && !def.waves) {
                        std::uint64_t key = (1ull << 63) | static_cast<std::uint64_t>(layer & 1023) |
                                            (static_cast<std::uint64_t>(tint & 0xFFF) << 10);
                        for (int c = 0; c < 4; ++c) {
                            const std::uint64_t bits = static_cast<std::uint64_t>(ao[c] & 3) |
                                                       (static_cast<std::uint64_t>(sky[c] & 15) << 2) |
                                                       (static_cast<std::uint64_t>(torch[c] & 15) << 6);
                            key |= bits << (22 + 10 * c);
                        }
                        merge[merge_index(f, x, y, z)] = key;
                        continue;
                    }
                    // Diagonal: por la pareja de esquinas mas clara (sin corte visible).
                    const int order[4] = {0, 1, 2, 3};
                    const int rotated[4] = {1, 2, 3, 0};
                    const int* o = (ao[0] + ao[2] < ao[1] + ao[3]) ? rotated : order;
                    static constexpr int kUv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
                    for (int k = 0; k < 4; ++k) {
                        const int c = o[k];
                        out.push_back(packA(px[c], py[c], pz[c], f, kUv[c][0], kUv[c][1], ao[c], def.waves));
                        out.push_back(packB(layer, sky[c], torch[c], tint));
                    }
                }
            }
        }
    }

    // Fusion: en cada capa, rectangulos de caras con la misma clave. Se alarga
    // por un eje solo si la luz de las esquinas no cambia a lo largo de el (la
    // interpolacion del rectangulo da lo mismo que las caras sueltas).
    const auto corner_of = [](std::uint64_t key, int c) { return static_cast<int>((key >> (22 + 10 * c)) & 1023u); };
    for (int f = 0; f < 6; ++f) {
        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
        const int d = f / 2, pa = (d + 1) % 3, qa = (d + 2) % 3;
        const bool u_is_p = axes.u[pa] != 0;  // el eje u de la textura va a lo largo de p (si no, de q)
        for (int slice = 0; slice < kN; ++slice) {
            std::uint64_t* cells = merge.data() + static_cast<std::size_t>((f * kN + slice) * kN * kN);
            for (int q0 = 0; q0 < kN; ++q0) {
                for (int p0 = 0; p0 < kN; ++p0) {
                    const std::uint64_t key = cells[q0 * kN + p0];
                    if (key == 0) continue;
                    // Esquinas 0 BL, 1 BR, 2 TR, 3 TL: igual a lo largo de u si BL = BR y TL = TR.
                    const int k0 = corner_of(key, 0), k1 = corner_of(key, 1), k2 = corner_of(key, 2), k3 = corner_of(key, 3);
                    const bool along_u = k0 == k1 && k3 == k2;
                    const bool along_v = k0 == k3 && k1 == k2;
                    const bool grow_p = u_is_p ? along_u : along_v;
                    const bool grow_q = u_is_p ? along_v : along_u;
                    int w = 1;
                    if (grow_p)
                        while (p0 + w < kN && cells[q0 * kN + p0 + w] == key) ++w;
                    int h = 1;
                    if (grow_q) {
                        for (; q0 + h < kN; ++h) {
                            bool row = true;
                            for (int i = 0; i < w && row; ++i) row = cells[(q0 + h) * kN + p0 + i] == key;
                            if (!row) break;
                        }
                    }
                    for (int j = 0; j < h; ++j)
                        for (int i = 0; i < w; ++i) cells[(q0 + j) * kN + p0 + i] = 0;
                    // Celdas [lo, hi) por eje; la cara en el plano de delante del bloque.
                    int lo[3], hi[3];
                    lo[d] = slice;
                    hi[d] = slice + 1;
                    lo[pa] = p0;
                    hi[pa] = p0 + w;
                    lo[qa] = q0;
                    hi[qa] = q0 + h;
                    const int layer = static_cast<int>(key & 1023u);
                    const auto tint = static_cast<std::uint16_t>((key >> 10) & 0xFFFu);
                    int ao[4], sky[4], torch[4];
                    for (int c = 0; c < 4; ++c) {
                        const int bits = corner_of(key, c);
                        ao[c] = bits & 3;
                        sky[c] = (bits >> 2) & 15;
                        torch[c] = (bits >> 6) & 15;
                    }
                    // Un solo bloque: como siempre (uv de la esquina). Mas grande: uv desde la posicion.
                    const std::uint32_t planar = (w > 1 || h > 1) ? kPlanarBit : 0u;
                    static constexpr int kCorner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
                    static constexpr int kUv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
                    // Diagonal: por la pareja de esquinas mas clara (sin corte visible).
                    const int order[4] = {0, 1, 2, 3};
                    const int rotated[4] = {1, 2, 3, 0};
                    const int* o = (ao[0] + ao[2] < ao[1] + ao[3]) ? rotated : order;
                    for (int k = 0; k < 4; ++k) {
                        const int c = o[k];
                        const int su = kCorner[c][0], sv = kCorner[c][1];
                        int pos[3];
                        for (int i = 0; i < 3; ++i) {
                            if (axes.n[i] != 0) pos[i] = axes.n[i] > 0 ? hi[i] : lo[i];
                            else if (axes.u[i] != 0) pos[i] = axes.u[i] * su > 0 ? hi[i] : lo[i];
                            else pos[i] = axes.v[i] * sv > 0 ? hi[i] : lo[i];
                        }
                        out.push_back(packA(pos[0], pos[1], pos[2], f, kUv[c][0], kUv[c][1], ao[c], false));
                        out.push_back(packB(layer, sky[c], torch[c], tint) | planar);
                    }
                }
            }
        }
    }
    if (tier_flags != nullptr) *tier_flags = flags;
}

}  // namespace cramion::voxel
