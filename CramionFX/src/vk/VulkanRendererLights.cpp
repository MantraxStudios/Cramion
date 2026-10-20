// VulkanRenderer: luces locales por clusteres (light_clusters.glsl). Aparte
// para no engordar VulkanRenderer.cpp.
//
// Las 32 puntuales y 8 focos principales (los mas cercanos que tocan la vista)
// siguen en GpuLights con sus teselas, sombras por rayos y niebla. Las demas
// que tocan la vista van aqui sin tope: la vista se parte en columnas x filas
// de pantalla x rebanadas de profundidad exponenciales y cada cluster guarda
// la lista de las luces cuya esfera puede llegar a el. Un pixel solo recorre
// las de su cluster (lighting.frag y glass.frag).
//
// Lo reparte la CPU: las que se ven son unos cientos (Ashbound tiene ~1300 en
// el mapa) y cada una cae en unos pocos clusteres; el coste es una pasada
// para contar y otra para rellenar, sin asignar memoria (los vectores se
// reutilizan). Los buffers son por frame en vuelo y crecen al doble cuando no
// caben: la fence de ese frame ya se espero (como los uniform buffers), asi
// que no hace falta ni waitIdle ni cola de retirada.

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace cramion::gfx {

namespace {

using core::Mat4;
using core::Vec3;
using core::Vec4;

// Capacidad inicial de cada frame: luces y entradas de las listas.
constexpr std::uint32_t kInitialClusterLights = 256;
constexpr std::uint32_t kInitialClusterIndices = 8192;
// Luces de un cluster como mucho. Van ordenadas por cercania (RenderSync), asi
// que si un cluster lejano junta mas, se quedan las mas cercanas a la camara.
constexpr std::uint32_t kMaxLightsPerCluster = 255;

constexpr vk::MemoryPropertyFlags kHostVisible =
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

vk::DeviceSize lightBytes(std::size_t lights) {
    return sizeof(GpuClusterHeader) + sizeof(GpuClusterLight) * std::max<std::size_t>(lights, 1);
}

vk::DeviceSize gridBytes(std::size_t items) {
    return sizeof(std::uint32_t) * std::max<std::size_t>(items, 1);
}

}  // namespace

void VulkanRenderer::createLightClusterBuffers() {
    cluster_light_buffers_.resize(kMaxFramesInFlight);
    cluster_grid_buffers_.resize(kMaxFramesInFlight);
    const GpuClusterHeader empty{};
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        cluster_light_buffers_[i].create(device_, lightBytes(kInitialClusterLights),
                                         vk::BufferUsageFlagBits::eStorageBuffer, kHostVisible);
        cluster_light_buffers_[i].write(&empty, sizeof(empty));
        cluster_grid_buffers_[i].create(device_, gridBytes(2 * kLightClusterCount + kInitialClusterIndices),
                                        vk::BufferUsageFlagBits::eStorageBuffer, kHostVisible);
    }
    cluster_lights_.reserve(kInitialClusterLights);
    cluster_spans_.reserve(kInitialClusterLights);
    cluster_items_.reserve(2 * kLightClusterCount + kInitialClusterIndices);
}

void VulkanRenderer::destroyLightClusterBuffers() {
    for (VulkanBuffer& buffer : cluster_light_buffers_) buffer.destroy();
    for (VulkanBuffer& buffer : cluster_grid_buffers_) buffer.destroy();
    cluster_light_buffers_.clear();
    cluster_grid_buffers_.clear();
}

void VulkanRenderer::writeLightClusterDescriptors(std::uint32_t frame_index) {
    if (frame_index >= cluster_light_buffers_.size() || !cluster_light_buffers_[frame_index].isValid() ||
        !cluster_grid_buffers_[frame_index].isValid()) {
        return;
    }
    const vk::DescriptorBufferInfo lights_info{*cluster_light_buffers_[frame_index].handle(), 0, VK_WHOLE_SIZE};
    const vk::DescriptorBufferInfo grid_info{*cluster_grid_buffers_[frame_index].handle(), 0, VK_WHOLE_SIZE};
    std::array<vk::WriteDescriptorSet, 4> writes{};
    std::uint32_t count = 0;
    const auto add = [&](const vk::raii::DescriptorSet& set, std::uint32_t binding, const vk::DescriptorBufferInfo& info) {
        vk::WriteDescriptorSet& write = writes[count++];
        write.dstSet = *set;
        write.dstBinding = binding;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(info);
    };
    if (frame_index < lighting_sets_.size()) {
        add(lighting_sets_[frame_index], 29, lights_info);
        add(lighting_sets_[frame_index], 30, grid_info);
    }
    // El vidrio, salvo en el modo compatible (su set se queda en 7 bindings).
    if (frame_index < glass_sets_.size() && SkinnedPass::glassBindingCount() > SkinnedPass::kGlassClusterBinding) {
        add(glass_sets_[frame_index], SkinnedPass::kGlassClusterBinding, lights_info);
        add(glass_sets_[frame_index], SkinnedPass::kGlassClusterBinding + 1, grid_info);
    }
    if (count > 0) {
        device_.handle().updateDescriptorSets(vk::ArrayProxy<const vk::WriteDescriptorSet>(count, writes.data()),
                                              nullptr);
    }
}

void VulkanRenderer::buildLightClusters(const Mat4& view_projection, const scene::Camera& camera, bool screen_grid,
                                        std::uint32_t frame_index) {
    if (frame_index >= cluster_light_buffers_.size() || !cluster_light_buffers_[frame_index].isValid()) return;
    const std::size_t light_count = cluster_lights_.size();
    if (!isolated()) cluster_light_count_ = static_cast<std::uint32_t>(light_count);

    GpuClusterHeader header{};
    if (light_count == 0) {
        // Sin luces por clusteres: solo la cabecera (el shader no lee mas).
        cluster_light_buffers_[frame_index].write(&header, sizeof(header));
        return;
    }

    const std::uint32_t columns = screen_grid ? kLightClusterColumns : 1u;
    const std::uint32_t rows = screen_grid ? kLightClusterRows : 1u;
    const std::uint32_t slices = kLightClusterSlices;
    const std::uint32_t clusters = columns * rows * slices;
    const float width = static_cast<float>(std::max(render_extent_.width, 1u));
    const float height = static_cast<float>(std::max(render_extent_.height, 1u));
    // Pixeles de una tesela (redondeados hacia arriba: las ultimas cubren el borde).
    const float tile_w = screen_grid ? std::ceil(width / static_cast<float>(columns)) : width;
    const float tile_h = screen_grid ? std::ceil(height / static_cast<float>(rows)) : height;

    // --- Rebanadas de profundidad (a lo largo del eje de la camara) ---
    // Exponenciales entre `z_near` y la luz mas lejana (cada rebanada un tanto
    // por ciento mas gruesa que la anterior): lo cercano, que ocupa media
    // pantalla, en rebanadas finas.
    const Vec3 axis = core::normalize(camera.forward());
    const float eye_depth = core::dot(axis, camera.position());
    const float z_near = std::max(camera.nearPlane(), 0.1f);
    float z_far = z_near * 4.0f;
    for (const GpuClusterLight& light : cluster_lights_) {
        const Vec3 p{light.position_range.x, light.position_range.y, light.position_range.z};
        z_far = std::max(z_far, core::dot(axis, p) - eye_depth + light.position_range.w);
    }
    z_far = std::min(z_far, std::max(camera.farPlane(), z_near * 4.0f));
    const float slice_scale = static_cast<float>(slices) / std::log2(z_far / z_near);
    const float slice_bias = -std::log2(z_near) * slice_scale;
    const auto slice_of = [&](float depth) {
        const float s = std::floor(std::log2(std::max(depth, 1e-4f)) * slice_scale + slice_bias);
        return static_cast<std::uint32_t>(std::clamp(s, 0.0f, static_cast<float>(slices - 1)));
    };

    // --- 1) Que clusteres toca cada luz (y cuantas luces caen en cada uno) ---
    // En pantalla, la caja de su esfera proyectada con 2 pixeles de margen
    // (como las teselas); si la camara esta dentro de la caja, toda la
    // pantalla. En profundidad, de la rebanada de su punto mas cercano a la
    // del mas lejano.
    cluster_spans_.resize(light_count);
    cluster_items_.assign(2 * static_cast<std::size_t>(clusters), 0u);
    std::uint32_t* const ranges = cluster_items_.data();
    for (std::size_t i = 0; i < light_count; ++i) {
        const Vec4& sphere = cluster_lights_[i].position_range;
        const float r = sphere.w;
        ClusterSpan& span = cluster_spans_[i];
        const float center_depth = sphere.x * axis.x + sphere.y * axis.y + sphere.z * axis.z - eye_depth;
        span.s0 = static_cast<std::uint16_t>(slice_of(center_depth - r));
        span.s1 = static_cast<std::uint16_t>(slice_of(center_depth + r));
        span.c0 = span.r0 = 0;
        span.c1 = static_cast<std::uint16_t>(columns - 1);
        span.r1 = static_cast<std::uint16_t>(rows - 1);
        if (screen_grid) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            bool everywhere = false;
            for (int corner = 0; corner < 8; ++corner) {
                const Vec4 p{sphere.x + ((corner & 1) ? r : -r), sphere.y + ((corner & 2) ? r : -r),
                             sphere.z + ((corner & 4) ? r : -r), 1.0f};
                const Vec4 clip = view_projection * p;
                if (clip.w < 0.01f) {
                    everywhere = true;
                    break;
                }
                const float px = (clip.x / clip.w * 0.5f + 0.5f) * width;
                const float py = (clip.y / clip.w * 0.5f + 0.5f) * height;
                x0 = std::min(x0, px);
                x1 = std::max(x1, px);
                y0 = std::min(y0, py);
                y1 = std::max(y1, py);
            }
            if (!everywhere) {
                // Entera fuera de la pantalla (por un lado): no toca ninguno
                // (s0 > s1: las dos pasadas la saltan).
                if (x1 + 2.0f < 0.0f || y1 + 2.0f < 0.0f || x0 - 2.0f > width || y0 - 2.0f > height) {
                    span.s0 = 1;
                    span.s1 = 0;
                    continue;
                }
                const int last_column = static_cast<int>(columns) - 1;
                const int last_row = static_cast<int>(rows) - 1;
                const int c0 = std::clamp(static_cast<int>(std::floor((x0 - 2.0f) / tile_w)), 0, last_column);
                const int c1 = std::clamp(static_cast<int>(std::floor((x1 + 2.0f) / tile_w)), 0, last_column);
                const int r0 = std::clamp(static_cast<int>(std::floor((y0 - 2.0f) / tile_h)), 0, last_row);
                const int r1 = std::clamp(static_cast<int>(std::floor((y1 + 2.0f) / tile_h)), 0, last_row);
                span.c0 = static_cast<std::uint16_t>(c0);
                span.c1 = static_cast<std::uint16_t>(c1);
                span.r0 = static_cast<std::uint16_t>(r0);
                span.r1 = static_cast<std::uint16_t>(r1);
            }
        }
        for (std::uint32_t s = span.s0; s <= span.s1; ++s) {
            for (std::uint32_t row = span.r0; row <= span.r1; ++row) {
                std::uint32_t* counts = ranges + 2 * ((s * rows + row) * columns + span.c0) + 1;
                for (std::uint32_t column = span.c0; column <= span.c1; ++column, counts += 2) {
                    *counts = std::min(*counts + 1, kMaxLightsPerCluster);
                }
            }
        }
    }

    // --- 2) Donde empieza la lista de cada cluster ---
    std::uint32_t total = 0;
    for (std::uint32_t c = 0; c < clusters; ++c) {
        ranges[2 * c] = total;
        total += ranges[2 * c + 1];
        ranges[2 * c + 1] = 0;  // la pasada 3 lo vuelve a contar al rellenar
    }

    // --- 3) Los indices de las luces de cada cluster (en el orden de la lista) ---
    const std::size_t first_index = 2 * static_cast<std::size_t>(clusters);
    cluster_items_.resize(first_index + total);
    std::uint32_t* const items = cluster_items_.data();  // (despues del resize: puede mover la memoria)
    for (std::size_t i = 0; i < light_count; ++i) {
        const ClusterSpan& span = cluster_spans_[i];
        for (std::uint32_t s = span.s0; s <= span.s1; ++s) {
            for (std::uint32_t row = span.r0; row <= span.r1; ++row) {
                std::uint32_t* range = items + 2 * ((s * rows + row) * columns + span.c0);
                for (std::uint32_t column = span.c0; column <= span.c1; ++column, range += 2) {
                    if (range[1] >= kMaxLightsPerCluster) continue;
                    items[first_index + range[0] + range[1]++] = static_cast<std::uint32_t>(i);
                }
            }
        }
    }

    header.grid[0] = columns;
    header.grid[1] = rows;
    header.grid[2] = slices;
    header.grid[3] = static_cast<std::uint32_t>(light_count);
    header.tile = Vec4{screen_grid ? 1.0f / tile_w : 0.0f, screen_grid ? 1.0f / tile_h : 0.0f, slice_scale, slice_bias};
    header.view_axis = Vec4{axis.x, axis.y, axis.z, eye_depth};

    // --- Subida (crecen al doble si no caben) ---
    VulkanBuffer& light_buffer = cluster_light_buffers_[frame_index];
    VulkanBuffer& grid_buffer = cluster_grid_buffers_[frame_index];
    const vk::DeviceSize light_bytes = lightBytes(light_count);
    const vk::DeviceSize grid_bytes = gridBytes(cluster_items_.size());
    bool grown = false;
    if (light_buffer.size() < light_bytes) {
        light_buffer.create(device_, std::max(light_bytes, light_buffer.size() * 2), vk::BufferUsageFlagBits::eStorageBuffer,
                            kHostVisible);
        grown = true;
    }
    if (grid_buffer.size() < grid_bytes) {
        grid_buffer.create(device_, std::max(grid_bytes, grid_buffer.size() * 2), vk::BufferUsageFlagBits::eStorageBuffer,
                           kHostVisible);
        grown = true;
    }
    if (grown) writeLightClusterDescriptors(frame_index);
    light_buffer.write(&header, sizeof(header));
    light_buffer.write(cluster_lights_.data(), sizeof(GpuClusterLight) * light_count, sizeof(header));
    grid_buffer.write(cluster_items_.data(), sizeof(std::uint32_t) * cluster_items_.size());
}

}  // namespace cramion::gfx
