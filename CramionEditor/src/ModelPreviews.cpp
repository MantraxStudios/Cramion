#include "ModelPreviews.h"

#include <CramionCore/asset/AssetManager.h>
#include <CramionFX/asset/ImageFile.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;

namespace {

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    const core::Vec4 r = m * core::Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

Vec3 transformDirection(const Mat4& m, const Vec3& d) {
    const core::Vec4 r = m * core::Vec4{d.x, d.y, d.z, 0.0f};
    return Vec3{r.x, r.y, r.z};
}

float toSrgb(float linear) { return std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f); }
float toLinear(float srgb) { return std::pow(std::clamp(srgb, 0.0f, 1.0f), 2.2f); }

struct Vertex {
    Vec3 position;  // en el mundo del modelo
    Vec3 normal;
    core::Vec2 uv;
};

}  // namespace

void ModelPreviews::start(const std::filesystem::path& cache_folder) {
    stop();
    cache_ = cache_folder;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void ModelPreviews::stop() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        queue_.clear();
        queued_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    ready_.clear();
    failed_.clear();
}

void ModelPreviews::invalidate() {
    std::lock_guard lock(mutex_);
    ready_.clear();
    failed_.clear();
}

std::filesystem::path ModelPreviews::pngFor(const Uuid& uuid) const {
    return cache_ / (uuid.toString() + ".png");
}

std::optional<std::filesystem::path> ModelPreviews::preview(const Uuid& uuid, const std::filesystem::path& file,
                                                            const std::string& name) {
    if (cache_.empty()) return std::nullopt;
    const std::filesystem::path png = pngFor(uuid);
    std::lock_guard lock(mutex_);
    if (const auto it = ready_.find(uuid); it != ready_.end() && it->second) return png;
    if (failed_.contains(uuid) || queued_.contains(uuid)) return std::nullopt;
    // Primera vez en esta sesion: mirar si el PNG de la cache sigue valiendo.
    std::error_code error;
    if (std::filesystem::exists(png, error)) {
        const bool fresh = file.empty() ||
                           std::filesystem::last_write_time(png, error) >= std::filesystem::last_write_time(file, error);
        if (fresh) {
            ready_[uuid] = true;
            return png;
        }
    }
    queued_.insert(uuid);
    queue_.push_back(Job{uuid, file, name});
    wake_.notify_one();
    return std::nullopt;
}

void ModelPreviews::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        bool ok = false;
        try {
            const std::shared_ptr<assets::ModelAsset> model = assets::AssetManager::readModel(job.uuid, job.file, job.name);
            asset::ImageRgba8 image;
            ok = model && render(*model, 128, image) && asset::saveImagePng(pngFor(job.uuid), image);
        } catch (const std::exception& e) {
            std::cerr << "[Editor] Miniatura de " << job.name << ": " << e.what() << "\n";
        }
        std::lock_guard lock(mutex_);
        queued_.erase(job.uuid);
        if (ok) {
            ready_[job.uuid] = true;
        } else {
            failed_.insert(job.uuid);
        }
    }
}

// Rasterizador sencillo: triangulos con z-buffer a 2x (se reduce al final
// para suavizar los bordes), normal interpolada, una luz y un relleno,
// color del material por su textura (sin comprimir) en sRGB.
bool ModelPreviews::render(const assets::ModelAsset& model, std::uint32_t size, asset::ImageRgba8& out) {
    // Matriz de cada nodo en el modelo (los padres van antes que los hijos).
    std::vector<Mat4> world(model.nodes.size(), Mat4::identity());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& node = model.nodes[i];
        world[i] = node.parent >= 0 && static_cast<std::size_t>(node.parent) < i ? world[node.parent] * node.local
                                                                                 : node.local;
    }
    std::vector<Mat4> part_matrix(model.parts.size(), Mat4::identity());
    std::vector<bool> placed(model.parts.size(), false);
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const int part = model.nodes[i].part;
        if (part >= 0 && static_cast<std::size_t>(part) < model.parts.size() && !placed[part]) {
            part_matrix[part] = world[i];
            placed[part] = true;
        }
    }

    // Caja de todo (la esfera que la envuelve encuadra la vista).
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 high{-low.x, -low.y, -low.z};
    std::size_t triangles = 0;
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        if (!model.parts[p]) continue;
        for (const asset::SkinnedVertex& v : model.parts[p]->vertices) {
            const Vec3 w = transformPoint(part_matrix[p], v.position);
            low = Vec3{std::min(low.x, w.x), std::min(low.y, w.y), std::min(low.z, w.z)};
            high = Vec3{std::max(high.x, w.x), std::max(high.y, w.y), std::max(high.z, w.z)};
        }
        triangles += model.parts[p]->indices.size() / 3;
    }
    if (triangles == 0 || low.x > high.x) return false;

    const Vec3 center = (low + high) * 0.5f;
    const float radius = std::max(core::length(high - low) * 0.5f, 1e-4f);
    // Camara 3/4 desde arriba a la derecha, como las miniaturas de Unity.
    const Vec3 forward = core::normalize(Vec3{-1.0f, -0.65f, -1.0f});
    const Vec3 right = core::normalize(core::cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 up = core::cross(right, forward);
    const Vec3 light = core::normalize(Vec3{-0.35f, 0.85f, 0.4f});

    // Encuadre: lo que ocupa de verdad en pantalla (no la esfera de la caja).
    float min_x = radius, max_x = -radius, min_y = radius, max_y = -radius;
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        if (!model.parts[p]) continue;
        for (const asset::SkinnedVertex& v : model.parts[p]->vertices) {
            const Vec3 c = transformPoint(part_matrix[p], v.position) - center;
            min_x = std::min(min_x, core::dot(c, right));
            max_x = std::max(max_x, core::dot(c, right));
            min_y = std::min(min_y, core::dot(c, up));
            max_y = std::max(max_y, core::dot(c, up));
        }
    }
    const float extent = std::max({max_x - min_x, max_y - min_y, 1e-4f}) * 0.5f;
    const float mid_x = (min_x + max_x) * 0.5f;
    const float mid_y = (min_y + max_y) * 0.5f;
    const std::uint32_t S = size * 2;
    const float scale = static_cast<float>(S) * 0.5f / (extent * 1.08f);
    std::vector<float> depth(static_cast<std::size_t>(S) * S, std::numeric_limits<float>::max());
    std::vector<float> color(static_cast<std::size_t>(S) * S * 3, 0.0f);
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(S) * S, 0);

    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        if (!model.parts[p]) continue;
        const asset::ModelData& data = *model.parts[p];
        const Mat4& m = part_matrix[p];
        std::vector<Vertex> verts(data.vertices.size());
        std::vector<Vec3> screen(data.vertices.size());
        for (std::size_t i = 0; i < data.vertices.size(); ++i) {
            const asset::SkinnedVertex& v = data.vertices[i];
            verts[i].position = transformPoint(m, v.position);
            verts[i].normal = core::normalize(transformDirection(m, v.normal));
            verts[i].uv = v.uv;
            const Vec3 c = verts[i].position - center;
            screen[i] = Vec3{static_cast<float>(S) * 0.5f + (core::dot(c, right) - mid_x) * scale,
                             static_cast<float>(S) * 0.5f - (core::dot(c, up) - mid_y) * scale, core::dot(c, forward)};
        }
        for (const asset::SubMesh& sub : data.submeshes) {
            const asset::MaterialData* mat =
                sub.material < data.materials.size() ? &data.materials[sub.material] : nullptr;
            const asset::TextureData* tex = nullptr;
            if (mat != nullptr && mat->albedo_texture >= 0 &&
                static_cast<std::size_t>(mat->albedo_texture) < data.textures.size()) {
                const asset::TextureData& t = data.textures[mat->albedo_texture];
                if (t.format == asset::TextureFormat::Rgba8 && t.width > 0 &&
                    t.pixels.size() >= static_cast<std::size_t>(t.width) * t.height * 4) {
                    tex = &t;
                }
            }
            const Vec3 base = mat != nullptr ? Vec3{mat->base_color.x, mat->base_color.y, mat->base_color.z}
                                             : Vec3{0.8f, 0.8f, 0.8f};
            const Vec3 emissive = mat != nullptr ? mat->emissive : Vec3{};
            for (std::uint32_t t = 0; t + 2 < sub.index_count; t += 3) {
                const std::uint32_t i0 = data.indices[sub.first_index + t];
                const std::uint32_t i1 = data.indices[sub.first_index + t + 1];
                const std::uint32_t i2 = data.indices[sub.first_index + t + 2];
                if (i0 >= screen.size() || i1 >= screen.size() || i2 >= screen.size()) continue;
                const Vec3& a = screen[i0];
                const Vec3& b = screen[i1];
                const Vec3& c = screen[i2];
                const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                if (std::abs(area) < 1e-8f) continue;
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
                const int x1 = std::min(static_cast<int>(S) - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
                const int y1 = std::min(static_cast<int>(S) - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
                if (x0 > x1 || y0 > y1) continue;
                // Normal de la cara, para las mallas de una cara vistas por detras.
                const Vec3 face = core::cross(verts[i1].position - verts[i0].position,
                                              verts[i2].position - verts[i0].position);
                const bool back = core::dot(face, forward) > 0.0f;
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const float px = static_cast<float>(x) + 0.5f;
                        const float py = static_cast<float>(y) + 0.5f;
                        float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                        float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                        float w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                        const float z = w0 * a.z + w1 * b.z + w2 * c.z;
                        const std::size_t pixel = static_cast<std::size_t>(y) * S + x;
                        if (z >= depth[pixel]) continue;
                        depth[pixel] = z;
                        covered[pixel] = 1;

                        Vec3 n = verts[i0].normal * w0 + verts[i1].normal * w1 + verts[i2].normal * w2;
                        const float len = core::length(n);
                        n = len > 1e-6f ? n * (1.0f / len) : core::normalize(face);
                        if (back) n = n * -1.0f;
                        if (core::dot(n, forward) > 0.0f) n = n * -1.0f;

                        Vec3 albedo = base;
                        if (tex != nullptr) {
                            const core::Vec2 uv{verts[i0].uv.x * w0 + verts[i1].uv.x * w1 + verts[i2].uv.x * w2,
                                                verts[i0].uv.y * w0 + verts[i1].uv.y * w1 + verts[i2].uv.y * w2};
                            const float u = uv.x - std::floor(uv.x);
                            const float v = uv.y - std::floor(uv.y);
                            const std::uint32_t tx = std::min(static_cast<std::uint32_t>(u * tex->width), tex->width - 1);
                            const std::uint32_t ty = std::min(static_cast<std::uint32_t>(v * tex->height), tex->height - 1);
                            const std::uint8_t* texel = &tex->pixels[(static_cast<std::size_t>(ty) * tex->width + tx) * 4];
                            albedo = Vec3{albedo.x * toLinear(texel[0] / 255.0f), albedo.y * toLinear(texel[1] / 255.0f),
                                          albedo.z * toLinear(texel[2] / 255.0f)};
                        }
                        const float diffuse = std::max(core::dot(n, light), 0.0f);
                        const float fill = 0.28f + 0.12f * std::max(n.y, 0.0f);
                        const Vec3 lit = albedo * (diffuse * 0.95f + fill) + emissive;
                        color[pixel * 3 + 0] = lit.x;
                        color[pixel * 3 + 1] = lit.y;
                        color[pixel * 3 + 2] = lit.z;
                    }
                }
            }
        }
    }

    // 2x2 -> 1, en sRGB; el fondo queda transparente.
    out.width = size;
    out.height = size;
    out.pixels.assign(static_cast<std::size_t>(size) * size * 4, 0);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            Vec3 sum{};
            int count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t pixel = static_cast<std::size_t>(y * 2 + dy) * S + (x * 2 + dx);
                    if (!covered[pixel]) continue;
                    sum = sum + Vec3{color[pixel * 3], color[pixel * 3 + 1], color[pixel * 3 + 2]};
                    ++count;
                }
            }
            if (count == 0) continue;
            sum = sum * (1.0f / static_cast<float>(count));
            std::uint8_t* o = &out.pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            o[0] = static_cast<std::uint8_t>(toSrgb(sum.x) * 255.0f + 0.5f);
            o[1] = static_cast<std::uint8_t>(toSrgb(sum.y) * 255.0f + 0.5f);
            o[2] = static_cast<std::uint8_t>(toSrgb(sum.z) * 255.0f + 0.5f);
            o[3] = static_cast<std::uint8_t>(count * 255 / 4);
        }
    }
    return true;
}

}  // namespace cramion::editor
