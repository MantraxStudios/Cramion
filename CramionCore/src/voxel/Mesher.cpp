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

#include "VoxelInternal.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

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

// Caja [lo, hi] (dieciseisavos, 0..16) dentro de la celda (x, y, z). skip[f]:
// no poner esa cara (tapada por un vecino o por otra pieza).
void emitBox(std::vector<std::uint32_t>& out, int x, int y, int z, const int lo[3], const int hi[3], const bool skip[6],
             const BlockDef& def, int sky, int torch) {
    const int cell[3] = {x, y, z};
    for (int f = 0; f < 6; ++f) {
        if (skip[f]) continue;
        const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
        const std::string& texture = f == 2 ? def.top : (f == 3 ? def.bottom : def.side);
        const std::uint32_t b = packB(textureLayer(texture), sky, torch, kWhite) | kFineBit;
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

// --- Agua (pase del agua: water_voxel.vert) ---
// Caras del agua que se ven: contra el aire, plantas o cristal; entre aguas
// solo el escalon de la mas alta sobre la mas baja (las de dentro no). La cara
// de arriba a 14/16 (como Minecraft) salvo con agua o un bloque encima.
void meshWaterBlock(const SectionSnapshot& s, int x, int y, int z, std::vector<std::uint32_t>& out) {
    const auto block_at = [&](int bx, int by, int bz) { return s.blocks[static_cast<std::size_t>(SectionSnapshot::index(bx, by, bz))]; };
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

}  // namespace

int waterHeight16(int level) {
    static constexpr int kHeights[8] = {14, 12, 11, 9, 8, 6, 5, 3};
    return kHeights[std::clamp(level, 0, 7)];
}

void meshSection(const SectionSnapshot& s, std::vector<std::uint32_t>& out, std::vector<std::uint32_t>* water) {
    out.clear();
    if (water != nullptr) water->clear();
    const auto block_at = [&](int x, int y, int z) { return s.blocks[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))]; };
    const auto light_at = [&](int x, int y, int z) { return s.light[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))]; };
    const auto opaque = [&](int x, int y, int z) { return blockDef(block_at(x, y, z)).opaque; };

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
                    const int layer = textureLayer(def.side);
                    // Dos planos diagonales, cada uno por sus dos caras.
                    const int planes[2][4] = {{0, 0, 1, 1}, {1, 0, 0, 1}};  // (x0, z0, x1, z1)
                    for (const auto& p : planes) {
                        for (int side = 0; side < 2; ++side) {
                            const int ax = side == 0 ? p[0] : p[2], az = side == 0 ? p[1] : p[3];
                            const int bx = side == 0 ? p[2] : p[0], bz = side == 0 ? p[3] : p[1];
                            const std::uint32_t b = packB(layer, sky, torch, tint);
                            out.push_back(packA(x + ax, y, z + az, 6, 0, 1, 3, def.waves));
                            out.push_back(b);
                            out.push_back(packA(x + bx, y, z + bz, 6, 1, 1, 3, def.waves));
                            out.push_back(b);
                            out.push_back(packA(x + bx, y + 1, z + bz, 6, 1, 0, 3, def.waves));
                            out.push_back(b);
                            out.push_back(packA(x + ax, y + 1, z + az, 6, 0, 0, 3, def.waves));
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
                    const auto box = [&](int x0, int y0, int z0, int x1, int y1, int z1, const bool inner_skip[6]) {
                        const int lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
                        bool skip[6];
                        for (int f = 0; f < 6; ++f) {
                            const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                            int k = 0;
                            while (axes.n[k] == 0) ++k;
                            const bool at_edge = axes.n[k] > 0 ? hi[k] == 16 : lo[k] == 0;
                            skip[f] = inner_skip[f] || (at_edge && (covered[f] || same[f]));
                        }
                        emitBox(out, x, y, z, lo, hi, skip, def, sky, torch);
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
                    } else {
                        // Farol: cuerpo y tapa; cadena si hay algo encima, si no un pie.
                        box(5, 2, 5, 11, 10, 11, none);
                        box(6, 10, 6, 10, 12, 10, none);
                        if (block_at(x, y + 1, z) != block::Air) box(7, 12, 7, 9, 16, 9, none);
                        else box(6, 0, 6, 10, 2, 10, none);
                    }
                    continue;
                }

                for (int f = 0; f < 6; ++f) {
                    const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                    const int nx = x + axes.n[0], ny = y + axes.n[1], nz = z + axes.n[2];
                    const BlockId neighbor = block_at(nx, ny, nz);
                    const BlockDef& ndef = blockDef(neighbor);
                    if (ndef.opaque) continue;
                    if (neighbor == id && def.cutout) continue;  // cristal con cristal, hojas con hojas
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
                        // Posicion de la esquina (enteros 0..16 dentro de la seccion).
                        const auto coord = [&](int base, int n, int u, int v) {
                            return base + (n > 0 ? 1 : 0) + (u * su > 0 ? 1 : 0) + (v * sv > 0 ? 1 : 0);
                        };
                        px[c] = coord(x, axes.n[0], axes.u[0], axes.v[0]);
                        py[c] = coord(y, axes.n[1], axes.u[1], axes.v[1]);
                        pz[c] = coord(z, axes.n[2], axes.u[2], axes.v[2]);
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
}

}  // namespace cramion::voxel
