#include "CramionCore/twod/System2D.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/World.h"  // la definicion de ComponentRegistry::registerComponent<T>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace cramion::twod {

namespace {

std::mutex g_worlds_mutex;
std::unordered_map<const ecs::World*, System2D*>& worlds() {
    static std::unordered_map<const ecs::World*, System2D*> map;
    return map;
}

float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

core::Vec3 linearColor(const core::Vec3& srgb) {
    return core::Vec3{srgbToLinear(srgb.x), srgbToLinear(srgb.y), srgbToLinear(srgb.z)};
}

// Lo que se dibuja, antes de ordenarlo.
struct Batch {
    int layer = 0;
    int order = 0;
    float depth = 0.0f;  // distancia a lo largo de la camara (lo lejano primero)
    std::size_t first = 0;
    std::size_t count = 0;
};

}  // namespace

void register2DComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("SpriteRenderer") != nullptr) return;
    registry.registerComponent<SpriteRenderer>("SpriteRenderer", "Sprite Renderer", "2D");
    registry.registerComponent<SpriteAnimator>("SpriteAnimator", "Sprite Animator", "2D");
    registry.registerComponent<Light2D>("Light2D", "Luz 2D", "2D");
    registry.registerComponent<Tilemap>("Tilemap", "Tilemap", "2D");
    registry.registerComponent<TilemapCollider2D>("TilemapCollider2D", "Tilemap Collider 2D", "2D");
    registry.registerComponent<Rigidbody2D>("Rigidbody2D", "Rigidbody 2D", "2D");
    registry.registerComponent<BoxCollider2D>("BoxCollider2D", "Box Collider 2D", "2D");
    registry.registerComponent<CircleCollider2D>("CircleCollider2D", "Circle Collider 2D", "2D");
    registry.registerComponent<PolygonCollider2D>("PolygonCollider2D", "Polygon Collider 2D", "2D");
    registry.registerComponent<CompositeCollider2D>("CompositeCollider2D", "Composite Collider 2D", "2D");
}

System2D::System2D() : physics_(std::make_unique<Physics2DWorld>()) { physics_->setTilesetLibrary(&tilesets_); }

System2D::~System2D() { stop(); }

void System2D::setAssetsRoot(const std::filesystem::path& assets_root) {
    sprites_.setRoot(assets_root);
    tilesets_.setRoot(assets_root);
    sprites_.clear();
    tilesets_.clear();
}

void System2D::start(ecs::World& world) {
    stop();
    world_ = &world;
    running_ = true;
    physics_->setTilesetLibrary(&tilesets_);
    physics_->start(world);
    for (const entt::entity h : world.registry().view<SpriteAnimator>()) {
        SpriteAnimator& a = world.registry().get<SpriteAnimator>(h);
        a.state = SpriteAnimatorState{};
    }
    std::lock_guard lock(g_worlds_mutex);
    worlds()[&world] = this;
}

void System2D::stop() {
    if (world_ != nullptr) {
        std::lock_guard lock(g_worlds_mutex);
        const auto it = worlds().find(world_);
        if (it != worlds().end() && it->second == this) worlds().erase(it);
    }
    physics_->stop();
    running_ = false;
    world_ = nullptr;
}

System2D* System2D::forWorld(const ecs::World* world) {
    std::lock_guard lock(g_worlds_mutex);
    const auto it = worlds().find(world);
    return it != worlds().end() ? it->second : nullptr;
}

void System2D::advanceAnimators(ecs::World& world, float dt, bool play) {
    for (const entt::entity h : world.registry().view<SpriteAnimator>()) {
        const ecs::Entity e = world.wrap(h);
        SpriteAnimator& a = e.get<SpriteAnimator>();
        if (!play && !a.preview_in_editor) continue;
        if (!e.activeInHierarchy() || a.clips.empty()) continue;
        SpriteRenderer* renderer = e.tryGet<SpriteRenderer>();
        if (renderer == nullptr) continue;
        if (a.state.clip.empty() || a.findClip(a.state.clip) == nullptr) {
            a.state.clip = a.default_clip.empty() || a.findClip(a.default_clip) == nullptr ? a.clips.front().name : a.default_clip;
            a.state.time = 0.0f;
            a.state.finished = false;
        }
        const SpriteClip* clip = a.findClip(a.state.clip);
        if (clip == nullptr) continue;
        const std::vector<int> frames = parseFrameList(clip->frames);
        if (frames.empty()) continue;
        if (a.playing && !a.state.finished) a.state.time += dt * std::max(a.speed, 0.0f);
        const int count = static_cast<int>(frames.size());
        int index = static_cast<int>(std::floor(a.state.time * std::max(clip->fps, 0.01f)));
        if (clip->loop) {
            index = ((index % count) + count) % count;
        } else if (index >= count) {
            index = count - 1;
            a.state.finished = true;
        }
        renderer->frame = frames[static_cast<std::size_t>(std::max(index, 0))];
    }
}

int System2D::update(ecs::World& world, float dt, Mode mode) {
    advanceAnimators(world, dt, mode == Mode::Play);
    if (mode == Mode::Play && running_) return physics_->update(world, dt);
    return 0;
}

void System2D::step(ecs::World& world) {
    advanceAnimators(world, physics_->fixed_step, true);
    if (running_) physics_->step(world);
}

std::vector<Physics2DEvent> System2D::takeEvents() { return physics_->takeEvents(); }

bool System2D::spriteBounds(ecs::Entity e, core::Vec2& min, core::Vec2& max) {
    if (!e.valid() || !e.has<SpriteRenderer>()) return false;
    const SpriteRenderer& r = e.get<SpriteRenderer>();
    if (r.draw_mode == SpriteDrawMode::Tiled) {
        min = core::Vec2{-r.size.x * 0.5f, -r.size.y * 0.5f};
        max = core::Vec2{r.size.x * 0.5f, r.size.y * 0.5f};
        return true;
    }
    const SpriteSheet* sheet = r.sprite.empty() ? nullptr : sprites_.get(r.sprite);
    if (sheet == nullptr) {
        min = core::Vec2{-0.5f, -0.5f};
        max = core::Vec2{0.5f, 0.5f};
        return false;
    }
    const SpriteFrame f = sheet->frame(r.frame);
    const float ppu = std::max(sheet->pixels_per_unit, 0.001f);
    const float w = static_cast<float>(f.w) / ppu;
    const float h = static_cast<float>(f.h) / ppu;
    min = core::Vec2{-f.pivot.x * w, -f.pivot.y * h};
    max = core::Vec2{min.x + w, min.y + h};
    return true;
}

gfx::SpriteDrawList System2D::drawList(ecs::World& world, const core::Vec3& camera_position,
                                       const core::Vec3& camera_forward) {
    gfx::SpriteDrawList list;
    // Por la ruta relativa (como esta en el componente) y el filtro: la ruta
    // absoluta (y su texto) solo para las texturas nuevas. Antes se montaba
    // para cada sprite en cada frame (varios ms con unos miles).
    std::unordered_map<std::string, std::uint32_t> texture_index[2];
    const auto textureFor = [&](const std::string& relative, bool point) -> std::uint32_t {
        auto& index_of = texture_index[point ? 1 : 0];
        if (const auto it = index_of.find(relative); it != index_of.end()) return it->second;
        const std::uint32_t index = static_cast<std::uint32_t>(list.textures.size());
        list.textures.push_back(gfx::SpriteTexture{sprites_.absolute(relative), point});
        index_of.emplace(relative, index);
        return index;
    };
    const core::Vec3 forward = core::length(camera_forward) > 1e-6f ? core::normalize(camera_forward) : core::Vec3{0.0f, 0.0f, -1.0f};
    const auto depthOf = [&](const core::Vec3& p) { return core::dot(p - camera_position, forward); };

    std::vector<gfx::SpriteQuad> quads;
    std::vector<Batch> batches;

    // --- Sprites ---
    for (const entt::entity h : world.registry().view<SpriteRenderer>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const SpriteRenderer& r = e.get<SpriteRenderer>();
        if (r.sprite.empty() || r.opacity <= 0.0f) continue;
        const SpriteSheet* sheet = sprites_.get(r.sprite);
        if (sheet == nullptr || sheet->width <= 0 || sheet->height <= 0) continue;
        const SpriteFrame f = sheet->frame(r.frame);
        if (f.w <= 0 || f.h <= 0) continue;
        const float ppu = std::max(sheet->pixels_per_unit, 0.001f);
        const float w = static_cast<float>(f.w) / ppu;
        const float hh = static_cast<float>(f.h) / ppu;
        float u0 = static_cast<float>(f.x) / static_cast<float>(sheet->width);
        float u1 = static_cast<float>(f.x + f.w) / static_cast<float>(sheet->width);
        float v0 = static_cast<float>(f.y) / static_cast<float>(sheet->height);  // arriba
        float v1 = static_cast<float>(f.y + f.h) / static_cast<float>(sheet->height);  // abajo
        if (r.flip_x) std::swap(u0, u1);
        if (r.flip_y) std::swap(v0, v1);
        const std::uint32_t texture = textureFor(r.sprite, sheet->filter == SpriteFilter::Point);
        const core::Vec3 tint = linearColor(r.color);
        const core::Mat4& m = e.worldMatrix();
        Batch batch;
        batch.layer = sortingLayerIndex(r.sorting_layer);
        batch.order = r.order_in_layer;
        batch.depth = depthOf(e.worldPosition());
        batch.first = quads.size();
        const auto addQuad = [&](float x0, float y0, float x1, float y1, float qu0, float qv0, float qu1, float qv1) {
            gfx::SpriteQuad q;
            q.corners[0] = ecs::transformPoint(m, core::Vec3{x0, y0, 0.0f});
            q.corners[1] = ecs::transformPoint(m, core::Vec3{x1, y0, 0.0f});
            q.corners[2] = ecs::transformPoint(m, core::Vec3{x1, y1, 0.0f});
            q.corners[3] = ecs::transformPoint(m, core::Vec3{x0, y1, 0.0f});
            q.uvs[0] = core::Vec2{qu0, qv1};
            q.uvs[1] = core::Vec2{qu1, qv1};
            q.uvs[2] = core::Vec2{qu1, qv0};
            q.uvs[3] = core::Vec2{qu0, qv0};
            q.color = core::Vec4{tint, std::clamp(r.opacity, 0.0f, 1.0f)};
            q.texture = texture;
            q.alpha_cutoff = r.alpha_cutoff;
            q.lit = r.lit;
            quads.push_back(q);
        };
        if (r.draw_mode == SpriteDrawMode::Tiled && w > 1e-5f && hh > 1e-5f) {
            // Mosaico centrado: el corte se repite; el ultimo se recorta.
            const float sx = std::max(r.size.x, 0.0f);
            const float sy = std::max(r.size.y, 0.0f);
            const float start_x = -sx * 0.5f;
            const float start_y = -sy * 0.5f;
            int made = 0;
            for (float y = 0.0f; y < sy - 1e-5f && made < 4096; y += hh) {
                const float th = std::min(hh, sy - y);
                for (float x = 0.0f; x < sx - 1e-5f && made < 4096; x += w, ++made) {
                    const float tw = std::min(w, sx - x);
                    const float cu1 = u0 + (u1 - u0) * (tw / w);
                    const float cv0 = v1 + (v0 - v1) * (th / hh);  // desde abajo
                    addQuad(start_x + x, start_y + y, start_x + x + tw, start_y + y + th, u0, cv0, cu1, v1);
                }
            }
        } else {
            const float x0 = -f.pivot.x * w;
            const float y0 = -f.pivot.y * hh;
            addQuad(x0, y0, x0 + w, y0 + hh, u0, v0, u1, v1);
        }
        batch.count = quads.size() - batch.first;
        if (batch.count > 0) batches.push_back(batch);
    }

    // --- Tilemaps ---
    for (const entt::entity h : world.registry().view<Tilemap>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        Tilemap& map = e.get<Tilemap>();
        if (map.tileset.empty()) continue;
        const Tileset* tileset = tilesets_.get(map.tileset);
        if (tileset == nullptr || tileset->image.empty() || tileset->image_width <= 0) continue;
        buildTilemapMeshes(map, *tileset);
        if (!map.runtime.ptr) continue;
        const std::uint32_t texture = textureFor(tileset->image, tileset->filter == SpriteFilter::Point);
        const core::Mat4& m = e.worldMatrix();
        const int layer_index = sortingLayerIndex(map.sorting_layer);
        const float depth = depthOf(e.worldPosition());
        for (std::size_t li = 0; li < map.layers.size() && li < map.runtime.ptr->layers.size(); ++li) {
            const TilemapLayer& layer = map.layers[li];
            if (!layer.visible || layer.opacity <= 0.0f) continue;
            Batch batch;
            batch.layer = layer_index;
            batch.order = map.order_in_layer + layer.order;
            batch.depth = depth;
            batch.first = quads.size();
            const core::Vec4 color{linearColor(layer.tint), std::clamp(layer.opacity, 0.0f, 1.0f)};
            for (const auto& [key, chunk] : map.runtime.ptr->layers[li]) {
                for (const gfx::SpriteQuad& local : chunk.quads) {
                    gfx::SpriteQuad q = local;
                    for (core::Vec3& c : q.corners) c = ecs::transformPoint(m, c);
                    q.color = color;
                    q.texture = texture;
                    q.alpha_cutoff = 0.01f;
                    q.lit = map.lit;
                    quads.push_back(q);
                }
            }
            batch.count = quads.size() - batch.first;
            if (batch.count > 0) batches.push_back(batch);
        }
    }

    // Capa, orden y lo lejano primero.
    std::stable_sort(batches.begin(), batches.end(), [](const Batch& a, const Batch& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.order != b.order) return a.order < b.order;
        return a.depth > b.depth;
    });
    list.quads.reserve(quads.size());
    for (const Batch& b : batches) {
        list.quads.insert(list.quads.end(), quads.begin() + static_cast<std::ptrdiff_t>(b.first),
                          quads.begin() + static_cast<std::ptrdiff_t>(b.first + b.count));
    }

    // --- Luces 2D ---
    bool has_global = false;
    core::Vec3 ambient{};
    for (const entt::entity h : world.registry().view<Light2D>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const Light2D& l = e.get<Light2D>();
        const core::Vec3 color = linearColor(l.color);
        if (l.type == Light2DType::Global) {
            has_global = true;
            ambient = ambient + color * l.intensity;
        } else {
            gfx::SpriteLight light;
            light.position = e.worldPosition();
            light.radius = l.radius;
            light.color = color;
            light.intensity = l.intensity;
            light.falloff = l.falloff;
            list.lights.push_back(light);
        }
    }
    list.ambient = has_global ? ambient : (list.lights.empty() ? core::Vec3{1.0f, 1.0f, 1.0f} : core::Vec3{0.15f, 0.15f, 0.18f});
    return list;
}

ecs::Entity System2D::pick(ecs::World& world, const core::Vec3& ray_origin, const core::Vec3& ray_direction) {
    ecs::Entity best;
    int best_layer = -1;
    int best_order = -1 << 30;
    float best_t = 1e30f;
    const auto consider = [&](ecs::Entity e, int layer, int order, float t) {
        const bool better = !best.valid() || layer > best_layer || (layer == best_layer && order > best_order) ||
                            (layer == best_layer && order == best_order && t < best_t);
        if (better) {
            best = e;
            best_layer = layer;
            best_order = order;
            best_t = t;
        }
    };
    const auto localHit = [&](const ecs::Entity& e, core::Vec3& local, float& t) {
        const core::Mat4 inv = core::inverse(e.worldMatrix());
        const core::Vec3 o = ecs::transformPoint(inv, ray_origin);
        const core::Vec3 d = ecs::transformDirection(inv, ray_direction);
        if (std::fabs(d.z) < 1e-8f) return false;
        const float s = -o.z / d.z;
        if (s < 0.0f) return false;
        local = o + d * s;
        t = core::length(ecs::transformPoint(e.worldMatrix(), local) - ray_origin);
        return true;
    };
    for (const entt::entity h : world.registry().view<SpriteRenderer>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        core::Vec2 mn, mx;
        spriteBounds(e, mn, mx);
        core::Vec3 local;
        float t = 0.0f;
        if (!localHit(e, local, t)) continue;
        if (local.x < mn.x || local.x > mx.x || local.y < mn.y || local.y > mx.y) continue;
        const SpriteRenderer& r = e.get<SpriteRenderer>();
        consider(e, sortingLayerIndex(r.sorting_layer), r.order_in_layer, t);
    }
    for (const entt::entity h : world.registry().view<Tilemap>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const Tilemap& map = e.get<Tilemap>();
        core::Vec3 local;
        float t = 0.0f;
        if (!localHit(e, local, t)) continue;
        int cx = 0, cy = 0;
        map.cellAt(local, cx, cy);
        for (std::size_t li = map.layers.size(); li-- > 0;) {
            if (!map.layers[li].visible || map.getTile(cx, cy, static_cast<int>(li)) == 0) continue;
            consider(e, sortingLayerIndex(map.sorting_layer), map.order_in_layer + map.layers[li].order, t);
            break;
        }
    }
    return best;
}

}  // namespace cramion::twod
