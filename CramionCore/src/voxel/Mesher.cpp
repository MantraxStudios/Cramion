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

#include "VoxelInternal.h"

#include <algorithm>

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

}  // namespace

void meshSection(const SectionSnapshot& s, std::vector<std::uint32_t>& out) {
    out.clear();
    const auto block_at = [&](int x, int y, int z) { return s.blocks[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))]; };
    const auto light_at = [&](int x, int y, int z) { return s.light[static_cast<std::size_t>(SectionSnapshot::index(x, y, z))]; };
    const auto opaque = [&](int x, int y, int z) { return blockDef(block_at(x, y, z)).opaque; };

    for (int y = 0; y < kChunkSize; ++y) {
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                const BlockId id = block_at(x, y, z);
                const BlockDef& def = blockDef(id);
                if (def.shape == BlockShape::Air || def.shape == BlockShape::Liquid) continue;
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

                for (int f = 0; f < 6; ++f) {
                    const FaceAxes& axes = kFaces[static_cast<std::size_t>(f)];
                    const int nx = x + axes.n[0], ny = y + axes.n[1], nz = z + axes.n[2];
                    const BlockId neighbor = block_at(nx, ny, nz);
                    const BlockDef& ndef = blockDef(neighbor);
                    if (ndef.opaque) continue;
                    if (neighbor == id && def.cutout) continue;  // cristal con cristal, hojas con hojas
                    const std::string& texture = f == 2 ? def.top : (f == 3 ? def.bottom : def.side);
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
