#ifndef CRAMION_EDITOR_THEME_H
#define CRAMION_EDITOR_THEME_H

// Tema del editor de Cramion: negros en capas, texto blanco y dos acentos.
//
// Reglas de color (usarlas igual en todas las ventanas):
//  - Rojo   (kRed):    lo principal y lo elegido. Seleccion, pestana con foco,
//                      boton principal, deslizadores, casillas, barras de
//                      progreso, arrastre de separadores. Tambien los errores
//                      (kRedText para texto legible sobre negro).
//  - Amarillo (kYellow): atencion y estado. Avisos, modo Play/Pausa, cambios
//                      sin guardar, overrides de prefab, insignias (nuevo,
//                      actualizacion), foco de teclado y destino al soltar.
//  - Blanco/grises:    contenido (kText) y lo secundario (kTextDim/kTextFaint).
//  - Fondos:           kBg0 (barra de titulo, menus, pestanas) < kBg1 (paneles)
//                      < kBg2 (tarjetas, cabeceras) < kBg3 (campos) < kBg4 (hover).
//  - Ejes X/Y/Z:       rojo/verde/azul (convencion de la industria).
//  - Prefabs:          azul (convencion de Unity); kOk solo para "correcto".
// Lo que no significa nada va en grises: el acento solo donde dice algo.
//
// Este archivo es solo cabecera (lo usa tambien el player con ImGuiLayer);
// los widgets compartidos del editor estan en EditorTheme.cpp.

#include <imgui.h>

#include <string>

namespace cramion::editor::theme {

// --- Paleta ---------------------------------------------------------------
constexpr ImU32 kBg0 = IM_COL32(11, 11, 12, 255);         // #0b0b0c
constexpr ImU32 kBg1 = IM_COL32(18, 18, 20, 255);         // #121214
constexpr ImU32 kBg2 = IM_COL32(24, 24, 27, 255);         // #18181b
constexpr ImU32 kBg3 = IM_COL32(32, 32, 36, 255);         // #202024
constexpr ImU32 kBg4 = IM_COL32(42, 42, 46, 255);         // #2a2a2e
constexpr ImU32 kBg5 = IM_COL32(52, 52, 58, 255);         // #34343a (pulsado)
constexpr ImU32 kBorder = IM_COL32(42, 42, 46, 255);      // #2a2a2e
constexpr ImU32 kBorderStrong = IM_COL32(60, 60, 66, 255);
constexpr ImU32 kText = IM_COL32(242, 242, 242, 255);     // #f2f2f2
constexpr ImU32 kTextDim = IM_COL32(161, 161, 166, 255);  // #a1a1a6
constexpr ImU32 kTextFaint = IM_COL32(108, 108, 116, 255);
constexpr ImU32 kLabel = IM_COL32(200, 200, 205, 255);    // etiquetas de propiedades
constexpr ImU32 kRed = IM_COL32(229, 56, 59, 255);        // #e5383b
constexpr ImU32 kRedHover = IM_COL32(255, 77, 79, 255);   // #ff4d4f
constexpr ImU32 kRedActive = IM_COL32(193, 18, 31, 255);  // #c1121f
constexpr ImU32 kRedText = IM_COL32(255, 107, 107, 255);  // errores en texto
constexpr ImU32 kYellow = IM_COL32(255, 197, 61, 255);    // #ffc53d
constexpr ImU32 kYellowDeep = IM_COL32(245, 179, 1, 255); // #f5b301
constexpr ImU32 kOk = IM_COL32(92, 196, 128, 255);
constexpr ImU32 kPrefab = IM_COL32(110, 170, 255, 255);
constexpr ImU32 kHeader = IM_COL32(40, 40, 45, 255);
constexpr ImU32 kHeaderHover = IM_COL32(50, 50, 56, 255);
constexpr ImU32 kHeaderActive = IM_COL32(60, 60, 67, 255);
constexpr ImU32 kSelection = IM_COL32(229, 56, 59, 72);       // fila elegida (rojo translucido)
constexpr ImU32 kSelectionHover = IM_COL32(229, 56, 59, 100);
constexpr ImU32 kAxisX = IM_COL32(214, 52, 56, 255);
constexpr ImU32 kAxisY = IM_COL32(92, 168, 52, 255);
constexpr ImU32 kAxisZ = IM_COL32(52, 112, 222, 255);

constexpr ImU32 withAlpha(ImU32 color, int alpha) {
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha & 0xFF) << IM_COL32_A_SHIFT);
}

inline ImVec4 vec(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }
inline ImVec4 vec(ImU32 color, float alpha) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(color);
    v.w = alpha;
    return v;
}

// Fuentes (las pone ImGuiLayer al iniciar): seminegrita para titulos.
inline ImFont* g_bold_font = nullptr;
inline ImFont* boldFont() { return g_bold_font; }
// Tamano de la letra pequena (metadatos, insignias, etiquetas de eje).
inline float smallFontSize() { return ImGui::GetStyle().FontSizeBase * 0.84f; }

// Estilo de ImGui con la paleta (sin la escala de DPI).
inline void applyStyle(ImGuiStyle& style) {
    ImGui::StyleColorsDark(&style);

    // Formas: nitidas, con poco redondeo (Unreal 5 / Blender 4).
    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 9.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(10.0f, 4.0f);
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.DockingSeparatorSize = 2.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.TabCloseButtonMinWidthUnselected = 0.0f;

    ImVec4* c = style.Colors;
    const ImVec4 red = vec(kRed);
    c[ImGuiCol_Text] = vec(kText);
    c[ImGuiCol_TextDisabled] = vec(kTextDim);
    c[ImGuiCol_WindowBg] = vec(kBg1);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = vec(kBg2, 0.985f);
    c[ImGuiCol_Border] = vec(kBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = vec(kBg3);
    c[ImGuiCol_FrameBgHovered] = vec(kBg4);
    c[ImGuiCol_FrameBgActive] = vec(kBg5);
    c[ImGuiCol_TitleBg] = vec(kBg0);
    c[ImGuiCol_TitleBgActive] = vec(kBg0);
    c[ImGuiCol_TitleBgCollapsed] = vec(kBg0);
    c[ImGuiCol_MenuBarBg] = vec(kBg0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = vec(kBg5);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(IM_COL32(74, 74, 82, 255));
    c[ImGuiCol_ScrollbarGrabActive] = vec(kRed, 0.85f);
    c[ImGuiCol_CheckMark] = red;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = vec(kRedHover);
    c[ImGuiCol_Button] = vec(kBg3);
    c[ImGuiCol_ButtonHovered] = vec(kBg4);
    c[ImGuiCol_ButtonActive] = vec(kRedActive, 0.85f);
    // Cabeceras plegables y filas de menus/listas en gris neutro (ImGui usa el
    // mismo color para las dos cosas); la seleccion roja la ponen las listas
    // principales (Jerarquia, Proyecto) con pushSelectionColors().
    c[ImGuiCol_Header] = vec(kHeader);
    c[ImGuiCol_HeaderHovered] = vec(kHeaderHover);
    c[ImGuiCol_HeaderActive] = vec(kHeaderActive);
    c[ImGuiCol_Separator] = vec(kBorder);
    c[ImGuiCol_SeparatorHovered] = vec(kRed, 0.7f);
    c[ImGuiCol_SeparatorActive] = red;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = vec(kRed, 0.45f);
    c[ImGuiCol_ResizeGripActive] = vec(kRed, 0.8f);
    c[ImGuiCol_InputTextCursor] = vec(kText);
    c[ImGuiCol_Tab] = vec(kBg0);
    c[ImGuiCol_TabHovered] = vec(kBg3);
    c[ImGuiCol_TabSelected] = vec(kBg1);
    c[ImGuiCol_TabSelectedOverline] = red;
    c[ImGuiCol_TabDimmed] = vec(kBg0);
    c[ImGuiCol_TabDimmedSelected] = vec(kBg1);
    c[ImGuiCol_TabDimmedSelectedOverline] = vec(kBorderStrong);
    c[ImGuiCol_DockingPreview] = vec(kRed, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = vec(kBg0);
    c[ImGuiCol_PlotLines] = vec(kRedHover);
    c[ImGuiCol_PlotLinesHovered] = vec(kYellow);
    c[ImGuiCol_PlotHistogram] = red;
    c[ImGuiCol_PlotHistogramHovered] = vec(kRedHover);
    c[ImGuiCol_TableHeaderBg] = vec(kBg2);
    c[ImGuiCol_TableBorderStrong] = vec(kBorder);
    c[ImGuiCol_TableBorderLight] = vec(IM_COL32(32, 32, 36, 255));
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    c[ImGuiCol_TextLink] = vec(kRedHover);
    c[ImGuiCol_TextSelectedBg] = vec(kRed, 0.35f);
    c[ImGuiCol_TreeLines] = vec(kBorderStrong);
    c[ImGuiCol_DragDropTarget] = vec(kYellow);
    c[ImGuiCol_DragDropTargetBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_UnsavedMarker] = vec(kYellow);
    c[ImGuiCol_NavCursor] = vec(kYellow);
    c[ImGuiCol_NavWindowingHighlight] = vec(kText, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.62f);
}

// Seleccion roja para las listas (Selectable/TreeNode con Selected): poner
// antes de dibujar la lista y quitar con ImGui::PopStyleColor(3).
inline void pushSelectionColors() {
    ImGui::PushStyleColor(ImGuiCol_Header, kSelection);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHeaderHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSelectionHover);
}

// --- Widgets compartidos (EditorTheme.cpp) -----------------------------------

// Boton principal (relleno rojo, texto blanco): la accion importante de un
// panel o dialogo. Uno por zona como mucho.
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// Boton plano (sin fondo hasta pasar el raton): acciones secundarias.
bool ghostButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// Titulo de seccion: texto en seminegrita y una linea fina hasta el borde.
void sectionHeader(const char* text);
// Insignia pequena con fondo de color (texto oscuro sobre amarillo, blanco
// sobre rojo...). En la linea actual.
void badge(const char* text, ImU32 color);
// Caja de busqueda con lupa y boton de borrar. Devuelve si cambio.
bool searchBox(const char* id, std::string& text, const char* hint, float width = -1.0f);
// Boton "⋮" (tres puntos) de un menu; dibujado en `min` con lado `size`.
bool kebabButton(const char* id, const ImVec2& min, float size);
// Texto en letra pequena (gris) en la linea actual.
void smallText(ImU32 color, const char* fmt, ...) IM_FMTARGS(2);
// Texto en seminegrita.
void boldText(const char* text);

// Tarjeta: fondo y borde detras de lo que se dibuje entre begin y end (con
// su propio separador de capas: se puede anidar con tablas).
// `attached`: pegada debajo de una cabecera (sin hueco ni esquinas arriba).
void beginCard(const char* id, ImU32 background = kBg2, bool attached = false);
void endCard();

}  // namespace cramion::editor::theme

#endif  // CRAMION_EDITOR_THEME_H
