// Dibujo de depuracion desde los scripts (DebugDraw.h).

#include "CramionCore/scripting/DebugDraw.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace cramion::debugdraw {

namespace {

cvar::CVar<bool> g_in_game("debug.draw.InGame", false,
                           "Dibujo de depuracion de los scripts (Debug.drawLine, drawSphere, drawText...) en el juego "
                           "exportado (CramionPlayer). En el Play del editor se ve siempre",
                           cvar::Saved);
cvar::CVar<int> g_max_lines("debug.draw.MaxLines", 65536,
                            "Tope de segmentos vivos del dibujo de depuracion (una esfera son 72); lo que pase se descarta",
                            cvar::Saved, 0, 4000000);
cvar::CVar<int> g_max_texts("debug.draw.MaxTexts", 512, "Tope de textos vivos del dibujo de depuracion (Debug.drawText)",
                            cvar::Saved, 0, 100000);
cvar::CVar<float> g_line_width("debug.draw.LineWidth", 2.0f, "Grosor en pixeles de las lineas de depuracion", cvar::Saved,
                               1.0f, 16.0f);

// Sin dibujarse en este tiempo de mas (ninguna vista lo pinta) se tira.
constexpr float kStaleSeconds = 1.0f;

std::uint8_t channel(float v) {
    if (!(v > 0.0f)) return 0;  // tambien NaN
    if (v >= 1.0f) return 255;
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

bool finite(const core::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void basis(const core::Vec3& n, core::Vec3& u, core::Vec3& v) {
    const core::Vec3 helper = std::abs(n.y) < 0.95f ? core::Vec3{0.0f, 1.0f, 0.0f} : core::Vec3{1.0f, 0.0f, 0.0f};
    u = core::normalize(core::cross(helper, n));
    v = core::cross(n, u);
}

}  // namespace

std::uint32_t packColor(float r, float g, float b, float a) {
    return static_cast<std::uint32_t>(channel(r)) | (static_cast<std::uint32_t>(channel(g)) << 8) |
           (static_cast<std::uint32_t>(channel(b)) << 16) | (static_cast<std::uint32_t>(channel(a)) << 24);
}

bool DebugDraw::enabled() const { return editor_ || g_in_game.get(); }

bool DebugDraw::roomFor(std::size_t segments) {
    const std::size_t limit = static_cast<std::size_t>(std::max(0, g_max_lines.get()));
    if (segments_.size() + segments <= limit) return true;
    dropped_ += segments;
    if (!warned_) {
        warned_ = true;
        std::cerr << "[Debug.draw] Tope de " << limit
                  << " segmentos de depuracion vivos (debug.draw.MaxLines): lo que pasa se descarta. Usa duraciones mas "
                     "cortas o sube el tope."
                  << std::endl;
    }
    return false;
}

void DebugDraw::push(const core::Vec3& a, const core::Vec3& b, const Style& s) {
    const float duration = std::isfinite(s.duration) ? std::max(s.duration, 0.0f) : 0.0f;
    segments_.push_back(Segment{a, b, s.color, duration, 0.0f, s.depth_test, false});
}

void DebugDraw::line(const core::Vec3& a, const core::Vec3& b, const Style& s) {
    if (!enabled() || !finite(a) || !finite(b) || !roomFor(1)) return;
    push(a, b, s);
}

void DebugDraw::ray(const core::Vec3& origin, const core::Vec3& direction, const Style& s) {
    line(origin, origin + direction, s);
}

void DebugDraw::circle(const core::Vec3& center, const core::Vec3& normal, float radius, const Style& s, int segments) {
    if (!enabled() || !finite(center) || !finite(normal) || !std::isfinite(radius)) return;
    const float len = core::length(normal);
    if (len < 1e-6f) return;
    segments = std::clamp(segments, 3, 256);
    if (!roomFor(static_cast<std::size_t>(segments))) return;
    core::Vec3 u, v;
    basis(normal * (1.0f / len), u, v);
    const float r = std::abs(radius);
    core::Vec3 prev = center + u * r;
    for (int i = 1; i <= segments; ++i) {
        const float t = 2.0f * core::kPi * static_cast<float>(i) / static_cast<float>(segments);
        const core::Vec3 p = center + u * (std::cos(t) * r) + v * (std::sin(t) * r);
        push(prev, p, s);
        prev = p;
    }
}

void DebugDraw::sphere(const core::Vec3& center, float radius, const Style& s, int segments) {
    if (!enabled()) return;
    segments = std::clamp(segments, 3, 256);
    if (!roomFor(static_cast<std::size_t>(segments) * 3)) return;
    circle(center, core::Vec3{1.0f, 0.0f, 0.0f}, radius, s, segments);
    circle(center, core::Vec3{0.0f, 1.0f, 0.0f}, radius, s, segments);
    circle(center, core::Vec3{0.0f, 0.0f, 1.0f}, radius, s, segments);
}

void DebugDraw::box(const core::Vec3& center, const core::Vec3& size, const core::Quat& rotation, const Style& s) {
    if (!enabled() || !finite(center) || !finite(size) || !roomFor(12)) return;
    core::Quat q = rotation;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    q = std::isfinite(n) && n > 1e-12f ? core::normalize(q) : core::Quat{};
    const core::Vec3 h = size * 0.5f;
    core::Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const core::Vec3 local{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
        c[i] = center + ecs::quatRotate(q, local);
    }
    // Las 12 aristas: las que cambian un solo bit.
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if ((i & bit) == 0) push(c[i], c[i | bit], s);
        }
    }
}

void DebugDraw::arrow(const core::Vec3& from, const core::Vec3& to, const Style& s, float head) {
    if (!enabled() || !finite(from) || !finite(to)) return;
    const core::Vec3 d = to - from;
    const float len = core::length(d);
    if (len < 1e-6f) return;
    if (!roomFor(5)) return;
    push(from, to, s);
    const core::Vec3 dir = d * (1.0f / len);
    const float h = std::isfinite(head) && head > 0.0f ? std::min(head, len) : std::min(len * 0.25f, 0.5f);
    core::Vec3 u, v;
    basis(dir, u, v);
    const core::Vec3 back = to - dir * h;
    const float w = h * 0.4f;
    push(to, back + u * w, s);
    push(to, back - u * w, s);
    push(to, back + v * w, s);
    push(to, back - v * w, s);
}

void DebugDraw::text(const core::Vec3& position, std::string_view text, const Style& s, float size) {
    if (!enabled() || text.empty() || !finite(position)) return;
    const std::size_t limit = static_cast<std::size_t>(std::max(0, g_max_texts.get()));
    if (texts_.size() >= limit) {
        ++dropped_;
        if (!warned_) {
            warned_ = true;
            std::cerr << "[Debug.draw] Tope de " << limit << " textos de depuracion vivos (debug.draw.MaxTexts): lo que pasa se descarta."
                      << std::endl;
        }
        return;
    }
    const float duration = std::isfinite(s.duration) ? std::max(s.duration, 0.0f) : 0.0f;
    const float px = std::isfinite(size) && size > 0.0f ? std::clamp(size, 4.0f, 256.0f) : 14.0f;
    texts_.push_back(Text{position, std::string(text.substr(0, 256)), s.color, px, duration, 0.0f, s.depth_test, false});
}

void DebugDraw::advance(float dt) {
    if (empty()) return;
    const float step = std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    const auto expired = [step](auto& item) {
        if (item.drawn) {
            item.remaining -= step;
            return item.remaining <= 0.0f;
        }
        item.age += step;
        return item.age > item.remaining + kStaleSeconds;
    };
    segments_.erase(std::remove_if(segments_.begin(), segments_.end(), expired), segments_.end());
    texts_.erase(std::remove_if(texts_.begin(), texts_.end(), expired), texts_.end());
}

void DebugDraw::build(gfx::OverlayGeometry& out) {
    const float width = g_line_width.get();
    out.line_width = width;
    out.top_line_width = width;
    out.occluded_alpha = 0.0f;  // con prueba de profundidad, lo tapado no se ve (como Unity)
    for (Segment& s : segments_) {
        std::vector<gfx::OverlayVertex>& to = s.depth_test ? out.lines : out.top_lines;
        to.push_back(gfx::OverlayVertex{s.a, s.color});
        to.push_back(gfx::OverlayVertex{s.b, s.color});
        s.drawn = true;
    }
    for (Text& t : texts_) {
        out.texts.push_back(gfx::OverlayText{t.position, t.color, t.size, !t.depth_test, t.text});
        t.drawn = true;
    }
}

void DebugDraw::clear() {
    segments_.clear();
    texts_.clear();
    dropped_ = 0;
    warned_ = false;
}

DebugDraw& shared() {
    static DebugDraw draw;
    return draw;
}

}  // namespace cramion::debugdraw
