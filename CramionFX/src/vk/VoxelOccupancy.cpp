#include "CramionFX/vk/VoxelOccupancy.h"

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

// Ladrillos subidos como mucho por frame (4 KB cada uno): la ventana entera
// (los ~1500 con bloques de un mundo normal) en unos 10 frames.
constexpr std::uint32_t kUploadBudget = 160;
constexpr vk::DeviceSize kHeaderBytes = sizeof(VoxelOccupancy::GpuHeader);
// Por hueco: ladrillo (xyz), cargado (w) y la mascara de sus 64 bloques de 4^3 con algo.
constexpr vk::DeviceSize kTagBytes = 32ull * VoxelOccupancy::kSlots;

std::int32_t floorDiv(std::int32_t a, std::int32_t b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

std::int32_t positiveMod(std::int32_t a, std::int32_t b) {
    const std::int32_t m = a % b;
    return m < 0 ? m + b : m;
}

// Color de la celda en un byte (0 = aire): sRGB en 6 x 7 x 6 niveles.
std::uint8_t encodeColor(const core::Vec3& linear) {
    const auto level = [](float v, float steps) {
        const float s = std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f);
        return static_cast<std::uint32_t>(std::lround(s * steps));
    };
    const std::uint32_t r = level(linear.x, 5.0f);
    const std::uint32_t g = level(linear.y, 6.0f);
    const std::uint32_t b = level(linear.z, 5.0f);
    return static_cast<std::uint8_t>(1u + r + 6u * g + 42u * b);
}

}  // namespace

std::uint64_t VoxelOccupancy::pack(std::int32_t x, std::int32_t y, std::int32_t z) {
    constexpr std::int64_t kBias = 1 << 20;
    constexpr std::uint64_t kMask = (1ull << 21) - 1;
    return ((static_cast<std::uint64_t>(x + kBias) & kMask) << 42) |
           ((static_cast<std::uint64_t>(y + kBias) & kMask) << 21) | (static_cast<std::uint64_t>(z + kBias) & kMask);
}

void VoxelOccupancy::create(const VulkanDevice& device, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = std::max(frames_in_flight, 1u);
}

void VoxelOccupancy::destroy() {
    info_.destroy();
    cells_.destroy();
    for (VulkanBuffer& b : staging_) b.destroy();
    staging_.clear();
    bricks_.clear();
    sections_.clear();
    slots_.clear();
    tags_.clear();
    tags_initialized_ = false;
    pending_slots_ = 0;
    device_ = nullptr;
}

// Los buffers de la GPU, la primera vez que hay bloques (un juego sin voxeles
// no gasta sus ~26 MB).
void VoxelOccupancy::allocate() {
    if (device_ == nullptr || ready()) return;
    const auto usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
    info_.create(*device_, kHeaderBytes + kTagBytes, usage, vk::MemoryPropertyFlagBits::eDeviceLocal,
                 "Voxeles: ocupacion (etiquetas)");
    cells_.create(*device_, static_cast<vk::DeviceSize>(kSlots) * kBrickBytes, usage,
                  vk::MemoryPropertyFlagBits::eDeviceLocal, "Voxeles: ocupacion (celdas)");
    staging_.resize(frames_);
    for (VulkanBuffer& s : staging_) {
        s.create(*device_, kHeaderBytes + kTagBytes + static_cast<vk::DeviceSize>(kUploadBudget) * kBrickBytes,
                 vk::BufferUsageFlagBits::eTransferSrc,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                 "Voxeles: ocupacion (subida)");
    }
    slots_.assign(kSlots, Slot{});
    tags_.assign(kSlots, Tag{});
    tags_initialized_ = false;
    ++revision_;
    std::cout << "[Vulkan] Ocupacion de voxeles para los rayos: " << kDim[0] * kBrick << " x " << kDim[1] * kBrick
              << " x " << kDim[2] * kBrick << " m ("
              << (kHeaderBytes + kTagBytes + static_cast<vk::DeviceSize>(kSlots) * kBrickBytes) / (1024 * 1024)
              << " MB)\n";
}

void VoxelOccupancy::setSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices,
                                std::uint32_t quads) {
    if (vertices == nullptr || quads == 0) {
        removeSection(key);
        return;
    }
    // El ladrillo absoluto de la seccion (su esquina, multiplo de 16 sin el
    // origen flotante).
    const auto brick_of = [&](float world, double shift) {
        return static_cast<std::int32_t>(std::llround((static_cast<double>(world) + shift) / kBrick));
    };
    const std::int32_t bx = brick_of(origin.x, shift_[0]);
    const std::int32_t by = brick_of(origin.y, shift_[1]);
    const std::int32_t bz = brick_of(origin.z, shift_[2]);
    const std::uint64_t packed = pack(bx, by, bz);

    std::vector<std::uint8_t> cells(kBrickBytes, 0);
    bool any = false;
    for (std::uint32_t q = 0; q < quads; ++q) {
        const std::uint32_t* v = vertices + static_cast<std::size_t>(q) * 8;
        const std::uint32_t a0 = v[0];
        const std::uint32_t b0 = v[1];
        const std::uint32_t face = (a0 >> 15u) & 7u;
        // Plantas en cruz y bloques finos (vallas, faroles: b bit 30) no tapan.
        if (face >= 6u) continue;
        bool fine = false;
        std::int32_t lo[3] = {31, 31, 31};
        std::int32_t hi[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t a = v[k * 2];
            const std::uint32_t b = v[k * 2 + 1];
            if (((b >> 30u) & 1u) != 0u) fine = true;
            const std::int32_t p[3] = {static_cast<std::int32_t>(a & 31u), static_cast<std::int32_t>((a >> 5u) & 31u),
                                       static_cast<std::int32_t>((a >> 10u) & 31u)};
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], p[i]);
                hi[i] = std::max(hi[i], p[i]);
            }
        }
        if (fine) continue;
        const std::uint32_t layer = b0 & 1023u;
        LayerInfo info{};
        if (layer < layers_.size()) info = layers_[layer];
        if (!info.opaque) continue;  // hojas, cristal: con huecos
        const core::Vec3 tint{static_cast<float>((b0 >> 18u) & 15u) / 15.0f,
                              static_cast<float>((b0 >> 22u) & 15u) / 15.0f,
                              static_cast<float>((b0 >> 26u) & 15u) / 15.0f};
        const std::uint8_t color =
            encodeColor(core::Vec3{info.albedo.x * tint.x, info.albedo.y * tint.y, info.albedo.z * tint.z});
        // La celda solida esta detras de la cara: +X en x = plano - 1, -X en x = plano.
        const int axis = static_cast<int>(face / 2u);
        const bool positive = (face % 2u) == 0u;
        std::int32_t from[3] = {lo[0], lo[1], lo[2]};
        std::int32_t to[3] = {hi[0], hi[1], hi[2]};
        from[axis] = positive ? lo[axis] - 1 : lo[axis];
        to[axis] = from[axis] + 1;
        bool degenerate = false;
        for (int i = 0; i < 3; ++i) {
            from[i] = std::clamp(from[i], 0, kBrick);
            to[i] = std::clamp(to[i], 0, kBrick);
            if (to[i] <= from[i]) degenerate = true;
        }
        if (degenerate) continue;
        for (std::int32_t z = from[2]; z < to[2]; ++z) {
            for (std::int32_t y = from[1]; y < to[1]; ++y) {
                for (std::int32_t x = from[0]; x < to[0]; ++x) {
                    cells[static_cast<std::size_t>(x + kBrick * (y + kBrick * z))] = color;
                }
            }
        }
        any = true;
    }

    // La seccion pudo cambiar de ladrillo (no deberia): se quita la vieja.
    const auto old = sections_.find(key);
    if (old != sections_.end() && old->second != packed) bricks_.erase(old->second);
    if (!any) {
        sections_.erase(key);
        bricks_.erase(packed);
        return;
    }
    sections_[key] = packed;
    Brick& brick = bricks_[packed];
    // Que bloques de 4^3 tienen alguna celda llena: los shaders saltan los vacios.
    brick.mask = 0;
    for (std::int32_t z = 0; z < kBrick; ++z) {
        for (std::int32_t y = 0; y < kBrick; ++y) {
            for (std::int32_t x = 0; x < kBrick; ++x) {
                if (cells[static_cast<std::size_t>(x + kBrick * (y + kBrick * z))] == 0) continue;
                brick.mask |= 1ull << ((x >> 2) + 4 * ((y >> 2) + 4 * (z >> 2)));
            }
        }
    }
    brick.cells = std::move(cells);
    brick.version = next_version_++;
    if (!ready()) allocate();
}

void VoxelOccupancy::removeSection(std::uint64_t key) {
    const auto it = sections_.find(key);
    if (it == sections_.end()) return;
    bricks_.erase(it->second);
    sections_.erase(it);
}

void VoxelOccupancy::clear() {
    bricks_.clear();
    sections_.clear();
}

void VoxelOccupancy::shiftOrigin(const core::Vec3& offset) {
    // Las secciones pasan a origin - offset: lo absoluto no cambia.
    shift_[0] += offset.x;
    shift_[1] += offset.y;
    shift_[2] += offset.z;
}

void VoxelOccupancy::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                            const core::Vec3& camera_position) {
    if (!ready() || frame >= staging_.size()) return;
    // La ventana: kDim ladrillos alrededor del de la camara; en vertical, mas
    // por encima (lo que tapa el sol y el cielo: techos, montes, islas
    // flotantes a 140-240 m) que por debajo: setUpReach (192 m arriba por defecto; el resto, abajo).
    std::int32_t window_min[3];
    const double camera[3] = {camera_position.x + shift_[0], camera_position.y + shift_[1],
                              camera_position.z + shift_[2]};
    for (int i = 0; i < 3; ++i) {
        const auto cell = static_cast<std::int32_t>(std::floor(camera[i]));
        window_min[i] = floorDiv(cell, kBrick) - (i == 1 ? kDim[1] - 1 - up_bricks_ : kDim[static_cast<std::size_t>(i)] / 2);
    }

    // Lo que deberia haber en cada hueco y lo que hay.
    struct Dirty {
        std::uint32_t slot;
        std::int32_t distance;
        const Brick* brick;
        std::array<std::int32_t, 3> coords;
    };
    std::vector<Dirty> dirty;
    bool tags_changed = !tags_initialized_;
    const std::int32_t cx = window_min[0] + kDim[0] / 2;
    const std::int32_t cy = window_min[1] + kDim[1] - 1 - up_bricks_;  // el ladrillo de la camara
    const std::int32_t cz = window_min[2] + kDim[2] / 2;
    for (std::int32_t sz = 0; sz < kDim[2]; ++sz) {
        const std::int32_t z = window_min[2] + positiveMod(sz - window_min[2], kDim[2]);
        for (std::int32_t sy = 0; sy < kDim[1]; ++sy) {
            const std::int32_t y = window_min[1] + positiveMod(sy - window_min[1], kDim[1]);
            for (std::int32_t sx = 0; sx < kDim[0]; ++sx) {
                const std::int32_t x = window_min[0] + positiveMod(sx - window_min[0], kDim[0]);
                const auto slot = static_cast<std::uint32_t>(sx + kDim[0] * (sy + kDim[1] * sz));
                Slot& s = slots_[slot];
                const auto it = bricks_.find(pack(x, y, z));
                const std::array<std::int32_t, 3> coords{x, y, z};
                if (it == bricks_.end()) {
                    if (s.version != 0) {
                        s.version = 0;
                        tags_[slot].loaded = 0;
                        tags_changed = true;
                    }
                    continue;
                }
                if (s.version == it->second.version && s.brick == coords) continue;
                // Otro ladrillo en el hueco: fuera hasta que llegue el suyo.
                if (s.brick != coords && tags_[slot].loaded != 0) {
                    tags_[slot].loaded = 0;
                    s.version = 0;
                    tags_changed = true;
                }
                const std::int32_t d = std::abs(x - cx) + std::abs(y - cy) * 2 + std::abs(z - cz);
                dirty.push_back({slot, d, &it->second, coords});
            }
        }
    }
    pending_slots_ = static_cast<std::uint32_t>(dirty.size());
    const std::size_t count = std::min<std::size_t>(dirty.size(), kUploadBudget);
    if (dirty.size() > count) {
        std::partial_sort(dirty.begin(), dirty.begin() + static_cast<std::ptrdiff_t>(count), dirty.end(),
                          [](const Dirty& a, const Dirty& b) { return a.distance < b.distance; });
    }

    VulkanBuffer& staging = staging_[frame];
    std::vector<vk::BufferCopy> cell_copies;
    cell_copies.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Dirty& d = dirty[i];
        const vk::DeviceSize source = kHeaderBytes + kTagBytes + static_cast<vk::DeviceSize>(i) * kBrickBytes;
        staging.write(d.brick->cells.data(), kBrickBytes, source);
        cell_copies.push_back({source, static_cast<vk::DeviceSize>(d.slot) * kBrickBytes, kBrickBytes});
        Slot& s = slots_[d.slot];
        s.brick = d.coords;
        s.version = d.brick->version;
        Tag& tag = tags_[d.slot];
        tag.brick[0] = d.coords[0];
        tag.brick[1] = d.coords[1];
        tag.brick[2] = d.coords[2];
        tag.loaded = 1;
        tag.mask[0] = static_cast<std::uint32_t>(d.brick->mask);
        tag.mask[1] = static_cast<std::uint32_t>(d.brick->mask >> 32);
        tags_changed = true;
    }
    pending_slots_ -= static_cast<std::uint32_t>(count);

    // Cabecera: la ventana en celdas absolutas y en el mundo de este frame.
    GpuHeader header{};
    for (int i = 0; i < 3; ++i) {
        const std::int32_t anchor = window_min[i] * kBrick;
        header.anchor[i] = anchor;
        header.anchor_world[i] = static_cast<float>(static_cast<double>(anchor) - shift_[i]);
        header.dims[i] = kDim[static_cast<std::size_t>(i)];
        header.window_min[i] = window_min[i];
    }
    // Diagnostico: CRAMION_VOXEL_RT_OFF=1 deja la ocupacion inactiva (los rayos
    // vuelven a atravesar los bloques, como antes), para comparar.
    static const bool disabled = std::getenv("CRAMION_VOXEL_RT_OFF") != nullptr;
    header.anchor[3] = disabled ? 0 : 1;
    header.dims[3] = static_cast<std::int32_t>(kSlots);
    staging.write(&header, kHeaderBytes, 0);
    if (tags_changed) staging.write(tags_.data(), kTagBytes, kHeaderBytes);
    tags_initialized_ = true;

    // Los frames anteriores pueden estar leyendolos (WAR) y los pases de
    // rayos de este los leen despues.
    const auto barrier = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access, bool with_cells) {
        std::array<vk::BufferMemoryBarrier2, 2> barriers{};
        for (auto& b : barriers) {
            b.srcStageMask = src_stage;
            b.srcAccessMask = src_access;
            b.dstStageMask = dst_stage;
            b.dstAccessMask = dst_access;
            b.size = VK_WHOLE_SIZE;
        }
        barriers[0].buffer = *info_.handle();
        barriers[1].buffer = *cells_.handle();
        vk::DependencyInfo dependency{};
        dependency.setBufferMemoryBarriers(vk::ArrayProxyNoTemporaries<const vk::BufferMemoryBarrier2>(
            with_cells ? 2u : 1u, barriers.data()));
        compat::pipelineBarrier(cmd, dependency);
    };
    const bool with_cells = !cell_copies.empty();
    barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderStorageRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, with_cells);
    const vk::BufferCopy info_copy{0, 0, tags_changed ? kHeaderBytes + kTagBytes : kHeaderBytes};
    cmd.copyBuffer(*staging.handle(), *info_.handle(), info_copy);
    if (with_cells) cmd.copyBuffer(*staging.handle(), *cells_.handle(), cell_copies);
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderStorageRead, with_cells);
}

}  // namespace cramion::gfx
