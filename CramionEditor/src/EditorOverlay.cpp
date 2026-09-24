// Geometria de ayuda en 3D (gizmos, alcance de luces, cajas de decals) que
// dibuja el renderizador con prueba de profundidad (setOverlayGeometry): opaca
// donde se ve y atenuada donde queda dentro o detras de un objeto, como en
// Unity y Unreal. Antes se dibujaba en 2D con ImGui, encima de todo: cuando un
// gizmo atravesaba un objeto no se sabia por donde entraba.
//
// El gizmo de mover/rotar/escalar sigue siendo ImGuizmo para la interaccion
// (zonas de agarre y matematica), pero con sus colores transparentes: ImGui
// descarta las primitivas de alfa 0, asi que no dibuja nada. La geometria de
// aqui reproduce sus medidas (factor de pantalla, ejes de 0.1 a 1, planos de
// 0.5 a 0.8, circulos de rotacion a 1.2) para que lo que se ve coincida con
// lo que se agarra.

#include "EditorApp.h"

#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;
using core::Vec4;

namespace {

constexpr float kTwoPi = 2.0f * core::kPi;

// Colores de los ejes (los de Unity) y del eje bajo el raton / en uso.
constexpr ImU32 kAxisColors[3] = {IM_COL32(232, 62, 62, 255), IM_COL32(124, 224, 60, 255),
                                  IM_COL32(58, 122, 255, 255)};
constexpr ImU32 kHighlight = IM_COL32(255, 222, 60, 255);
constexpr ImU32 kCenterColor = IM_COL32(235, 235, 235, 255);

ImU32 withAlpha(ImU32 color, float alpha) {
    const std::uint32_t a = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (color & 0x00FFFFFFu) | (a << 24);
}

Vec3 column(const Mat4& m, int c) {
    return Vec3{m.m[c][0], m.m[c][1], m.m[c][2]};
}

// Dos vectores perpendiculares a `n` (base del plano de un circulo).
void orthonormalBasis(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 helper = std::abs(n.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    u = core::normalize(core::cross(helper, n));
    v = core::cross(n, u);
}

}  // namespace

// -----------------------------------------------------------------------------
// Primitivas
// -----------------------------------------------------------------------------

void EditorApp::overlayLine(const Vec3& a, const Vec3& b, std::uint32_t color) {
    overlay_.lines.push_back({a, color});
    overlay_.lines.push_back({b, color});
}

void EditorApp::overlayTriangle(const Vec3& a, const Vec3& b, const Vec3& c, std::uint32_t color) {
    overlay_.triangles.push_back({a, color});
    overlay_.triangles.push_back({b, color});
    overlay_.triangles.push_back({c, color});
}

void EditorApp::overlayQuad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                            std::uint32_t color) {
    overlayTriangle(a, b, c, color);
    overlayTriangle(a, c, d, color);
}

// `front_only`: solo la mitad que mira a la camara (los circulos de rotacion,
// como ImGuizmo: la mitad de atras no se puede agarrar).
void EditorApp::overlayCircle(const Vec3& center, const Vec3& u, const Vec3& v, float radius,
                              std::uint32_t color, bool front_only, int segments) {
    const Vec3 to_camera = core::normalize(scene_.camera().position() - center);
    Vec3 previous{};
    bool previous_ok = false;
    for (int i = 0; i <= segments; ++i) {
        const float a = static_cast<float>(i) / static_cast<float>(segments) * kTwoPi;
        const Vec3 offset = u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        const Vec3 p = center + offset;
        const bool ok = !front_only || core::dot(offset, to_camera) >= -0.02f * radius;
        if (ok && previous_ok) {
            overlayLine(previous, p, color);
        }
        previous = p;
        previous_ok = ok;
    }
}

// Cono macizo (punta de flecha): base en `base`, punta en base + dir * length.
void EditorApp::overlayCone(const Vec3& base, const Vec3& direction, float length, float radius,
                            std::uint32_t color) {
    Vec3 u{};
    Vec3 v{};
    const Vec3 axis = core::normalize(direction);
    orthonormalBasis(axis, u, v);
    const Vec3 tip = base + axis * length;
    constexpr int kSides = 16;
    for (int i = 0; i < kSides; ++i) {
        const float a0 = static_cast<float>(i) / kSides * kTwoPi;
        const float a1 = static_cast<float>(i + 1) / kSides * kTwoPi;
        const Vec3 p0 = base + (u * std::cos(a0) + v * std::sin(a0)) * radius;
        const Vec3 p1 = base + (u * std::cos(a1) + v * std::sin(a1)) * radius;
        overlayTriangle(tip, p0, p1, color);
        overlayTriangle(base, p1, p0, color);
    }
}

// Cubo macizo con los ejes dados (unitarios) y medio lado `half`.
void EditorApp::overlayCube(const Vec3& center, const Vec3& x, const Vec3& y, const Vec3& z,
                            float half, std::uint32_t color) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        c[i] = center + x * ((i & 1) ? half : -half) + y * ((i & 2) ? half : -half) +
               z * ((i & 4) ? half : -half);
    }
    // Las seis caras (dos triangulos cada una).
    static constexpr int kFaces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                         {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : kFaces) {
        overlayQuad(c[f[0]], c[f[1]], c[f[2]], c[f[3]], color);
    }
}

// Aristas de una caja: la unitaria [-0.5, 0.5]^3 transformada por `m`.
void EditorApp::overlayBoxEdges(const Mat4& m, std::uint32_t color) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const Vec4 p = m * Vec4{(i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f, 1.0f};
        c[i] = Vec3{p.x, p.y, p.z};
    }
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) {
        overlayLine(c[e[0]], c[e[1]], color);
    }
}

// Disco de cara a la camara (el punto central de los gizmos).
void EditorApp::overlayScreenDisc(const Vec3& center, float radius, std::uint32_t color) {
    const Mat4 view = scene_.camera().view();
    const Vec3 right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
    const Vec3 up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
    constexpr int kSides = 20;
    for (int i = 0; i < kSides; ++i) {
        const float a0 = static_cast<float>(i) / kSides * kTwoPi;
        const float a1 = static_cast<float>(i + 1) / kSides * kTwoPi;
        overlayTriangle(center, center + (right * std::cos(a0) + up * std::sin(a0)) * radius,
                        center + (right * std::cos(a1) + up * std::sin(a1)) * radius, color);
    }
}

// Tamano en el mundo del gizmo en `origin`, igual que el mScreenFactor de
// ImGuizmo: 0.1 de espacio de clip medido sobre el vector derecho de la
// camara (con la correccion de aspecto de ImGuizmo).
float EditorApp::gizmoWorldSize(const Vec3& origin) const {
    const Mat4 view = scene_.camera().view();
    const Vec3 right{view.m[0][0], view.m[1][0], view.m[2][0]};
    const Mat4 view_projection = scene_.camera().projection() * view;
    const Vec4 a = view_projection * Vec4{origin.x, origin.y, origin.z, 1.0f};
    const Vec3 end = origin + right;
    const Vec4 b = view_projection * Vec4{end.x, end.y, end.z, 1.0f};
    if (a.w <= 1e-4f || b.w <= 1e-4f) {
        return 1.0f;
    }
    float dx = b.x / b.w - a.x / a.w;
    float dy = b.y / b.w - a.y / a.w;
    const float ratio = view_w_ / std::max(view_h_, 1.0f);
    if (ratio < 1.0f) {
        dx *= ratio;
    } else {
        dy /= ratio;
    }
    const float length = std::sqrt(dx * dx + dy * dy);
    return length > 1e-6f ? 0.1f / length : 1.0f;
}

// -----------------------------------------------------------------------------
// Gizmo de mover / rotar / escalar
// -----------------------------------------------------------------------------

void EditorApp::drawGizmoGeometry(const Mat4& matrix, bool local, int operation) {
    const Vec3 origin = column(matrix, 3);
    Vec3 axes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    if (local) {
        for (int i = 0; i < 3; ++i) {
            const Vec3 c = column(matrix, i);
            if (core::length(c) > 1e-6f) axes[i] = core::normalize(c);
        }
    }
    const float size = gizmoWorldSize(origin);

    // Que parte esta bajo el raton o se arrastra (la resaltada); mientras se
    // arrastra solo se ve esa, como en ImGuizmo.
    const ImGuizmo::MOVETYPE active = ImGuizmo::GetActiveHandleType();
    const ImGuizmo::MOVETYPE hovered = ImGuizmo::GetHoveredHandleType();
    const bool using_gizmo = active != ImGuizmo::MT_NONE;
    const auto state = [&](ImGuizmo::MOVETYPE type) {
        return type == active || (!using_gizmo && type == hovered);
    };
    const auto visible = [&](ImGuizmo::MOVETYPE type) { return !using_gizmo || type == active; };

    if (operation == 0) {  // --- Mover ---
        for (int i = 0; i < 3; ++i) {
            const auto axis_type = static_cast<ImGuizmo::MOVETYPE>(ImGuizmo::MT_MOVE_X + i);
            if (visible(axis_type)) {
                const ImU32 color = state(axis_type) ? kHighlight : kAxisColors[i];
                overlayLine(origin + axes[i] * (0.1f * size), origin + axes[i] * size, color);
                overlayCone(origin + axes[i] * size, axes[i], 0.2f * size, 0.06f * size, color);
            }
            // Plano perpendicular al eje i (YZ, ZX, XY), del 0.5 al 0.8.
            const auto plane_type = static_cast<ImGuizmo::MOVETYPE>(ImGuizmo::MT_MOVE_YZ + i);
            if (visible(plane_type)) {
                const Vec3 px = axes[(i + 1) % 3] * size;
                const Vec3 py = axes[(i + 2) % 3] * size;
                const Vec3 a = origin + px * 0.5f + py * 0.5f;
                const Vec3 b = origin + px * 0.5f + py * 0.8f;
                const Vec3 c = origin + px * 0.8f + py * 0.8f;
                const Vec3 d = origin + px * 0.8f + py * 0.5f;
                const bool lit = state(plane_type);
                overlayQuad(a, b, c, d, withAlpha(lit ? kHighlight : kAxisColors[i], lit ? 0.6f : 0.35f));
                const ImU32 edge = lit ? kHighlight : kAxisColors[i];
                overlayLine(a, b, edge);
                overlayLine(b, c, edge);
                overlayLine(c, d, edge);
                overlayLine(d, a, edge);
            }
        }
        if (visible(ImGuizmo::MT_MOVE_SCREEN)) {
            overlayScreenDisc(origin, 0.05f * size,
                              state(ImGuizmo::MT_MOVE_SCREEN) ? kHighlight : kCenterColor);
        }
    } else if (operation == 1) {  // --- Rotar ---
        for (int i = 0; i < 3; ++i) {
            const auto type = static_cast<ImGuizmo::MOVETYPE>(ImGuizmo::MT_ROTATE_X + i);
            if (!visible(type)) continue;
            // El circulo de giro alrededor del eje i esta en el plano de los
            // otros dos.
            overlayCircle(origin, axes[(i + 1) % 3], axes[(i + 2) % 3], 1.2f * size,
                          state(type) ? kHighlight : kAxisColors[i], /*front_only=*/!using_gizmo, 96);
        }
        if (visible(ImGuizmo::MT_ROTATE_SCREEN)) {
            // Circulo de la vista: radio de pantalla fijo (0.06 del alto, el
            // de ImGuizmo), en el plano de la camara.
            const Mat4 view = scene_.camera().view();
            const Vec3 right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
            const Vec3 up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            float radius = 1.35f * size;
            if (worldToScreen(origin, x0, y0) && worldToScreen(origin + right * size, x1, y1)) {
                const float pixels_per_unit = std::hypot(x1 - x0, y1 - y0) / size;
                if (pixels_per_unit > 1e-4f) {
                    radius = 0.06f * view_h_ / pixels_per_unit;
                }
            }
            overlayCircle(origin, right, up, radius,
                          state(ImGuizmo::MT_ROTATE_SCREEN) ? kHighlight : IM_COL32(230, 230, 230, 255),
                          false, 64);
        }
    } else {  // --- Escalar ---
        for (int i = 0; i < 3; ++i) {
            const auto type = static_cast<ImGuizmo::MOVETYPE>(ImGuizmo::MT_SCALE_X + i);
            if (!visible(type)) continue;
            const ImU32 color = state(type) ? kHighlight : kAxisColors[i];
            overlayLine(origin + axes[i] * (0.1f * size), origin + axes[i] * size, color);
            overlayCube(origin + axes[i] * size, axes[0], axes[1], axes[2], 0.055f * size, color);
        }
        if (visible(ImGuizmo::MT_SCALE_XYZ)) {
            overlayCube(origin, axes[0], axes[1], axes[2], 0.07f * size,
                        state(ImGuizmo::MT_SCALE_XYZ) ? kHighlight : kCenterColor);
        }
    }
}

// Deja la geometria del frame al renderizador (se dibuja en el siguiente
// drawFrame). Vacia = nada que dibujar.
void EditorApp::flushOverlay() {
    // Las lineas se acercan un poco a la camara (0.2 % de la distancia): las
    // aristas de un collider coinciden con las de su malla y, sin esto,
    // parpadearian medio tapadas por la propia superficie.
    const Vec3 eye = scene_.camera().position();
    for (gfx::OverlayVertex& v : overlay_.lines) {
        v.position = v.position + (eye - v.position) * 0.002f;
    }
    overlay_.line_width = 2.5f;
    overlay_.occluded_alpha = 0.22f;
    renderer_.setOverlayGeometry(overlay_);
    overlay_.clear();
}

}  // namespace cramion::editor
