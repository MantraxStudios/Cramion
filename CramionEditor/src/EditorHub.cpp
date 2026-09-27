// Hub de proyectos (como el Unity Hub y el lanzador de Unreal):
//
//   - Barra lateral: marca, secciones con icono (Proyectos, Nuevo proyecto,
//     Actualizaciones con aviso "Nueva", Aprender), abrir, y abajo la tarjeta
//     de la version (al dia / version nueva).
//   - Proyectos: tarjetas o lista, ordenados por fecha o nombre, busqueda,
//     fecha relativa y menu (abrir, mostrar en el Explorador, quitar de la
//     lista). Doble clic abre. Si hay version nueva, un aviso arriba.
//   - Actualizaciones: version instalada y publicada, novedades, buscar,
//     actualizar (abre CramionUpdater.exe, que instala y vuelve a abrir el
//     Hub) y las opciones del actualizador.
//   - Aprender: manual, primeros pasos con Lua, shaders, Discord y GitHub.
//   - Nuevo proyecto: galeria de plantillas (integradas y del usuario) con
//     miniaturas dibujadas, y a la derecha sus detalles, nombre, ubicacion y
//     Crear.
//
// Los selectores de carpeta van en otro hilo (pickFolderAsync): el nativo en
// el hilo principal puede colgar el editor en este equipo (OneDrive).

#include "EditorApp.h"

#include "Dialogs.h"
#include "ProjectTemplates.h"

#include <CramionUpdater/NotesView.h>

#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iostream>

namespace cramion::editor {

namespace {

constexpr ImU32 kAccent = IM_COL32(0, 143, 242, 255);
constexpr ImU32 kCard = IM_COL32(30, 32, 37, 255);
constexpr ImU32 kCardHover = IM_COL32(38, 41, 47, 255);
constexpr ImU32 kCardBorder = IM_COL32(52, 56, 64, 255);
constexpr ImU32 kSidebar = IM_COL32(20, 21, 25, 255);
constexpr ImU32 kTextDim = IM_COL32(150, 156, 168, 255);

ImU32 scaled(ImU32 color, float factor, int alpha = -1) {
    const float r = static_cast<float>(color & 0xFF) * factor;
    const float g = static_cast<float>((color >> 8) & 0xFF) * factor;
    const float b = static_cast<float>((color >> 16) & 0xFF) * factor;
    const int a = alpha >= 0 ? alpha : static_cast<int>(color >> 24);
    return IM_COL32(static_cast<int>(std::min(r, 255.0f)), static_cast<int>(std::min(g, 255.0f)),
                    static_cast<int>(std::min(b, 255.0f)), a);
}

// Color estable por nombre (las tarjetas de proyecto).
ImU32 colorFor(const std::string& name) {
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : name) h = (h ^ c) * 16777619u;
    const float hue = static_cast<float>(h % 360u) / 360.0f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.62f, r, g, b);
    return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f), static_cast<int>(b * 255.0f), 255);
}

std::string initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (const unsigned char c : name) {
        if (std::isspace(c) || c == '_' || c == '-') {
            start = true;
            continue;
        }
        if (start && out.size() < 2 && c < 128) out += static_cast<char>(std::toupper(c));
        start = false;
    }
    return out.empty() ? "?" : out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string relativeTime(std::int64_t seconds_since_epoch) {
    if (seconds_since_epoch <= 0) return "nunca";
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t d = std::max<std::int64_t>(0, now - seconds_since_epoch);
    char text[64];
    if (d < 60) return "hace un momento";
    if (d < 3600) {
        std::snprintf(text, sizeof(text), "hace %lld min", static_cast<long long>(d / 60));
    } else if (d < 86400) {
        std::snprintf(text, sizeof(text), "hace %lld h", static_cast<long long>(d / 3600));
    } else if (d < 2 * 86400) {
        return "ayer";
    } else if (d < 30 * 86400) {
        std::snprintf(text, sizeof(text), "hace %lld días", static_cast<long long>(d / 86400));
    } else {
        const std::time_t t = static_cast<std::time_t>(seconds_since_epoch);
        std::tm local{};
        localtime_s(&local, &t);
        std::strftime(text, sizeof(text), "%d/%m/%Y", &local);
    }
    return text;
}

// Texto centrado con tamano propio (titulos, iniciales).
void centeredText(ImDrawList* draw, float size, ImVec2 center, ImU32 color, const char* text) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    draw->AddText(font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), color, text);
}

// Texto recortado con "..." si no cabe.
std::string ellipsize(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string out = text;
    while (out.size() > 3 && ImGui::CalcTextSize((out + "...").c_str()).x > width) {
        // Sin cortar un caracter UTF-8 por la mitad.
        do {
            out.pop_back();
        } while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
    }
    return out + "...";
}

void showInExplorer(const std::filesystem::path& path) {
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// Suelo en perspectiva (lineas que se juntan en el horizonte).
void perspectiveGrid(ImDrawList* draw, ImVec2 a, ImVec2 b, float horizon, ImU32 color) {
    const float w = b.x - a.x;
    const ImVec2 vanish(a.x + w * 0.5f, horizon);
    for (int i = -8; i <= 8; ++i) {
        draw->AddLine(vanish, ImVec2(vanish.x + static_cast<float>(i) * w * 0.18f, b.y), color, 1.0f);
    }
    for (int i = 1; i <= 6; ++i) {
        const float t = static_cast<float>(i) / 6.0f;
        const float y = horizon + (b.y - horizon) * t * t;
        draw->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), color, 1.0f);
    }
}

// Iconos de la barra lateral: 0 proyectos (rejilla), 1 nuevo (+),
// 2 actualizaciones (flecha abajo), 3 aprender (libro).
void hubIcon(ImDrawList* draw, ImVec2 c, int icon, ImU32 color) {
    switch (icon) {
        case 0:
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    const ImVec2 a(c.x - 7.0f + static_cast<float>(x) * 8.0f, c.y - 7.0f + static_cast<float>(y) * 8.0f);
                    draw->AddRectFilled(a, ImVec2(a.x + 6.0f, a.y + 6.0f), color, 1.5f);
                }
            }
            break;
        case 1:
            draw->AddRect(ImVec2(c.x - 7.5f, c.y - 7.5f), ImVec2(c.x + 7.5f, c.y + 7.5f), color, 3.0f, 0, 1.5f);
            draw->AddLine(ImVec2(c.x - 3.5f, c.y), ImVec2(c.x + 3.5f, c.y), color, 1.8f);
            draw->AddLine(ImVec2(c.x, c.y - 3.5f), ImVec2(c.x, c.y + 3.5f), color, 1.8f);
            break;
        case 2:
            draw->AddCircle(c, 8.0f, color, 24, 1.5f);
            draw->AddLine(ImVec2(c.x, c.y - 4.5f), ImVec2(c.x, c.y + 3.5f), color, 1.8f);
            draw->AddLine(ImVec2(c.x - 3.5f, c.y + 0.5f), ImVec2(c.x, c.y + 4.0f), color, 1.8f);
            draw->AddLine(ImVec2(c.x + 3.5f, c.y + 0.5f), ImVec2(c.x, c.y + 4.0f), color, 1.8f);
            break;
        default:
            draw->AddRect(ImVec2(c.x - 8.0f, c.y - 6.5f), ImVec2(c.x - 0.5f, c.y + 6.5f), color, 1.5f, 0, 1.5f);
            draw->AddRect(ImVec2(c.x + 0.5f, c.y - 6.5f), ImVec2(c.x + 8.0f, c.y + 6.5f), color, 1.5f, 0, 1.5f);
            break;
    }
}

}  // namespace

// Miniatura dibujada de una plantilla (sin imagenes: escala a cualquier tamano).
void EditorApp::drawTemplateArt(ImDrawList* draw, ImVec2 a, ImVec2 b, const ProjectTemplate& t) {
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    const ImU32 accent = t.accent;
    draw->PushClipRect(a, b, true);
    draw->AddRectFilledMultiColor(a, b, scaled(accent, 0.34f), scaled(accent, 0.34f), scaled(accent, 0.10f),
                                  scaled(accent, 0.10f));
    const float horizon = a.y + h * 0.46f;
    const auto P = [&](float x, float y) { return ImVec2(a.x + w * x, a.y + h * y); };
    switch (t.art) {
        case TemplateArt::Blank: {
            draw->AddCircleFilled(P(0.74f, 0.26f), h * 0.11f, IM_COL32(255, 226, 160, 230), 32);
            draw->AddCircleFilled(P(0.74f, 0.26f), h * 0.18f, IM_COL32(255, 226, 160, 40), 32);
            perspectiveGrid(draw, a, b, horizon, scaled(accent, 1.2f, 110));
            draw->AddLine(ImVec2(a.x, horizon), ImVec2(b.x, horizon), scaled(accent, 1.5f, 160), 1.5f);
            break;
        }
        case TemplateArt::ThirdPerson: {
            perspectiveGrid(draw, a, b, horizon, scaled(accent, 1.1f, 80));
            // Escalera de bloques.
            const ImU32 orange = IM_COL32(242, 140, 40, 255);
            const ImU32 blue = IM_COL32(40, 128, 235, 255);
            for (int i = 0; i < 4; ++i) {
                const float x = 0.52f + 0.1f * static_cast<float>(i);
                const float top = 0.72f - 0.1f * static_cast<float>(i);
                draw->AddRectFilled(P(x, top), P(x + 0.1f, 0.86f), i % 2 == 0 ? orange : blue, 2.0f);
                draw->AddRectFilled(P(x, top), P(x + 0.1f, top + 0.025f), IM_COL32(255, 255, 255, 60));
            }
            // Personaje (capsula) y su visor.
            const ImVec2 body_a = P(0.24f, 0.46f);
            const ImVec2 body_b = P(0.33f, 0.84f);
            draw->AddRectFilled(ImVec2(body_a.x + 2, body_b.y - 4), ImVec2(body_b.x + 8, body_b.y + 4), IM_COL32(0, 0, 0, 70),
                                6.0f);
            draw->AddRectFilled(body_a, body_b, IM_COL32(26, 184, 242, 255), (body_b.x - body_a.x) * 0.5f);
            draw->AddRectFilled(P(0.25f, 0.53f), P(0.32f, 0.58f), IM_COL32(15, 18, 24, 255), 3.0f);
            // Monedas.
            for (int i = 0; i < 3; ++i) {
                const ImVec2 c = P(0.58f + 0.1f * static_cast<float>(i), 0.62f - 0.1f * static_cast<float>(i) - 0.08f);
                draw->AddCircleFilled(c, h * 0.035f, IM_COL32(255, 200, 60, 255), 16);
                draw->AddCircle(c, h * 0.035f, IM_COL32(255, 240, 180, 255), 16, 1.5f);
            }
            break;
        }
        case TemplateArt::Navigation: {
            // Vista cenital: laberinto, malla verde, camino y guardias.
            draw->AddRectFilled(P(0.08f, 0.1f), P(0.92f, 0.9f), IM_COL32(40, 200, 90, 45), 4.0f);
            const ImU32 wall = IM_COL32(210, 205, 195, 255);
            draw->AddRectFilled(P(0.08f, 0.1f), P(0.92f, 0.14f), wall);
            draw->AddRectFilled(P(0.08f, 0.86f), P(0.92f, 0.9f), wall);
            draw->AddRectFilled(P(0.08f, 0.1f), P(0.11f, 0.9f), wall);
            draw->AddRectFilled(P(0.89f, 0.1f), P(0.92f, 0.9f), wall);
            draw->AddRectFilled(P(0.30f, 0.14f), P(0.33f, 0.62f), wall);
            draw->AddRectFilled(P(0.52f, 0.38f), P(0.55f, 0.86f), wall);
            draw->AddRectFilled(P(0.70f, 0.14f), P(0.73f, 0.55f), wall);
            const ImVec2 path[] = {P(0.18f, 0.78f), P(0.18f, 0.30f), P(0.42f, 0.22f), P(0.44f, 0.72f),
                                   P(0.63f, 0.74f), P(0.63f, 0.28f), P(0.80f, 0.22f)};
            for (int i = 0; i + 1 < 7; ++i) {
                // Discontinuo: el camino que calcula la IA.
                const ImVec2 p0 = path[i], p1 = path[i + 1];
                const float len = std::hypot(p1.x - p0.x, p1.y - p0.y);
                for (float s = 0.0f; s < len; s += 8.0f) {
                    const float t0 = s / len, t1 = std::min((s + 4.0f) / len, 1.0f);
                    draw->AddLine(ImVec2(p0.x + (p1.x - p0.x) * t0, p0.y + (p1.y - p0.y) * t0),
                                  ImVec2(p0.x + (p1.x - p0.x) * t1, p0.y + (p1.y - p0.y) * t1),
                                  IM_COL32(255, 225, 60, 255), 2.0f);
                }
            }
            draw->AddCircleFilled(P(0.18f, 0.78f), h * 0.04f, IM_COL32(26, 184, 242, 255), 16);
            for (const ImVec2 g : {P(0.42f, 0.5f), P(0.78f, 0.7f), P(0.22f, 0.22f)}) {
                draw->AddCircleFilled(g, h * 0.09f, IM_COL32(230, 50, 45, 45), 20);
                draw->AddCircleFilled(g, h * 0.04f, IM_COL32(230, 50, 45, 255), 16);
            }
            const ImVec2 goal = P(0.80f, 0.22f);
            const float r = h * 0.05f;
            draw->AddQuadFilled(ImVec2(goal.x, goal.y - r), ImVec2(goal.x + r, goal.y), ImVec2(goal.x, goal.y + r),
                                ImVec2(goal.x - r, goal.y), IM_COL32(80, 255, 120, 255));
            break;
        }
        case TemplateArt::Voxel: {
            // Un monton de bloques (cesped y tierra) en perspectiva isometrica.
            draw->AddCircleFilled(P(0.82f, 0.2f), h * 0.08f, IM_COL32(255, 230, 150, 230), 24);
            const float s = h * 0.13f;
            const auto block = [&](float gx, float gy, float gz, ImU32 top, ImU32 left, ImU32 right) {
                const ImVec2 c(a.x + w * 0.46f + (gx - gz) * s * 0.87f, a.y + h * 0.62f + (gx + gz) * s * 0.5f - gy * s);
                const ImVec2 t0(c.x, c.y - s), t1(c.x + s * 0.87f, c.y - s * 0.5f), t2(c.x, c.y), t3(c.x - s * 0.87f, c.y - s * 0.5f);
                draw->AddQuadFilled(t0, t1, t2, t3, top);
                draw->AddQuadFilled(t3, t2, ImVec2(t2.x, t2.y + s), ImVec2(t3.x, t3.y + s), left);
                draw->AddQuadFilled(t2, t1, ImVec2(t1.x, t1.y + s), ImVec2(t2.x, t2.y + s), right);
            };
            const ImU32 grass = IM_COL32(110, 190, 70, 255), dirt_l = IM_COL32(120, 84, 52, 255), dirt_r = IM_COL32(96, 66, 40, 255);
            const ImU32 stone = IM_COL32(150, 150, 155, 255), stone_l = IM_COL32(118, 118, 124, 255), stone_r = IM_COL32(98, 98, 104, 255);
            for (int gz = -2; gz <= 1; ++gz) {
                for (int gx = -2; gx <= 1; ++gx) {
                    const bool hill = gx == 0 && gz == -1;
                    block(static_cast<float>(gx), 0.0f, static_cast<float>(gz), grass, dirt_l, dirt_r);
                    if (hill) block(static_cast<float>(gx), 1.0f, static_cast<float>(gz), stone, stone_l, stone_r);
                }
            }
            break;
        }
        case TemplateArt::Mmo: {
            // Barras de vida y mana, la barra de habilidades y un enemigo con su marca.
            perspectiveGrid(draw, a, b, horizon, scaled(accent, 1.1f, 70));
            draw->AddRectFilled(P(0.05f, 0.08f), P(0.38f, 0.14f), IM_COL32(40, 12, 12, 220), 3.0f);
            draw->AddRectFilled(P(0.05f, 0.08f), P(0.30f, 0.14f), IM_COL32(220, 50, 45, 255), 3.0f);
            draw->AddRectFilled(P(0.05f, 0.16f), P(0.38f, 0.21f), IM_COL32(12, 18, 45, 220), 3.0f);
            draw->AddRectFilled(P(0.05f, 0.16f), P(0.24f, 0.21f), IM_COL32(60, 110, 240, 255), 3.0f);
            for (int i = 0; i < 5; ++i) {
                const float x = 0.26f + 0.1f * static_cast<float>(i);
                draw->AddRectFilled(P(x, 0.84f), P(x + 0.085f, 0.95f), IM_COL32(28, 30, 38, 235), 3.0f);
                draw->AddRectFilled(P(x + 0.01f, 0.855f), P(x + 0.075f, 0.935f),
                                    i == 1 ? IM_COL32(240, 120, 40, 255) : scaled(accent, 1.2f), 2.0f);
            }
            // Heroe y enemigo con su barra.
            draw->AddRectFilled(P(0.30f, 0.44f), P(0.37f, 0.76f), IM_COL32(26, 184, 242, 255), w * 0.035f);
            draw->AddRectFilled(P(0.62f, 0.52f), P(0.72f, 0.76f), IM_COL32(90, 160, 60, 255), w * 0.04f);
            draw->AddRectFilled(P(0.61f, 0.45f), P(0.73f, 0.48f), IM_COL32(40, 12, 12, 230));
            draw->AddRectFilled(P(0.61f, 0.45f), P(0.69f, 0.48f), IM_COL32(230, 45, 40, 255));
            centeredText(draw, h * 0.16f, P(0.84f, 0.36f), IM_COL32(255, 215, 60, 255), "!");
            break;
        }
        case TemplateArt::Creatures: {
            // Un perro de huesos (lineas y articulaciones) sobre un escalon,
            // la cola curvada y la pelota que mira.
            perspectiveGrid(draw, a, b, horizon, scaled(accent, 1.1f, 70));
            draw->AddRectFilled(P(0.52f, 0.70f), P(0.80f, 0.80f), IM_COL32(150, 105, 60, 255), 2.0f);
            const ImU32 bone = IM_COL32(235, 225, 210, 255);
            const ImU32 joint = scaled(accent, 1.3f);
            const float t = w * 0.012f;
            const ImVec2 hip = P(0.36f, 0.46f), chest = P(0.62f, 0.44f), neck = P(0.70f, 0.32f), head = P(0.78f, 0.28f);
            const ImVec2 legs[4][3] = {{hip, P(0.33f, 0.60f), P(0.34f, 0.80f)},
                                       {P(0.40f, 0.47f), P(0.43f, 0.62f), P(0.41f, 0.80f)},
                                       {chest, P(0.62f, 0.58f), P(0.62f, 0.70f)},
                                       {P(0.66f, 0.45f), P(0.69f, 0.58f), P(0.70f, 0.70f)}};
            draw->AddLine(hip, chest, bone, t * 1.4f);
            draw->AddLine(chest, neck, bone, t);
            draw->AddLine(neck, head, bone, t);
            for (const auto& leg : legs) {
                draw->AddLine(leg[0], leg[1], bone, t);
                draw->AddLine(leg[1], leg[2], bone, t);
                draw->AddCircleFilled(leg[1], t * 1.2f, joint);
                draw->AddCircleFilled(leg[2], t * 1.2f, joint);
            }
            draw->AddBezierCubic(hip, P(0.26f, 0.40f), P(0.22f, 0.30f), P(0.25f, 0.24f), IM_COL32(120, 255, 150, 255), t);
            for (const ImVec2& p : {hip, chest, neck, head}) draw->AddCircleFilled(p, t * 1.5f, joint);
            draw->AddCircleFilled(P(0.88f, 0.16f), h * 0.05f, IM_COL32(255, 205, 50, 255));
            draw->AddLine(head, P(0.88f, 0.16f), IM_COL32(255, 120, 200, 160), t * 0.6f);
            break;
        }
        case TemplateArt::OpenWorld: {
            // Isla en el mar con un bosque de pinos y montanas.
            draw->AddRectFilledMultiColor(ImVec2(a.x, horizon), b, IM_COL32(20, 70, 110, 255), IM_COL32(20, 70, 110, 255),
                                          IM_COL32(10, 40, 70, 255), IM_COL32(10, 40, 70, 255));
            draw->AddTriangleFilled(P(0.30f, 0.46f), P(0.52f, 0.12f), P(0.74f, 0.46f), IM_COL32(120, 120, 125, 255));
            draw->AddTriangleFilled(P(0.45f, 0.22f), P(0.52f, 0.12f), P(0.59f, 0.22f), IM_COL32(240, 240, 245, 255));
            draw->AddRectFilled(P(0.08f, 0.44f), P(0.92f, 0.52f), IM_COL32(200, 185, 130, 255), h * 0.04f);
            for (int i = 0; i < 16; ++i) {
                const float x = 0.12f + 0.05f * static_cast<float>(i);
                const float s2 = 0.05f + 0.02f * static_cast<float>((i * 7) % 3);
                draw->AddTriangleFilled(P(x - s2 * 0.5f, 0.46f), P(x, 0.46f - s2 * 2.2f), P(x + s2 * 0.5f, 0.46f),
                                        IM_COL32(30, 90 + (i * 13) % 50, 45, 255));
            }
            break;
        }
        case TemplateArt::Online: {
            // Tres jugadores unidos por lineas de red alrededor de un balon.
            perspectiveGrid(draw, a, b, horizon, scaled(accent, 1.1f, 70));
            const ImVec2 nodes[] = {P(0.22f, 0.40f), P(0.78f, 0.36f), P(0.50f, 0.78f)};
            const ImU32 colors[] = {IM_COL32(40, 128, 235, 255), IM_COL32(230, 50, 45, 255), IM_COL32(60, 200, 90, 255)};
            for (int i = 0; i < 3; ++i) {
                for (int j = i + 1; j < 3; ++j) draw->AddLine(nodes[i], nodes[j], IM_COL32(120, 200, 255, 150), 2.0f);
            }
            for (int i = 0; i < 3; ++i) {
                draw->AddCircleFilled(nodes[i], h * 0.13f, IM_COL32(0, 0, 0, 60), 24);
                draw->AddRectFilled(ImVec2(nodes[i].x - h * 0.05f, nodes[i].y - h * 0.11f), ImVec2(nodes[i].x + h * 0.05f, nodes[i].y + h * 0.09f),
                                    colors[i], h * 0.05f);
            }
            draw->AddCircleFilled(P(0.52f, 0.52f), h * 0.05f, IM_COL32(245, 245, 245, 255), 20);
            // Globo de chat.
            draw->AddRectFilled(P(0.62f, 0.10f), P(0.92f, 0.24f), IM_COL32(255, 255, 255, 220), 6.0f);
            for (int k = 0; k < 3; ++k) {
                draw->AddCircleFilled(P(0.70f + 0.07f * static_cast<float>(k), 0.17f), h * 0.018f, scaled(accent, 0.8f));
            }
            break;
        }
        case TemplateArt::User: {
            perspectiveGrid(draw, a, b, horizon, scaled(accent, 1.1f, 70));
            draw->AddRectFilled(P(0.36f, 0.26f), P(0.5f, 0.34f), scaled(accent, 1.3f), 4.0f);
            draw->AddRectFilled(P(0.36f, 0.31f), P(0.64f, 0.72f), scaled(accent, 1.1f), 6.0f);
            centeredText(draw, h * 0.22f, P(0.5f, 0.52f), IM_COL32(255, 255, 255, 235), initials(t.name).c_str());
            break;
        }
    }
    draw->PopClipRect();
}

// -----------------------------------------------------------------------------
// Hub
// -----------------------------------------------------------------------------

void EditorApp::drawHub() {
    drawHubTitleBar();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Cramion Hub", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    // Selectores de carpeta en otro hilo: se recoge el resultado aqui.
    if (hub_open_pick_ && hub_open_pick_->done) {
        const std::filesystem::path folder = hub_open_pick_->result;
        hub_open_pick_.reset();
        if (!folder.empty()) beginOpenProject(folder);
    }
    if (hub_folder_pick_ && hub_folder_pick_->done) {
        if (!hub_folder_pick_->result.empty()) new_project_folder_ = dialogs::utf8(hub_folder_pick_->result);
        hub_folder_pick_.reset();
    }
    if (show_new_project_) {  // (atajos antiguos: abren la pagina)
        hub_page_ = 1;
        show_new_project_ = false;
    }

    drawHubSidebar();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f, 26.0f));
    ImGui::BeginChild("hub_page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (hub_page_) {
        case 1: drawHubNewProject(); break;
        case 2: drawHubUpdates(); break;
        case 3: drawHubLearn(); break;
        default: drawHubProjects(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

void EditorApp::drawHubSidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kSidebar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 20.0f));
    ImGui::BeginChild("hub_side", ImVec2(252.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    {
        // Linea que separa la barra de la pagina.
        const ImVec2 wp = ImGui::GetWindowPos();
        draw->AddLine(ImVec2(wp.x + ImGui::GetWindowWidth() - 1.0f, wp.y),
                      ImVec2(wp.x + ImGui::GetWindowWidth() - 1.0f, wp.y + ImGui::GetWindowHeight()), kCardBorder);
    }

    // Marca.
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (const ImTextureID logo = imgui_.logo(); logo != 0) {
        draw->AddImage(logo, start, ImVec2(start.x + 40.0f, start.y + 40.0f));
    }
    draw->AddText(ImGui::GetFont(), 22.0f, ImVec2(start.x + 50.0f, start.y + 1.0f), IM_COL32(240, 242, 246, 255),
                  "CRAMION");
    draw->AddText(ImGui::GetFont(), 13.0f, ImVec2(start.x + 51.0f, start.y + 24.0f), kTextDim, "Hub");
    ImGui::Dummy(ImVec2(0.0f, 58.0f));

    // Secciones con icono; Actualizaciones con aviso si hay version nueva.
    const auto section = [&](const char* title) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        draw->AddText(ImGui::GetFont(), 12.0f, ImVec2(p.x + 8.0f, p.y), IM_COL32(110, 116, 128, 255), title);
        ImGui::Dummy(ImVec2(0.0f, 18.0f));
    };
    const auto nav = [&](const char* label, int page, int icon, const char* badge) {
        const bool active = hub_page_ == page;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float h = 38.0f;
        ImGui::PushID(label);
        if (ImGui::InvisibleButton("nav", ImVec2(width, h))) {
            hub_page_ = page;
            hub_error_.clear();
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (active || hovered) {
            draw->AddRectFilled(p, ImVec2(p.x + width, p.y + h), active ? IM_COL32(0, 143, 242, 38) : IM_COL32(255, 255, 255, 10),
                                7.0f);
        }
        if (active) draw->AddRectFilled(ImVec2(p.x, p.y + 8.0f), ImVec2(p.x + 3.0f, p.y + h - 8.0f), kAccent, 2.0f);
        const ImU32 color = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 195, 205, 255);
        hubIcon(draw, ImVec2(p.x + 22.0f, p.y + h * 0.5f), icon, active ? kAccent : IM_COL32(150, 156, 168, 255));
        draw->AddText(ImVec2(p.x + 42.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), color, label);
        if (badge != nullptr) {
            const ImVec2 ts = ImGui::CalcTextSize(badge);
            const ImVec2 b0(p.x + width - ts.x - 22.0f, p.y + h * 0.5f - 10.0f);
            draw->AddRectFilled(b0, ImVec2(b0.x + ts.x + 14.0f, b0.y + 20.0f), kAccent, 10.0f);
            draw->AddText(ImVec2(b0.x + 7.0f, b0.y + 10.0f - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), badge);
        }
    };
    section("INICIO");
    nav("Proyectos", 0, 0, nullptr);
    nav("Nuevo proyecto", 1, 1, nullptr);
    section("CRAMION");
    nav("Actualizaciones", 2, 2, updateAvailable() ? "Nueva" : nullptr);
    nav("Aprender", 3, 3, nullptr);

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::BeginDisabled(hub_open_pick_ != nullptr);
    if (ImGui::Button("Abrir proyecto...", ImVec2(-1.0f, 32.0f))) {
        hub_error_.clear();
        hub_open_pick_ = dialogs::pickFolderAsync(dialogs::fromUtf8(new_project_folder_));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Elige la carpeta del proyecto (la que tiene el .crproj)");
    if (ImGui::Button("Carpeta de plantillas", ImVec2(-1.0f, 28.0f))) {
        std::error_code error;
        std::filesystem::create_directories(userTemplatesFolder(), error);
        ShellExecuteW(nullptr, L"open", userTemplatesFolder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SetItemTooltip("Tus plantillas: se crean desde el editor con Archivo > Guardar proyecto como plantilla");

    // Abajo: tarjeta de la version (clic = Actualizaciones) y salir.
    const float card_h = 64.0f;
    const float bottom = ImGui::GetWindowHeight() - card_h - 58.0f;
    if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("version_card", ImVec2(w, card_h));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) hub_page_ = 2;
        const bool fresh = updateAvailable();
        draw->AddRectFilled(p, ImVec2(p.x + w, p.y + card_h), hovered ? kCardHover : kCard, 8.0f);
        draw->AddRect(p, ImVec2(p.x + w, p.y + card_h), fresh ? kAccent : kCardBorder, 8.0f);
        const std::string version = "Cramion " + update::currentVersion().str();
        draw->AddText(ImVec2(p.x + 12.0f, p.y + 10.0f), IM_COL32(236, 238, 242, 255), version.c_str());
        std::string status = "Vulkan 1.3 · Windows x64";
        ImU32 status_color = kTextDim;
        if (update_check_) {
            status = "Buscando actualizaciones...";
        } else if (fresh) {
            status = "Nueva versión: " + update_release_.version.str();
            status_color = kAccent;
        } else if (update_known_) {
            status = "Al día";
            status_color = IM_COL32(46, 204, 113, 255);
        }
        draw->AddText(ImVec2(p.x + 12.0f, p.y + 34.0f), status_color, status.c_str());
        if (fresh) draw->AddCircleFilled(ImVec2(p.x + w - 14.0f, p.y + 14.0f), 4.5f, kAccent);
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (ImGui::Button("Salir", ImVec2(-1.0f, 26.0f))) quit_ = true;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// --- Proyectos ---------------------------------------------------------------------

void EditorApp::drawHubProjects() {
    std::vector<project::RecentProject> recent = project::recentProjects();
    if (hub_sort_ == 1) {
        std::stable_sort(recent.begin(), recent.end(), [](const project::RecentProject& a, const project::RecentProject& b) {
            return lower(a.name) < lower(b.name);
        });
    } else {
        std::stable_sort(recent.begin(), recent.end(), [](const project::RecentProject& a, const project::RecentProject& b) {
            return a.last_opened > b.last_opened;
        });
    }

    // Cabecera: titulo, busqueda y acciones.
    ImGui::SetWindowFontScale(1.55f);
    ImGui::TextUnformatted("Proyectos");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%zu proyecto%s", recent.size(), recent.size() == 1 ? "" : "s");
    const float right = ImGui::GetContentRegionMax().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(right - 170.0f - 260.0f - 8.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 14.0f);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##buscar", "Buscar proyecto...", &hub_search_);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 1.0f, 1.0f));
    if (ImGui::Button("+  Nuevo proyecto", ImVec2(170.0f, 0.0f))) hub_page_ = 1;
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    drawHubUpdateBanner();

    // Orden y vista (tarjetas o lista).
    if (!recent.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Ordenar por");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        static constexpr const char* kSorts[] = {"Abiertos hace poco", "Nombre"};
        ImGui::Combo("##orden", &hub_sort_, kSorts, 2);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 176.0f);
        const auto toggle = [&](const char* label, bool list) {
            const bool active = hub_list_view_ == list;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.56f, 0.95f, 0.45f));
            if (ImGui::Button(label, ImVec2(84.0f, 0.0f))) hub_list_view_ = list;
            if (active) ImGui::PopStyleColor();
        };
        toggle("Tarjetas", false);
        ImGui::SameLine(0.0f, 8.0f);
        toggle("Lista", true);
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (!hub_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", hub_error_.c_str());
        ImGui::Spacing();
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (recent.empty()) {
        // Estado vacio: invita a crear o abrir.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 center(ImGui::GetCursorScreenPos().x + avail.x * 0.5f, ImGui::GetCursorScreenPos().y + avail.y * 0.35f);
        draw->AddCircleFilled(center, 54.0f, IM_COL32(0, 143, 242, 30), 48);
        centeredText(draw, 46.0f, center, kAccent, "+");
        centeredText(draw, 22.0f, ImVec2(center.x, center.y + 86.0f), IM_COL32(230, 232, 238, 255),
                     "Todavía no hay proyectos");
        centeredText(draw, 15.0f, ImVec2(center.x, center.y + 114.0f), kTextDim,
                     "Crea uno desde una plantilla o abre uno que ya tengas.");
        ImGui::SetCursorScreenPos(ImVec2(center.x - 170.0f, center.y + 140.0f));
        if (ImGui::Button("Nuevo proyecto", ImVec2(160.0f, 34.0f))) hub_page_ = 1;
        ImGui::SameLine(0.0f, 20.0f);
        if (ImGui::Button("Abrir proyecto...", ImVec2(160.0f, 34.0f)) && !hub_open_pick_) {
            hub_open_pick_ = dialogs::pickFolderAsync(dialogs::fromUtf8(new_project_folder_));
        }
        return;
    }

    // Rejilla de tarjetas.
    ImGui::BeginChild("hub_grid", ImVec2(0.0f, 0.0f));
    draw = ImGui::GetWindowDrawList();
    const float spacing = 18.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = hub_list_view_ ? 1 : std::max(1, static_cast<int>((avail + spacing) / (270.0f + spacing)));
    const float card_w = (avail - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float banner_h = 112.0f;
    const float card_h = hub_list_view_ ? 58.0f : banner_h + 78.0f;
    const std::string query = lower(hub_search_);

    std::optional<std::filesystem::path> to_open;
    std::optional<std::filesystem::path> to_remove;
    int column = 0;
    for (const project::RecentProject& p : recent) {
        if (!query.empty() && lower(p.name).find(query) == std::string::npos &&
            lower(dialogs::utf8(p.file)).find(query) == std::string::npos) {
            continue;
        }
        if (column > 0) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID(dialogs::utf8(p.file).c_str());
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + card_w, a.y + card_h);
        ImGui::InvisibleButton("card", ImVec2(card_w, card_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool exists = std::filesystem::exists(p.file);
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && exists) to_open = p.file;
        if (ImGui::BeginPopupContextItem("menu")) {
            if (ImGui::MenuItem("Abrir", nullptr, false, exists)) to_open = p.file;
            if (ImGui::MenuItem("Mostrar en el Explorador", nullptr, false, exists)) showInExplorer(p.file);
            if (ImGui::MenuItem("Copiar ruta")) ImGui::SetClipboardText(dialogs::utf8(p.file.parent_path()).c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Quitar de la lista")) to_remove = p.file;
            ImGui::EndPopup();
        }

        if (hub_list_view_) {
            // Fila: miniatura o iniciales, nombre, carpeta y fecha a la derecha.
            draw->AddRectFilled(a, b, hovered ? kCardHover : kCard, 8.0f);
            draw->AddRect(a, b, hovered ? kAccent : kCardBorder, 8.0f);
            const ImVec2 ta(a.x + 8.0f, a.y + 8.0f), tb(a.x + 8.0f + 74.0f, b.y - 8.0f);
            ImVec2 image_size{};
            const std::filesystem::path thumbnail = p.file.parent_path() / "Library" / "thumbnail.png";
            const ImTextureID texture =
                exists && std::filesystem::exists(thumbnail) ? imgui_.image(thumbnail, &image_size) : ImTextureID{};
            if (texture != 0 && image_size.x > 0.0f && image_size.y > 0.0f) {
                const float keep = std::min(1.0f, ((tb.x - ta.x) / (tb.y - ta.y)) / (image_size.x / image_size.y));
                draw->AddImageRounded(texture, ta, tb, ImVec2(0.0f, (1.0f - keep) * 0.5f), ImVec2(1.0f, 1.0f - (1.0f - keep) * 0.5f),
                                      IM_COL32(255, 255, 255, 255), 5.0f);
            } else {
                const ImU32 color = exists ? colorFor(p.name) : IM_COL32(70, 72, 78, 255);
                draw->AddRectFilled(ta, tb, color, 5.0f);
                centeredText(draw, 20.0f, ImVec2((ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f), IM_COL32(255, 255, 255, 230),
                             initials(p.name).c_str());
            }
            const std::string when = relativeTime(p.last_opened);
            const float when_w = ImGui::CalcTextSize(when.c_str()).x;
            const float text_w = card_w - 110.0f - when_w - 30.0f;
            draw->AddText(ImGui::GetFont(), 17.0f, ImVec2(tb.x + 14.0f, a.y + 9.0f), IM_COL32(240, 242, 246, 255),
                          ellipsize(p.name, text_w).c_str());
            const std::string where = exists ? dialogs::utf8(p.file.parent_path()) : "No se encuentra la carpeta";
            draw->AddText(ImVec2(tb.x + 14.0f, a.y + 31.0f), exists ? kTextDim : IM_COL32(235, 110, 100, 255),
                          ellipsize(where, text_w).c_str());
            draw->AddText(ImVec2(b.x - when_w - 16.0f, a.y + (card_h - ImGui::GetFontSize()) * 0.5f),
                          hovered && exists ? kAccent : IM_COL32(120, 126, 138, 255), when.c_str());
            ImGui::PopID();
            continue;
        }

        // Tarjeta: sombra, fondo, banner con iniciales y datos.
        const float lift = hovered ? 2.0f : 0.0f;
        const ImVec2 ca(a.x, a.y - lift), cb(b.x, b.y - lift);
        draw->AddRectFilled(ImVec2(ca.x + 2, ca.y + 5), ImVec2(cb.x + 2, cb.y + 5), IM_COL32(0, 0, 0, hovered ? 90 : 55), 10.0f);
        draw->AddRectFilled(ca, cb, hovered ? kCardHover : kCard, 10.0f);
        // Banner: la miniatura del proyecto (captura de su escena, en
        // Library/thumbnail.png) recortada para llenarlo; sin ella, las iniciales.
        ImVec2 image_size{};
        const std::filesystem::path thumbnail = p.file.parent_path() / "Library" / "thumbnail.png";
        const ImTextureID texture =
            exists && std::filesystem::exists(thumbnail) ? imgui_.image(thumbnail, &image_size) : ImTextureID{};
        const ImVec2 banner_b(cb.x, ca.y + banner_h);
        if (texture != 0 && image_size.x > 0.0f && image_size.y > 0.0f) {
            // Recorte centrado con la proporcion del banner.
            const float banner_aspect = (banner_b.x - ca.x) / banner_h;
            const float image_aspect = image_size.x / image_size.y;
            ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
            if (image_aspect > banner_aspect) {
                const float keep = banner_aspect / image_aspect;
                uv0.x = (1.0f - keep) * 0.5f;
                uv1.x = 1.0f - uv0.x;
            } else {
                const float keep = image_aspect / banner_aspect;
                uv0.y = (1.0f - keep) * 0.5f;
                uv1.y = 1.0f - uv0.y;
            }
            draw->AddImageRounded(texture, ca, banner_b, uv0, uv1, IM_COL32(255, 255, 255, hovered ? 255 : 235), 10.0f,
                                  ImDrawFlags_RoundCornersTop);
            // Sombra suave abajo para separar la imagen de los datos.
            draw->AddRectFilledMultiColor(ImVec2(ca.x, banner_b.y - 26.0f), banner_b, IM_COL32(0, 0, 0, 0),
                                          IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90));
            draw->AddRect(ca, cb, hovered ? kAccent : kCardBorder, 10.0f, 0, hovered ? 1.5f : 1.0f);
        } else {
            const ImU32 color = exists ? colorFor(p.name) : IM_COL32(70, 72, 78, 255);
            draw->AddRectFilledMultiColor(ca, banner_b, color, color, scaled(color, 0.45f), scaled(color, 0.45f));
            // Esquinas redondeadas del banner: se tapan las de abajo con la tarjeta.
            draw->AddRect(ca, cb, hovered ? kAccent : kCardBorder, 10.0f, 0, hovered ? 1.5f : 1.0f);
            centeredText(draw, 44.0f, ImVec2((ca.x + cb.x) * 0.5f, ca.y + banner_h * 0.5f), IM_COL32(255, 255, 255, 230),
                         initials(p.name).c_str());
        }
        const float tx = ca.x + 14.0f;
        const float ty = ca.y + banner_h + 12.0f;
        draw->AddText(ImGui::GetFont(), 18.0f, ImVec2(tx, ty), IM_COL32(240, 242, 246, 255),
                      ellipsize(p.name, card_w - 28.0f).c_str());
        const std::string where = exists ? dialogs::utf8(p.file.parent_path()) : "No se encuentra la carpeta";
        draw->AddText(ImVec2(tx, ty + 25.0f), exists ? kTextDim : IM_COL32(235, 110, 100, 255),
                      ellipsize(where, card_w - 28.0f).c_str());
        draw->AddText(ImVec2(tx, ty + 44.0f), IM_COL32(120, 126, 138, 255), relativeTime(p.last_opened).c_str());
        if (hovered && exists) {
            const char* hint = "Doble clic para abrir";
            const float hw = ImGui::CalcTextSize(hint).x;
            draw->AddText(ImVec2(cb.x - hw - 14.0f, ty + 44.0f), kAccent, hint);
        }
        ImGui::PopID();
        if (++column >= columns) {
            column = 0;
            ImGui::Dummy(ImVec2(0.0f, spacing - ImGui::GetStyle().ItemSpacing.y));
        }
    }
    ImGui::EndChild();
    if (to_remove) project::removeRecentProject(*to_remove);
    if (to_open) beginOpenProject(*to_open);
}

// --- Nuevo proyecto ----------------------------------------------------------------

void EditorApp::drawHubNewProject() {
    if (!hub_templates_loaded_) {
        hub_templates_ = availableTemplates();
        hub_templates_loaded_ = true;
        hub_template_ = std::clamp(hub_template_, 0, static_cast<int>(hub_templates_.size()) - 1);
    }
    ImGui::SetWindowFontScale(1.55f);
    ImGui::TextUnformatted("Nuevo proyecto");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Elige una plantilla para empezar");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    // Filtros por categoria.
    static constexpr const char* kCategories[] = {"Todas", "Integradas", "Mis plantillas"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool active = hub_category_ == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.56f, 0.95f, 0.45f));
        if (ImGui::Button(kCategories[i], ImVec2(0.0f, 28.0f))) hub_category_ = i;
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Actualizar")) hub_templates_loaded_ = false;
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    const float panel_w = 380.0f;
    const float gallery_w = std::max(ImGui::GetContentRegionAvail().x - panel_w - 24.0f, 280.0f);

    // --- Galeria ---
    ImGui::BeginChild("hub_templates", ImVec2(gallery_w, 0.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float spacing = 16.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (230.0f + spacing)));
    const float card_w = (avail - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float art_h = card_w * 0.56f;
    const float card_h = art_h + 64.0f;
    int column = 0;
    int shown = 0;
    for (int i = 0; i < static_cast<int>(hub_templates_.size()); ++i) {
        const ProjectTemplate& t = hub_templates_[static_cast<std::size_t>(i)];
        if (hub_category_ == 1 && t.category != "Integradas") continue;
        if (hub_category_ == 2 && t.category == "Integradas") continue;
        ++shown;
        if (column > 0) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID(i);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + card_w, a.y + card_h);
        if (ImGui::InvisibleButton("template", ImVec2(card_w, card_h))) hub_template_ = i;
        const bool hovered = ImGui::IsItemHovered();
        const bool selected = hub_template_ == i;
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            hub_template_ = i;
            createProjectFromHub();
        }
        draw->AddRectFilled(ImVec2(a.x + 2, a.y + 5), ImVec2(b.x + 2, b.y + 5), IM_COL32(0, 0, 0, 55), 10.0f);
        draw->AddRectFilled(a, b, hovered ? kCardHover : kCard, 10.0f);
        drawTemplateArt(draw, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + art_h), t);
        draw->AddText(ImGui::GetFont(), 17.0f, ImVec2(a.x + 12.0f, a.y + art_h + 10.0f), IM_COL32(240, 242, 246, 255),
                      ellipsize(t.name, card_w - 24.0f).c_str());
        draw->AddText(ImVec2(a.x + 12.0f, a.y + art_h + 34.0f), kTextDim, ellipsize(t.category, card_w - 24.0f).c_str());
        draw->AddRect(a, b, selected ? kAccent : (hovered ? IM_COL32(90, 96, 108, 255) : kCardBorder), 10.0f, 0,
                      selected ? 2.5f : 1.0f);
        if (selected) {
            const ImVec2 c(b.x - 16.0f, a.y + 16.0f);
            draw->AddCircleFilled(c, 10.0f, kAccent, 20);
            draw->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - 4.5f, c.y), ImVec2(c.x - 1.0f, c.y + 3.5f),
                                                    ImVec2(c.x + 5.0f, c.y - 3.5f)}
                                  .data(),
                              3, IM_COL32(255, 255, 255, 255), 0, 2.0f);
        }
        ImGui::PopID();
        if (++column >= columns) {
            column = 0;
            ImGui::Dummy(ImVec2(0.0f, spacing - ImGui::GetStyle().ItemSpacing.y));
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled("Aún no tienes plantillas propias.");
        ImGui::TextDisabled("Abre un proyecto y usa Archivo > Guardar proyecto como plantilla.");
    }
    ImGui::EndChild();

    // --- Panel de detalles y creacion ---
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kCard));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("hub_details", ImVec2(panel_w, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    if (!hub_templates_.empty()) {
        const ProjectTemplate& t = hub_templates_[static_cast<std::size_t>(hub_template_)];
        ImDrawList* d = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        drawTemplateArt(d, a, ImVec2(a.x + w, a.y + w * 0.5f), t);
        d->AddRect(a, ImVec2(a.x + w, a.y + w * 0.5f), kCardBorder, 6.0f);
        ImGui::Dummy(ImVec2(w, w * 0.5f + 6.0f));
        ImGui::SetWindowFontScale(1.3f);
        ImGui::TextUnformatted(t.name.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", t.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        for (const std::string& f : t.features) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.accent), "•");
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(f.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextUnformatted("Nombre del proyecto");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##name", &new_project_name_);
    ImGui::TextUnformatted("Ubicación");
    ImGui::SetNextItemWidth(-40.0f);
    ImGui::InputText("##folder", &new_project_folder_);
    ImGui::SameLine();
    ImGui::BeginDisabled(hub_folder_pick_ != nullptr);
    if (ImGui::Button("...", ImVec2(32.0f, 0.0f))) {
        hub_folder_pick_ = dialogs::pickFolderAsync(dialogs::fromUtf8(new_project_folder_));
    }
    ImGui::EndDisabled();
    const std::filesystem::path target = dialogs::fromUtf8(new_project_folder_) / dialogs::fromUtf8(new_project_name_);
    std::error_code error;
    const bool taken = !new_project_name_.empty() && std::filesystem::exists(target, error) &&
                       !std::filesystem::is_empty(target, error);
    ImGui::PushTextWrapPos(0.0f);
    if (taken) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "Ya existe una carpeta con ese nombre y no está vacía.");
    } else {
        ImGui::TextDisabled("Se creará en: %s", dialogs::utf8(target).c_str());
    }
    if (!hub_error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", hub_error_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::BeginDisabled(new_project_name_.empty() || taken || hub_templates_.empty());
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 1.0f, 1.0f));
    if (ImGui::Button("Crear proyecto", ImVec2(-1.0f, 40.0f))) createProjectFromHub();
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void EditorApp::createProjectFromHub() {
    if (hub_templates_.empty() || new_project_name_.empty()) return;
    const ProjectTemplate& t = hub_templates_[static_cast<std::size_t>(std::clamp(hub_template_, 0, static_cast<int>(hub_templates_.size()) - 1))];
    try {
        const project::ProjectInfo info =
            createProjectFromTemplate(t, dialogs::fromUtf8(new_project_folder_), new_project_name_);
        std::cout << "[Hub] Proyecto \"" << info.name << "\" creado con la plantilla " << t.name << "\n";
        hub_error_.clear();
        hub_page_ = 0;
        beginOpenProject(info.file);
    } catch (const std::exception& e) {
        hub_error_ = e.what();
    }
}

// --- Actualizaciones ------------------------------------------------------------------

// Aviso arriba de Proyectos cuando hay version nueva.
void EditorApp::drawHubUpdateBanner() {
    if (!updateAvailable()) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 52.0f;
    draw->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(0, 143, 242, 34), 8.0f);
    draw->AddRect(a, ImVec2(a.x + w, a.y + h), IM_COL32(0, 143, 242, 140), 8.0f);
    hubIcon(draw, ImVec2(a.x + 24.0f, a.y + h * 0.5f), 2, kAccent);
    const std::string title = "Cramion " + update_release_.version.str() + " ya está disponible";
    draw->AddText(ImVec2(a.x + 46.0f, a.y + 8.0f), IM_COL32(240, 242, 246, 255), title.c_str());
    const std::string sub = "Tienes la " + update::currentVersion().str() + ". Mira las novedades e instálala con el actualizador.";
    draw->AddText(ImVec2(a.x + 46.0f, a.y + 27.0f), kTextDim, sub.c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - 280.0f, a.y + (h - ImGui::GetFrameHeight()) * 0.5f));
    if (ImGui::Button("Ver novedades", ImVec2(130.0f, 0.0f))) hub_page_ = 2;
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 1.0f, 1.0f));
    ImGui::BeginDisabled(update_release_.zip_url.empty());
    if (ImGui::Button("Actualizar", ImVec2(126.0f, 0.0f))) beginUpdateInstall();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + 12.0f));
}

void EditorApp::drawHubUpdates() {
    ImGui::SetWindowFontScale(1.55f);
    ImGui::TextUnformatted("Actualizaciones");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Cramion se actualiza con su propio actualizador (CramionUpdater.exe): descarga, instala y "
                        "vuelve a abrir tus proyectos.");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::string current = update::currentVersion().str();
    const bool fresh = updateAvailable();
    const bool newer = update_known_ && update_release_.version > update::currentVersion();

    // Dos tarjetas: instalada y publicada.
    const float w = ImGui::GetContentRegionAvail().x;
    const float card_w = (w - 18.0f) * 0.5f;
    const float card_h = 96.0f;
    const auto card = [&](float x, const char* label, const std::string& big, const std::string& note, ImU32 accent) {
        const ImVec2 a(ImGui::GetCursorScreenPos().x + x, ImGui::GetCursorScreenPos().y);
        const ImVec2 b(a.x + card_w, a.y + card_h);
        draw->AddRectFilled(a, b, kCard, 10.0f);
        draw->AddRect(a, b, accent, 10.0f);
        draw->AddText(ImVec2(a.x + 18.0f, a.y + 14.0f), kTextDim, label);
        draw->AddText(ImGui::GetFont(), 30.0f, ImVec2(a.x + 18.0f, a.y + 34.0f), IM_COL32(240, 242, 246, 255), big.c_str());
        draw->AddText(ImVec2(b.x - ImGui::CalcTextSize(note.c_str()).x - 18.0f, a.y + 14.0f), kTextDim, note.c_str());
    };
    card(0.0f, "Instalada", current, "Windows x64 · Vulkan 1.3", kCardBorder);
    std::string published = "?";
    std::string published_small;
    if (update_check_) {
        published = "...";
        published_small = "Buscando";
    } else if (update_known_) {
        published = update_release_.version.str();
        published_small = update_release_.published_at.empty() ? std::string{} : update::formatDate(update_release_.published_at);
    } else if (!update_check_error_.empty()) {
        published_small = "Sin respuesta";
    }
    card(card_w + 18.0f, "Última publicada", published, published_small, fresh ? kAccent : kCardBorder);
    ImGui::Dummy(ImVec2(w, card_h + 14.0f));

    // Estado y acciones.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kCard));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("hub_update_state", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (update_check_) {
        ImGui::TextUnformatted("Buscando la última versión...");
    } else if (fresh) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "Cramion %s%s está disponible", update_release_.version.str().c_str(),
                           update_release_.prerelease ? " (beta)" : "");
        ImGui::PopFont();
        if (update_release_.prerelease) {
            ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.16f, 1.0f), "Versión beta: trae lo último pero puede tener fallos.");
        }
        ImGui::TextDisabled("Al actualizar se cierra el Hub, el actualizador descarga e instala la versión nueva y vuelve a "
                            "abrir el Hub. Tus proyectos no se tocan.");
        if (update_release_.zip_url.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "Esta versión aún no tiene el paquete (%s).", update::kPackageAsset);
        } else if (update_release_.zip_size > 0) {
            ImGui::TextDisabled("Descarga: %s", update::formatBytes(update_release_.zip_size).c_str());
        }
    } else if (update_known_) {
        ImGui::TextColored(ImVec4(0.18f, 0.8f, 0.44f, 1.0f), "Estás al día");
        ImGui::TextDisabled(newer ? "Omitiste la %s: no se avisará de ella." : "Cramion %s es la última versión publicada.",
                            newer ? update_release_.version.str().c_str() : current.c_str());
    } else if (!update_check_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "No se pudo comprobar: %s", update_check_error_.c_str());
    } else {
        ImGui::TextDisabled("Aún no se ha buscado ninguna actualización.");
    }
    if (!update_error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", update_error_.c_str());
    ImGui::Spacing();
    if (newer) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 1.0f, 1.0f));
        ImGui::BeginDisabled(update_release_.zip_url.empty());
        if (ImGui::Button("Actualizar ahora", ImVec2(170.0f, 34.0f))) beginUpdateInstall();
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(update_check_ != nullptr);
    if (ImGui::Button("Buscar actualizaciones", ImVec2(0.0f, 34.0f))) startUpdateCheck(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Abrir el actualizador", ImVec2(0.0f, 34.0f))) launchUpdater(UpdaterMode::Open);
    ImGui::SetItemTooltip("La aplicación de actualizaciones, aparte del editor");
    ImGui::SameLine();
    // Reinstalar: la version publicada otra vez (repara la instalacion).
    ImGui::BeginDisabled(!update_known_ || update_release_.zip_url.empty());
    if (ImGui::Button("Reinstalar", ImVec2(0.0f, 34.0f))) beginUpdateInstall(true);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(update_known_ && !update_release_.zip_url.empty()
                              ? "Descarga e instala otra vez la %s: repara archivos que falten o estén dañados. "
                                "Se cierra el Hub y vuelve a abrirse."
                              : "Busca actualizaciones primero (hace falta el paquete de la versión publicada)",
                          update_release_.version.str().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Versiones en GitHub", ImVec2(0.0f, 34.0f))) {
        update::openUrl(update_known_ ? update_release_.page_url : std::string(update::kReleasesPage));
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Checkbox("Buscar actualizaciones al abrir Cramion", &update_settings_.check_on_startup)) {
        update::Settings s = update::loadSettings();
        s.check_on_startup = update_settings_.check_on_startup;
        update::saveSettings(s);
    }
    ImGui::SameLine(0.0f, 30.0f);
    {
        // Canal: estable o beta (el canal beta tambien recibe las versiones previas).
        int channel = update_settings_.channel == "beta" ? 1 : 0;
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("##canal", &channel, "Canal estable\0Canal beta\0")) {
            update::Settings s = update::loadSettings();
            s.channel = channel == 1 ? "beta" : "estable";
            update::saveSettings(s);
            update_settings_.channel = s.channel;
            update_known_ = false;
            startUpdateCheck(true);
        }
        ImGui::SetItemTooltip("Beta: recibe también las versiones previas (salen antes, pueden tener fallos)");
    }
    if (!update_settings_.skipped_version.empty()) {
        ImGui::SameLine(0.0f, 30.0f);
        ImGui::TextDisabled("Omitida: %s", update_settings_.skipped_version.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dejar de omitir")) {
            update_settings_ = update::loadSettings();
            update_settings_.skipped_version.clear();
            update::saveSettings(update_settings_);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Novedades.
    if (update_known_) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextDisabled("Novedades de la %s", update_release_.version.str().c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 12.0f));
        ImGui::BeginChild("hub_update_notes", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (update_notes_.empty()) {
            ImGui::TextDisabled("Esta versión no tiene notas.");
        } else {
            update::drawReleaseNotes(update_notes_, kAccent);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}

// --- Aprender ----------------------------------------------------------------------

void EditorApp::drawHubLearn() {
    ImGui::SetWindowFontScale(1.55f);
    ImGui::TextUnformatted("Aprender");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("El manual, la referencia de Lua y la comunidad.");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // El manual va con el motor (docs/); si no esta, el de la web.
    const std::filesystem::path local_manual = update::installFolder() / "docs" / "manual" / "index.html";
    const bool offline = std::filesystem::exists(local_manual);
    struct Item {
        const char* title;
        const char* text;
        std::string target;
        ImU32 color;
    };
    const std::string web = "https://cramion.mantraxtools.store/manual/";
    const Item items[] = {
        {"Manual", offline ? "Todo el motor, paso a paso (sin conexión)." : "Todo el motor, paso a paso.",
         offline ? dialogs::utf8(local_manual) : web + "index.html", IM_COL32(0, 143, 242, 255)},
        {"Tu primer script", "Lua: mover objetos, entrada, física y UI.", web + "primer-script.html", IM_COL32(242, 140, 40, 255)},
        {"Shaders propios", "Superficies .crshader con recarga en caliente.", web + "shaders.html", IM_COL32(160, 90, 240, 255)},
        {"Novedades", "Qué trae cada versión (CHANGELOG).", std::string(update::kReleasesPage), IM_COL32(46, 204, 113, 255)},
        {"Discord", "Pregunta, enseña tu juego y habla con el equipo.", "https://discord.gg/zG7rSsUGEz", IM_COL32(88, 101, 242, 255)},
        {"GitHub", "Código, versiones e incidencias.", "https://github.com/MantraxStudios/Cramion", IM_COL32(200, 205, 215, 255)},
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float spacing = 18.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (300.0f + spacing)));
    const float card_w = (avail - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float card_h = 108.0f;
    int column = 0;
    for (const Item& item : items) {
        if (column > 0) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID(item.title);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + card_w, a.y + card_h);
        if (ImGui::InvisibleButton("learn", ImVec2(card_w, card_h))) update::openUrl(item.target);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("%s", item.target.c_str());
        draw->AddRectFilled(a, b, hovered ? kCardHover : kCard, 10.0f);
        draw->AddRect(a, b, hovered ? item.color : kCardBorder, 10.0f);
        draw->AddRectFilled(ImVec2(a.x, a.y + 14.0f), ImVec2(a.x + 4.0f, b.y - 14.0f), item.color, 2.0f);
        draw->AddText(ImGui::GetFont(), 19.0f, ImVec2(a.x + 22.0f, a.y + 20.0f), IM_COL32(240, 242, 246, 255), item.title);
        draw->AddText(ImVec2(a.x + 22.0f, a.y + 50.0f), kTextDim, ellipsize(item.text, card_w - 40.0f).c_str());
        draw->AddText(ImVec2(a.x + 22.0f, a.y + 76.0f), hovered ? item.color : IM_COL32(120, 126, 138, 255), "Abrir  >");
        ImGui::PopID();
        if (++column >= columns) {
            column = 0;
            ImGui::Dummy(ImVec2(0.0f, spacing - ImGui::GetStyle().ItemSpacing.y));
        }
    }
}

// --- Guardar como plantilla (desde el editor) ----------------------------------------

void EditorApp::drawSaveTemplateDialog() {
    if (show_save_template_) {
        ImGui::OpenPopup("Guardar como plantilla");
        show_save_template_ = false;
        if (template_name_.empty()) template_name_ = project_.name;
        template_error_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Guardar como plantilla", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextWrapped("Copia los Assets y los ajustes del proyecto a una plantilla. Aparecerá en el Hub, "
                       "en Nuevo proyecto > Mis plantillas.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Nombre");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##template_name", &template_name_);
    ImGui::TextUnformatted("Descripción");
    ImGui::InputTextMultiline("##template_description", &template_description_, ImVec2(-1.0f, 70.0f));
    if (!template_error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", template_error_.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(template_name_.empty());
    if (ImGui::Button("Guardar", ImVec2(120.0f, 0.0f))) {
        if (dirty_) saveScene();  // la escena abierta tal como se ve
        if (saveProjectAsTemplate(project_, template_name_, template_description_, &template_error_)) {
            std::cout << "[Editor] Plantilla guardada: " << template_name_ << "\n";
            hub_templates_loaded_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace cramion::editor
