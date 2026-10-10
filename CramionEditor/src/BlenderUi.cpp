// Piezas de la interfaz al estilo de Blender 4 (BlenderUi.h).

#include "BlenderUi.h"

#include "Theme.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace cramion::editor::blender {

namespace {

constexpr float kPi = 3.14159265358979f;

// Un punto en coordenadas del icono (-1..1, Y hacia abajo) en la pantalla.
struct Frame {
    ImVec2 c;
    float h;
    ImVec2 p(float x, float y) const { return ImVec2(c.x + x * h, c.y + y * h); }
};

void arc(ImDrawList* d, const Frame& f, float x, float y, float r, float a0, float a1, ImU32 col, float t) {
    d->PathArcTo(f.p(x, y), r * f.h, a0, a1, 24);
    d->PathStroke(col, ImDrawFlags_None, t);
}

void arrowHead(ImDrawList* d, ImVec2 tip, ImVec2 dir, float len, ImU32 col) {
    const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (l < 1e-4f) return;
    dir = ImVec2(dir.x / l, dir.y / l);
    const ImVec2 n(-dir.y, dir.x);
    const ImVec2 base(tip.x - dir.x * len, tip.y - dir.y * len);
    d->AddTriangleFilled(tip, ImVec2(base.x + n.x * len * 0.6f, base.y + n.y * len * 0.6f),
                         ImVec2(base.x - n.x * len * 0.6f, base.y - n.y * len * 0.6f), col);
}

ImU32 scaleAlpha(ImU32 color, float k) {
    const int a = static_cast<int>(((color >> IM_COL32_A_SHIFT) & 0xFF) * k);
    return theme::withAlpha(color, std::clamp(a, 0, 255));
}

// Fondo de un boton plano segun su estado.
ImU32 flatBackground(bool active, bool hovered, bool held) {
    if (active) return held ? theme::kAccentActive : (hovered ? theme::kAccentHover : theme::kAccent);
    if (held) return theme::kBg5;
    if (hovered) return theme::kBg4;
    return 0;
}

}  // namespace

void drawGlyph(ImDrawList* d, Glyph glyph, ImVec2 center, float size, ImU32 col) {
    const Frame f{center, size * 0.5f};
    const float t = std::max(1.0f, size / 13.0f);  // grosor de linea
    const float h = f.h;
    switch (glyph) {
        case Glyph::Select: {
            // Flecha del raton.
            d->AddTriangleFilled(f.p(-0.45f, -0.8f), f.p(-0.45f, 0.45f), f.p(0.42f, 0.12f), col);
            d->AddQuadFilled(f.p(-0.14f, 0.18f), f.p(0.04f, 0.08f), f.p(0.32f, 0.72f), f.p(0.13f, 0.82f), col);
            break;
        }
        case Glyph::Move: {
            const float r = 0.82f;
            d->AddLine(f.p(-r + 0.2f, 0.0f), f.p(r - 0.2f, 0.0f), col, t);
            d->AddLine(f.p(0.0f, -r + 0.2f), f.p(0.0f, r - 0.2f), col, t);
            arrowHead(d, f.p(r, 0.0f), ImVec2(1, 0), h * 0.32f, col);
            arrowHead(d, f.p(-r, 0.0f), ImVec2(-1, 0), h * 0.32f, col);
            arrowHead(d, f.p(0.0f, r), ImVec2(0, 1), h * 0.32f, col);
            arrowHead(d, f.p(0.0f, -r), ImVec2(0, -1), h * 0.32f, col);
            break;
        }
        case Glyph::Rotate: {
            arc(d, f, 0.0f, 0.0f, 0.62f, -kPi * 0.35f, kPi * 1.35f, col, t * 1.2f);
            const float a = -kPi * 0.35f;
            const ImVec2 tip = f.p(0.62f * std::cos(a), 0.62f * std::sin(a));
            arrowHead(d, ImVec2(tip.x + h * 0.08f, tip.y + h * 0.18f), ImVec2(0.3f, -1.0f), h * 0.42f, col);
            d->AddCircleFilled(center, h * 0.12f, col);
            break;
        }
        case Glyph::Scale: {
            d->AddRect(f.p(-0.75f, -0.1f), f.p(0.1f, 0.75f), col, 1.5f, 0, t);
            d->AddLine(f.p(-0.2f, 0.2f), f.p(0.62f, -0.62f), col, t);
            arrowHead(d, f.p(0.78f, -0.78f), ImVec2(1, -1), h * 0.4f, col);
            break;
        }
        case Glyph::Stamp: {
            d->AddCircleFilled(f.p(0.0f, -0.55f), h * 0.26f, col);
            d->AddRectFilled(f.p(-0.12f, -0.4f), f.p(0.12f, 0.05f), col);
            d->AddRectFilled(f.p(-0.6f, 0.05f), f.p(0.6f, 0.4f), col, 2.0f);
            d->AddLine(f.p(-0.75f, 0.65f), f.p(0.75f, 0.65f), col, t);
            break;
        }
        case Glyph::Brush: {
            d->AddLine(f.p(0.75f, -0.75f), f.p(-0.05f, 0.05f), col, t * 1.6f);
            d->AddTriangleFilled(f.p(-0.2f, -0.1f), f.p(0.1f, 0.2f), f.p(-0.7f, 0.75f), col);
            break;
        }
        case Glyph::Modeling: {
            const ImVec2 a = f.p(-0.6f, -0.25f), b = f.p(0.25f, -0.25f), c = f.p(0.25f, 0.6f), e = f.p(-0.6f, 0.6f);
            const ImVec2 o(h * 0.35f, -h * 0.35f);
            d->AddQuad(a, b, c, e, col, t);
            d->AddLine(a, ImVec2(a.x + o.x, a.y + o.y), col, t);
            d->AddLine(b, ImVec2(b.x + o.x, b.y + o.y), col, t);
            d->AddLine(c, ImVec2(c.x + o.x, c.y + o.y), col, t);
            d->AddLine(ImVec2(a.x + o.x, a.y + o.y), ImVec2(b.x + o.x, b.y + o.y), col, t);
            d->AddLine(ImVec2(b.x + o.x, b.y + o.y), ImVec2(c.x + o.x, c.y + o.y), col, t);
            for (const ImVec2& v : {a, b, c, e}) d->AddCircleFilled(v, t * 1.6f, col);
            break;
        }
        case Glyph::View3D: {
            // Cubo en perspectiva isometrica.
            const ImVec2 top = f.p(0.0f, -0.8f), r = f.p(0.72f, -0.4f), rb = f.p(0.72f, 0.42f), bottom = f.p(0.0f, 0.82f),
                         lb = f.p(-0.72f, 0.42f), l = f.p(-0.72f, -0.4f), mid = f.p(0.0f, 0.0f);
            const ImVec2 hex[6] = {top, r, rb, bottom, lb, l};
            d->AddPolyline(hex, 6, col, ImDrawFlags_Closed, t);
            d->AddLine(l, mid, col, t);
            d->AddLine(r, mid, col, t);
            d->AddLine(mid, bottom, col, t);
            break;
        }
        case Glyph::Game: {
            d->AddRect(f.p(-0.85f, -0.4f), f.p(0.85f, 0.45f), col, h * 0.35f, 0, t);
            d->AddLine(f.p(-0.6f, 0.02f), f.p(-0.2f, 0.02f), col, t);
            d->AddLine(f.p(-0.4f, -0.18f), f.p(-0.4f, 0.22f), col, t);
            d->AddCircleFilled(f.p(0.35f, -0.08f), h * 0.1f, col);
            d->AddCircleFilled(f.p(0.55f, 0.12f), h * 0.1f, col);
            break;
        }
        case Glyph::Outliner: {
            for (int i = 0; i < 3; ++i) {
                const float y = -0.55f + 0.55f * static_cast<float>(i);
                const float x = i == 0 ? -0.75f : -0.4f;
                d->AddRectFilled(f.p(x, y - 0.13f), f.p(x + 0.26f, y + 0.13f), col, 1.0f);
                d->AddLine(f.p(x + 0.42f, y), f.p(0.8f, y), col, t);
            }
            d->AddLine(f.p(-0.62f, -0.4f), f.p(-0.62f, 0.55f), scaleAlpha(col, 0.6f), t * 0.8f);
            break;
        }
        case Glyph::Properties: {
            // Tres deslizadores.
            for (int i = 0; i < 3; ++i) {
                const float y = -0.55f + 0.55f * static_cast<float>(i);
                d->AddLine(f.p(-0.8f, y), f.p(0.8f, y), col, t);
                const float x = i == 0 ? 0.35f : (i == 1 ? -0.3f : 0.1f);
                d->AddCircleFilled(f.p(x, y), h * 0.18f, col);
            }
            break;
        }
        case Glyph::FileBrowser: {
            const ImVec2 pts[6] = {f.p(-0.85f, -0.55f), f.p(-0.3f, -0.55f), f.p(-0.15f, -0.35f),
                                   f.p(0.85f, -0.35f), f.p(0.85f, 0.65f), f.p(-0.85f, 0.65f)};
            d->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
            d->AddLine(f.p(-0.85f, -0.15f), f.p(0.85f, -0.15f), col, t);
            break;
        }
        case Glyph::Console:
        case Glyph::Terminal: {
            d->AddRect(f.p(-0.85f, -0.65f), f.p(0.85f, 0.65f), col, 2.0f, 0, t);
            const ImVec2 chev[3] = {f.p(-0.55f, -0.3f), f.p(-0.25f, 0.0f), f.p(-0.55f, 0.3f)};
            d->AddPolyline(chev, 3, col, ImDrawFlags_None, t * 1.2f);
            d->AddLine(f.p(-0.05f, 0.32f), f.p(0.45f, 0.32f), col, t * 1.2f);
            if (glyph == Glyph::Terminal) d->AddLine(f.p(-0.85f, -0.4f), f.p(0.85f, -0.4f), col, t);
            break;
        }
        case Glyph::Statistics: {
            d->AddRectFilled(f.p(-0.75f, 0.1f), f.p(-0.35f, 0.75f), col);
            d->AddRectFilled(f.p(-0.2f, -0.6f), f.p(0.2f, 0.75f), col);
            d->AddRectFilled(f.p(0.35f, -0.2f), f.p(0.75f, 0.75f), col);
            break;
        }
        case Glyph::Render: {
            // Una imagen con el sol y una montana.
            d->AddRect(f.p(-0.85f, -0.65f), f.p(0.85f, 0.65f), col, 2.0f, 0, t);
            d->AddCircleFilled(f.p(0.4f, -0.25f), h * 0.16f, col);
            d->AddTriangleFilled(f.p(-0.7f, 0.5f), f.p(-0.2f, -0.15f), f.p(0.3f, 0.5f), col);
            d->AddTriangleFilled(f.p(0.05f, 0.5f), f.p(0.35f, 0.15f), f.p(0.7f, 0.5f), col);
            break;
        }
        case Glyph::Physics: {
            d->AddCircleFilled(f.p(0.0f, 0.0f), h * 0.32f, col);
            d->AddEllipse(center, ImVec2(h * 0.85f, h * 0.38f), col, -0.45f, 32, t);
            break;
        }
        case Glyph::Timeline: {
            d->AddCircle(center, h * 0.78f, col, 32, t);
            d->AddLine(center, f.p(0.0f, -0.5f), col, t);
            d->AddLine(center, f.p(0.38f, 0.18f), col, t);
            break;
        }
        case Glyph::Animation: {
            // Fotograma clave (rombo) sobre una linea de tiempo.
            const ImVec2 dia[4] = {f.p(0.0f, -0.6f), f.p(0.45f, -0.1f), f.p(0.0f, 0.4f), f.p(-0.45f, -0.1f)};
            d->AddConvexPolyFilled(dia, 4, col);
            d->AddLine(f.p(-0.85f, 0.7f), f.p(0.85f, 0.7f), col, t);
            break;
        }
        case Glyph::World:
        case Glyph::Globe: {
            d->AddCircle(center, h * 0.78f, col, 32, t);
            d->AddEllipse(center, ImVec2(h * 0.34f, h * 0.78f), col, 0.0f, 32, t);
            d->AddLine(f.p(-0.78f, 0.0f), f.p(0.78f, 0.0f), col, t);
            break;
        }
        case Glyph::Zoom:
        case Glyph::Search: {
            d->AddCircle(f.p(-0.15f, -0.15f), h * 0.5f, col, 24, t * 1.2f);
            d->AddLine(f.p(0.22f, 0.22f), f.p(0.75f, 0.75f), col, t * 1.8f);
            if (glyph == Glyph::Zoom) {
                d->AddLine(f.p(-0.4f, -0.15f), f.p(0.1f, -0.15f), col, t);
                d->AddLine(f.p(-0.15f, -0.4f), f.p(-0.15f, 0.1f), col, t);
            }
            break;
        }
        case Glyph::Pan: {
            // Mano: cuatro dedos, la palma y el pulgar.
            for (int i = 0; i < 4; ++i) {
                const float x = -0.42f + 0.26f * static_cast<float>(i);
                const float top = i == 0 || i == 3 ? -0.45f : -0.7f;
                d->AddRectFilled(f.p(x - 0.08f, top), f.p(x + 0.08f, 0.1f), col, h * 0.08f);
            }
            d->AddRectFilled(f.p(-0.5f, -0.05f), f.p(0.42f, 0.62f), col, h * 0.2f);
            d->AddQuadFilled(f.p(-0.5f, 0.05f), f.p(-0.8f, -0.2f), f.p(-0.88f, -0.08f), f.p(-0.5f, 0.35f), col);
            break;
        }
        case Glyph::Camera: {
            d->AddRect(f.p(-0.85f, -0.45f), f.p(0.3f, 0.45f), col, 2.0f, 0, t);
            d->AddTriangle(f.p(0.3f, 0.0f), f.p(0.85f, -0.4f), f.p(0.85f, 0.4f), col, t);
            d->AddCircleFilled(f.p(-0.28f, 0.0f), h * 0.16f, col);
            break;
        }
        case Glyph::Perspective: {
            d->AddQuad(f.p(-0.4f, -0.6f), f.p(0.4f, -0.6f), f.p(0.85f, 0.6f), f.p(-0.85f, 0.6f), col, t);
            d->AddLine(f.p(0.0f, -0.6f), f.p(0.0f, 0.6f), col, t);
            d->AddLine(f.p(-0.6f, 0.0f), f.p(0.6f, 0.0f), col, t);
            break;
        }
        case Glyph::Orthographic:
        case Glyph::Grid: {
            d->AddRect(f.p(-0.7f, -0.7f), f.p(0.7f, 0.7f), col, 0.0f, 0, t);
            d->AddLine(f.p(-0.23f, -0.7f), f.p(-0.23f, 0.7f), col, t);
            d->AddLine(f.p(0.23f, -0.7f), f.p(0.23f, 0.7f), col, t);
            d->AddLine(f.p(-0.7f, -0.23f), f.p(0.7f, -0.23f), col, t);
            d->AddLine(f.p(-0.7f, 0.23f), f.p(0.7f, 0.23f), col, t);
            break;
        }
        case Glyph::Wireframe: {
            d->AddCircle(center, h * 0.78f, col, 32, t);
            d->AddEllipse(center, ImVec2(h * 0.3f, h * 0.78f), col, 0.0f, 24, t);
            d->AddEllipse(center, ImVec2(h * 0.78f, h * 0.3f), col, 0.0f, 24, t);
            break;
        }
        case Glyph::Solid: {
            d->AddCircleFilled(center, h * 0.78f, scaleAlpha(col, 0.55f), 32);
            d->AddCircleFilled(f.p(-0.25f, -0.25f), h * 0.36f, col, 24);
            d->AddCircle(center, h * 0.78f, col, 32, t);
            break;
        }
        case Glyph::Material: {
            d->AddCircle(center, h * 0.78f, col, 32, t);
            d->PathArcTo(center, h * 0.78f, kPi * 0.5f, kPi * 1.5f, 24);
            d->PathFillConvex(col);
            d->AddCircleFilled(f.p(0.3f, -0.3f), h * 0.16f, col);
            break;
        }
        case Glyph::Rendered: {
            d->AddCircleFilled(center, h * 0.78f, col, 32);
            d->AddCircleFilled(f.p(-0.28f, -0.28f), h * 0.2f, IM_COL32(255, 255, 255, 120), 16);
            break;
        }
        case Glyph::Eye: {
            d->PathArcTo(f.p(0.0f, 0.75f), h * 1.05f, -kPi * 0.77f, -kPi * 0.23f, 16);
            d->PathArcTo(f.p(0.0f, -0.75f), h * 1.05f, kPi * 0.23f, kPi * 0.77f, 16);
            d->PathStroke(col, ImDrawFlags_Closed, t);
            d->AddCircleFilled(center, h * 0.24f, col);
            break;
        }
        case Glyph::EyeClosed: {
            d->PathArcTo(f.p(0.0f, -0.75f), h * 1.05f, kPi * 0.23f, kPi * 0.77f, 16);
            d->PathStroke(col, ImDrawFlags_None, t);
            for (int i = -1; i <= 1; ++i) {
                const float x = 0.42f * static_cast<float>(i);
                d->AddLine(f.p(x, 0.28f), f.p(x * 1.3f, 0.55f), col, t);
            }
            break;
        }
        case Glyph::Gizmo: {
            d->AddLine(f.p(-0.5f, 0.5f), f.p(0.7f, 0.5f), col, t);
            d->AddLine(f.p(-0.5f, 0.5f), f.p(-0.5f, -0.7f), col, t);
            d->AddLine(f.p(-0.5f, 0.5f), f.p(0.25f, -0.2f), col, t);
            arrowHead(d, f.p(0.85f, 0.5f), ImVec2(1, 0), h * 0.3f, col);
            arrowHead(d, f.p(-0.5f, -0.85f), ImVec2(0, -1), h * 0.3f, col);
            arrowHead(d, f.p(0.36f, -0.31f), ImVec2(1, -1), h * 0.3f, col);
            break;
        }
        case Glyph::Magnet: {
            d->PathArcTo(f.p(0.0f, 0.05f), h * 0.55f, 0.0f, kPi, 16);
            d->PathStroke(col, ImDrawFlags_None, t * 2.4f);
            d->AddLine(f.p(-0.55f, 0.05f), f.p(-0.55f, -0.55f), col, t * 2.4f);
            d->AddLine(f.p(0.55f, 0.05f), f.p(0.55f, -0.55f), col, t * 2.4f);
            d->AddRectFilled(f.p(-0.75f, -0.85f), f.p(-0.35f, -0.6f), col);
            d->AddRectFilled(f.p(0.35f, -0.85f), f.p(0.75f, -0.6f), col);
            break;
        }
        case Glyph::Pivot: {
            d->AddCircle(center, h * 0.62f, col, 24, t);
            d->AddCircleFilled(center, h * 0.2f, col);
            break;
        }
        case Glyph::Play: {
            d->AddTriangleFilled(f.p(-0.45f, -0.65f), f.p(0.65f, 0.0f), f.p(-0.45f, 0.65f), col);
            break;
        }
        case Glyph::Pause: {
            d->AddRectFilled(f.p(-0.55f, -0.6f), f.p(-0.15f, 0.6f), col, 1.0f);
            d->AddRectFilled(f.p(0.15f, -0.6f), f.p(0.55f, 0.6f), col, 1.0f);
            break;
        }
        case Glyph::Stop: {
            d->AddRectFilled(f.p(-0.55f, -0.55f), f.p(0.55f, 0.55f), col, 1.5f);
            break;
        }
        case Glyph::StepForward: {
            d->AddTriangleFilled(f.p(-0.6f, -0.6f), f.p(0.3f, 0.0f), f.p(-0.6f, 0.6f), col);
            d->AddRectFilled(f.p(0.35f, -0.6f), f.p(0.6f, 0.6f), col);
            break;
        }
        case Glyph::ChevronDown: {
            const ImVec2 pts[3] = {f.p(-0.55f, -0.22f), f.p(0.0f, 0.3f), f.p(0.55f, -0.22f)};
            d->AddPolyline(pts, 3, col, ImDrawFlags_None, t * 1.2f);
            break;
        }
        case Glyph::ChevronRight: {
            const ImVec2 pts[3] = {f.p(-0.22f, -0.55f), f.p(0.3f, 0.0f), f.p(-0.22f, 0.55f)};
            d->AddPolyline(pts, 3, col, ImDrawFlags_None, t * 1.2f);
            break;
        }
        case Glyph::Close: {
            d->AddLine(f.p(-0.5f, -0.5f), f.p(0.5f, 0.5f), col, t * 1.2f);
            d->AddLine(f.p(0.5f, -0.5f), f.p(-0.5f, 0.5f), col, t * 1.2f);
            break;
        }
        case Glyph::Plus: {
            d->AddLine(f.p(-0.6f, 0.0f), f.p(0.6f, 0.0f), col, t * 1.3f);
            d->AddLine(f.p(0.0f, -0.6f), f.p(0.0f, 0.6f), col, t * 1.3f);
            break;
        }
        case Glyph::Object: {
            d->AddRectFilled(f.p(-0.6f, -0.6f), f.p(0.6f, 0.6f), col, h * 0.15f);
            break;
        }
        case Glyph::Light: {
            d->AddCircle(f.p(0.0f, -0.2f), h * 0.5f, col, 24, t);
            d->AddLine(f.p(-0.25f, 0.45f), f.p(0.25f, 0.45f), col, t);
            d->AddLine(f.p(-0.18f, 0.65f), f.p(0.18f, 0.65f), col, t);
            break;
        }
        case Glyph::Count: break;
    }
}

bool iconButton(const char* id, Glyph glyph, bool active, const char* tooltip, float size) {
    const float side = size > 0.0f ? size : ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + side, min.y + side);
    if (const ImU32 bg = flatBackground(active, hovered, held); bg != 0) d->AddRectFilled(min, max, bg, 4.0f);
    const ImU32 color = active || hovered ? IM_COL32(255, 255, 255, 255) : theme::kText;
    drawGlyph(d, glyph, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), side * 0.6f, color);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

bool editorTypeButton(const char* id, Glyph glyph, const char* tooltip) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 size(h * 1.55f, h);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    d->AddRectFilled(min, max, held ? theme::kBg5 : (hovered ? theme::kBg4 : theme::kBg2), 4.0f);
    const ImU32 color = hovered ? IM_COL32(255, 255, 255, 255) : theme::kText;
    drawGlyph(d, glyph, ImVec2(min.x + h * 0.5f, min.y + h * 0.5f), h * 0.62f, color);
    drawGlyph(d, Glyph::ChevronDown, ImVec2(min.x + h * 1.17f, min.y + h * 0.52f), h * 0.36f, theme::kTextDim);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

int segmented(const char* id, const char* const* labels, const Glyph* glyphs, int count, int active,
              const char* const* tooltips) {
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    const float pad = ImGui::GetStyle().FramePadding.x;
    int clicked = -1;
    ImDrawList* d = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; ++i) {
        float w = h;
        if (labels != nullptr && labels[i] != nullptr) {
            w = ImGui::CalcTextSize(labels[i]).x + pad * 2.0f + (glyphs != nullptr ? h * 0.8f : 0.0f);
        }
        if (i > 0) ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(w, h))) clicked = i;
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const bool on = i == active;
        const ImVec2 max(min.x + w, min.y + h);
        ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
        if (count == 1) corners = ImDrawFlags_RoundCornersAll;
        else if (i == 0) corners = ImDrawFlags_RoundCornersLeft;
        else if (i == count - 1) corners = ImDrawFlags_RoundCornersRight;
        const ImU32 bg = on ? (hovered ? theme::kAccentHover : theme::kAccent)
                            : (held ? theme::kBg5 : (hovered ? theme::kBg4 : theme::kBg3));
        d->AddRectFilled(min, max, bg, 4.0f, corners);
        if (i > 0) d->AddLine(ImVec2(min.x, min.y + 1.0f), ImVec2(min.x, max.y - 1.0f), theme::kBorder);
        const ImU32 text = on || hovered ? IM_COL32(255, 255, 255, 255) : theme::kText;
        float x = min.x + pad;
        if (glyphs != nullptr) {
            const float gx = labels != nullptr && labels[i] != nullptr ? x + h * 0.32f : (min.x + max.x) * 0.5f;
            drawGlyph(d, glyphs[i], ImVec2(gx, (min.y + max.y) * 0.5f), h * 0.6f, text);
            x += h * 0.8f;
        }
        if (labels != nullptr && labels[i] != nullptr) {
            d->AddText(ImVec2(x, min.y + (h - ImGui::GetFontSize()) * 0.5f), text, labels[i]);
        }
        if (tooltips != nullptr && tooltips[i] != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", tooltips[i]);
        }
    }
    ImGui::PopID();
    return clicked;
}

bool toolButton(const char* id, Glyph glyph, bool active, const char* tooltip, float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size, min.y + size);
    ImU32 bg = IM_COL32(67, 67, 67, 235);  // #434343, el de los botones del estante de Blender
    if (active) bg = held ? theme::kAccentActive : (hovered ? theme::kAccentHover : theme::kAccent);
    else if (held) bg = theme::kBg5;
    else if (hovered) bg = theme::kBg4;
    d->AddRectFilled(min, max, bg, size * 0.16f);
    drawGlyph(d, glyph, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), size * 0.56f,
              active || hovered ? IM_COL32(255, 255, 255, 255) : theme::kText);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

void toolShelfBackground(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    draw->AddRectFilled(min, max, IM_COL32(29, 29, 29, 150), 6.0f);
}

NavigationResult navigationGizmo(const char* id, ImVec2 center, float radius, const float right[3], const float up[3],
                                 const float forward[3]) {
    NavigationResult result;
    const ImVec2 min(center.x - radius, center.y - radius);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton(id, ImVec2(radius * 2.0f, radius * 2.0f));
    const bool hovered_box = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float dx = mouse.x - center.x;
    const float dy = mouse.y - center.y;
    result.hovered = (hovered_box || active) && dx * dx + dy * dy <= radius * radius;
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (result.hovered || active) d->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 28), 48);

    struct Axis {
        int index;
        ImVec2 screen;
        float depth;  // > 0: hacia quien mira
    };
    std::array<Axis, 6> axes{};
    const float ball = radius * 0.24f;
    const float reach = radius - ball - 1.0f;
    for (int i = 0; i < 6; ++i) {
        float a[3] = {0.0f, 0.0f, 0.0f};
        a[i % 3] = i < 3 ? 1.0f : -1.0f;
        const float sx = a[0] * right[0] + a[1] * right[1] + a[2] * right[2];
        const float sy = -(a[0] * up[0] + a[1] * up[1] + a[2] * up[2]);
        const float sz = -(a[0] * forward[0] + a[1] * forward[1] + a[2] * forward[2]);
        axes[static_cast<std::size_t>(i)] = Axis{i, ImVec2(center.x + sx * reach, center.y + sy * reach), sz};
    }
    std::array<Axis, 6> order = axes;
    std::sort(order.begin(), order.end(), [](const Axis& a, const Axis& b) { return a.depth < b.depth; });

    // El eje bajo el raton (el mas cercano a quien mira, si se tapan).
    int hot = -1;
    if (result.hovered && !(active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))) {
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const float ex = mouse.x - it->screen.x;
            const float ey = mouse.y - it->screen.y;
            if (ex * ex + ey * ey <= ball * ball) {
                hot = it->index;
                break;
            }
        }
    }
    static const ImU32 kColors[3] = {theme::kAxisX, theme::kAxisY, theme::kAxisZ};
    static const char* const kNames[3] = {"X", "Y", "Z"};
    for (const Axis& axis : order) {
        const int k = axis.index % 3;
        const bool positive = axis.index < 3;
        const ImU32 color = kColors[k];
        if (positive) {
            d->AddLine(center, axis.screen, theme::withAlpha(color, 230), std::max(1.5f, radius * 0.045f));
            d->AddCircleFilled(axis.screen, ball, color, 24);
            if (hot == axis.index) d->AddCircle(axis.screen, ball, IM_COL32(255, 255, 255, 255), 24, 1.5f);
            ImFont* font = ImGui::GetFont();
            const float fs = ball * 1.35f;
            const ImVec2 ts = font->CalcTextSizeA(fs, 100.0f, 0.0f, kNames[k]);
            d->AddText(font, fs, ImVec2(axis.screen.x - ts.x * 0.5f, axis.screen.y - ts.y * 0.5f), IM_COL32(0, 0, 0, 230),
                       kNames[k]);
        } else {
            // Negativos: discos apagados con el borde del color (como Blender).
            d->AddCircleFilled(axis.screen, ball * 0.92f, theme::withAlpha(color, hot == axis.index ? 200 : 90), 24);
            d->AddCircle(axis.screen, ball * 0.92f, theme::withAlpha(color, 230), 24, 1.2f);
        }
    }

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        result.dragging = true;
        result.drag = ImGui::GetIO().MouseDelta;
    }
    if (ImGui::IsItemDeactivated() && hovered_box) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool click = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold;
        if (click) {
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                const float ex = mouse.x - it->screen.x;
                const float ey = mouse.y - it->screen.y;
                if (ex * ex + ey * ey <= ball * ball) {
                    result.axis = it->index;
                    break;
                }
            }
        }
    }
    if (result.hovered && hot >= 0 && !result.dragging) {
        static const char* const kViews[6] = {"Vista desde +X (derecha)", "Vista superior (+Y)", "Vista desde +Z (frontal)",
                                              "Vista desde -X (izquierda)", "Vista inferior (-Y)",
                                              "Vista desde -Z (trasera)"};
        ImGui::SetTooltip("%s", kViews[hot]);
    } else if (result.hovered && !result.dragging && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Arrastra para girar la vista; clic en un eje para alinearla");
    }
    return result;
}

bool viewButton(const char* id, Glyph glyph, ImVec2 center, float radius, bool active, const char* tooltip, ImVec2* drag) {
    ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
    ImGui::InvisibleButton(id, ImVec2(radius * 2.0f, radius * 2.0f));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImU32 bg = IM_COL32(0, 0, 0, 45);  // (siempre un disco tenue, como en Blender)
    if (active) bg = theme::kAccent;
    else if (held) bg = IM_COL32(255, 255, 255, 70);
    else if (hovered) bg = IM_COL32(255, 255, 255, 40);
    d->AddCircleFilled(center, radius, bg, 32);
    drawGlyph(d, glyph, center, radius * 1.15f, IM_COL32(235, 235, 235, 255));
    bool clicked = false;
    if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        if (drag != nullptr) *drag = ImGui::GetIO().MouseDelta;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (ImGui::IsItemDeactivated()) {
        const ImGuiIO& io = ImGui::GetIO();
        clicked = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold && hovered;
    }
    if (tooltip != nullptr && hovered && !held && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool verticalTab(const char* id, Glyph glyph, bool active, const char* tooltip, float width) {
    const float h = width;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + width, min.y + h);
    if (active) {
        // La activa con el color del panel, pegada a el (esquinas a la izquierda).
        d->AddRectFilled(ImVec2(min.x + 2.0f, min.y), ImVec2(max.x + 1.0f, max.y), theme::kBg1, 5.0f,
                         ImDrawFlags_RoundCornersLeft);
    } else if (hovered) {
        d->AddRectFilled(ImVec2(min.x + 3.0f, min.y + 1.0f), ImVec2(max.x - 3.0f, max.y - 1.0f), theme::kBg3, 4.0f);
    }
    drawGlyph(d, glyph, ImVec2((min.x + max.x) * 0.5f + 0.5f, (min.y + max.y) * 0.5f), width * 0.5f,
              active ? IM_COL32(255, 255, 255, 255) : (hovered ? theme::kText : theme::kTextDim));
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

void verticalTabsBackground(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    draw->AddRectFilled(min, max, theme::kBg0);
}

bool workspaceTab(const char* id, const char* label, bool active, bool dirty, ImU32 accent, bool* close) {
    const float h = ImGui::GetFrameHeight();
    const float pad = ImGui::GetStyle().FramePadding.x + 4.0f;
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const float close_w = close != nullptr ? h * 0.7f : 0.0f;
    const float dot_w = dirty ? h * 0.35f : 0.0f;
    const ImVec2 size(text.x + pad * 2.0f + close_w + dot_w, h);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##tab", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    if (active) {
        d->AddRectFilled(ImVec2(min.x, min.y + 2.0f), max, theme::kBg1, 5.0f, ImDrawFlags_RoundCornersTop);
    } else if (hovered) {
        d->AddRectFilled(ImVec2(min.x, min.y + 2.0f), max, theme::kBg2, 5.0f, ImDrawFlags_RoundCornersTop);
    }
    if (accent != 0) {
        // El tipo de documento (prefab, script...): una linea fina de su color.
        d->AddLine(ImVec2(min.x + 6.0f, max.y - 1.5f), ImVec2(max.x - 6.0f, max.y - 1.5f), theme::withAlpha(accent, active ? 255 : 150),
                   2.0f);
    }
    const ImU32 color = active ? IM_COL32(255, 255, 255, 255) : (hovered ? theme::kText : theme::kTextDim);
    float x = min.x + pad;
    d->AddText(ImVec2(x, min.y + (h - ImGui::GetFontSize()) * 0.5f + 1.0f), color, label, ImGui::FindRenderedTextEnd(label));
    x += text.x;
    if (dirty) {
        d->AddCircleFilled(ImVec2(x + dot_w * 0.6f, (min.y + max.y) * 0.5f + 1.0f), h * 0.1f, theme::kYellow);
        x += dot_w;
    }
    if (close != nullptr) {
        const ImVec2 c(x + close_w * 0.55f, (min.y + max.y) * 0.5f + 1.0f);
        const float r = h * 0.28f;
        const bool over = hovered && std::abs(ImGui::GetIO().MousePos.x - c.x) <= r + 2.0f &&
                          std::abs(ImGui::GetIO().MousePos.y - c.y) <= r + 2.0f;
        if (hovered || active) {
            if (over) d->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), theme::kBg4, 3.0f);
            drawGlyph(d, Glyph::Close, c, r * 1.5f, over ? IM_COL32(255, 255, 255, 255) : theme::kTextDim);
        }
        if (pressed && over) {
            *close = true;
            ImGui::PopID();
            return false;
        }
    }
    ImGui::PopID();
    return pressed;
}

void areaCorners(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius, ImU32 gap) {
    if (radius <= 0.5f || max.x - min.x < radius * 2.0f || max.y - min.y < radius * 2.0f) return;
    // Cada esquina: el cuadrado de la esquina menos el cuarto de circulo.
    const struct {
        ImVec2 corner;
        ImVec2 center;
        float a0;
    } corners[4] = {{min, ImVec2(min.x + radius, min.y + radius), kPi},
                    {ImVec2(max.x, min.y), ImVec2(max.x - radius, min.y + radius), kPi * 1.5f},
                    {max, ImVec2(max.x - radius, max.y - radius), 0.0f},
                    {ImVec2(min.x, max.y), ImVec2(min.x + radius, max.y - radius), kPi * 0.5f}};
    draw->PushClipRect(min, max, true);
    for (const auto& c : corners) {
        draw->PathLineTo(c.corner);
        draw->PathArcTo(c.center, radius, c.a0, c.a0 + kPi * 0.5f, 8);
        draw->PathFillConvex(gap);
    }
    draw->PopClipRect();
}

void overlayText(ImDrawList* draw, ImVec2 pos, ImU32 color, const char* text) {
    draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 170), text);
    draw->AddText(pos, color, text);
}

}  // namespace cramion::editor::blender
