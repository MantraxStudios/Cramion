#ifndef CRAMION_EDITOR_THEME_H
#define CRAMION_EDITOR_THEME_H

// Tema del editor de Cramion, al estilo de Blender (tema "Blender Dark"):
// areas grises separadas por huecos oscuros, campos gris medio y un acento
// azul para lo seleccionado.
//
// Reglas de color (usarlas igual en todas las ventanas):
//  - Azul   (kAccent): lo principal y lo elegido. Seleccion, pestana con foco,
//                      boton principal, deslizadores, casillas, barras de
//                      progreso, arrastre de separadores.
//  - Rojo   (kRed):    errores y lo destructivo (kRedText para texto legible).
//  - Naranja (kYellow): atencion y estado. Avisos, modo Play/Pausa, cambios
//                      sin guardar, overrides de prefab, insignias (nuevo,
//                      actualizacion), foco de teclado y destino al soltar.
//  - Blanco/grises:    contenido (kText) y lo secundario (kTextDim/kTextFaint).
//  - Fondos:           kGap (huecos entre areas) < kBg0 (barra superior,
//                      pestanas) < kBg1 (paneles) < kBg2 (tarjetas, cabeceras)
//                      < kBg3 (campos y botones) < kBg4 (hover).
//  - Ejes X/Y/Z:       rojo/verde/azul (convencion de la industria).
//  - Prefabs:          azul claro (convencion de Unity); kOk solo para "correcto".
// Lo que no significa nada va en grises: el acento solo donde dice algo.
//
// Este archivo es solo cabecera (lo usa tambien el player con ImGuiLayer);
// los widgets compartidos del editor estan en EditorTheme.cpp.

#include <imgui.h>

#include <string>

namespace cramion::editor::theme {

// --- Paleta ---------------------------------------------------------------
constexpr ImU32 kGap = IM_COL32(22, 22, 22, 255);         // #161616 huecos entre areas
constexpr ImU32 kBg0 = IM_COL32(35, 35, 35, 255);         // #232323 barra superior, pestanas
constexpr ImU32 kBg1 = IM_COL32(48, 48, 48, 255);         // #303030 fondo de las areas
constexpr ImU32 kBg2 = IM_COL32(61, 61, 61, 255);         // #3d3d3d paneles (tarjetas)
constexpr ImU32 kBg3 = IM_COL32(84, 84, 84, 255);         // #545454 campos y botones
constexpr ImU32 kBg4 = IM_COL32(101, 101, 101, 255);      // #656565 hover
constexpr ImU32 kBg5 = IM_COL32(121, 121, 121, 255);      // #797979 pulsado
constexpr ImU32 kBorder = IM_COL32(36, 36, 36, 255);      // #242424 contorno de los widgets
constexpr ImU32 kBorderStrong = IM_COL32(26, 26, 26, 255);
constexpr ImU32 kText = IM_COL32(229, 229, 229, 255);     // #e5e5e5
constexpr ImU32 kTextDim = IM_COL32(160, 160, 160, 255);  // #a0a0a0
constexpr ImU32 kTextFaint = IM_COL32(118, 118, 118, 255);
constexpr ImU32 kLabel = IM_COL32(196, 196, 196, 255);    // etiquetas de propiedades
constexpr ImU32 kAccent = IM_COL32(71, 114, 179, 255);       // #4772b3 azul de Blender
constexpr ImU32 kAccentHover = IM_COL32(86, 128, 194, 255);  // #5680c2
constexpr ImU32 kAccentActive = IM_COL32(58, 95, 153, 255);  // #3a5f99
constexpr ImU32 kRed = IM_COL32(214, 64, 64, 255);        // #d64040 errores
constexpr ImU32 kRedHover = IM_COL32(232, 84, 84, 255);
constexpr ImU32 kRedActive = IM_COL32(176, 44, 44, 255);
constexpr ImU32 kRedText = IM_COL32(255, 107, 107, 255);  // errores en texto
constexpr ImU32 kYellow = IM_COL32(255, 175, 41, 255);    // #ffaf29 naranja de Blender (activo)
constexpr ImU32 kYellowDeep = IM_COL32(241, 136, 0, 255); // #f18800
constexpr ImU32 kOk = IM_COL32(92, 196, 128, 255);
constexpr ImU32 kPrefab = IM_COL32(110, 170, 255, 255);
constexpr ImU32 kHeader = IM_COL32(61, 61, 61, 255);      // cabecera de panel (#3d3d3d)
constexpr ImU32 kHeaderHover = IM_COL32(72, 72, 72, 255);
constexpr ImU32 kHeaderActive = IM_COL32(82, 82, 82, 255);
constexpr ImU32 kSelection = IM_COL32(51, 77, 128, 255);      // fila elegida (#334d80, Outliner)
constexpr ImU32 kSelectionHover = IM_COL32(71, 114, 179, 255);
constexpr ImU32 kAxisX = IM_COL32(255, 51, 82, 255);      // ejes de Blender
constexpr ImU32 kAxisY = IM_COL32(139, 220, 0, 255);
constexpr ImU32 kAxisZ = IM_COL32(40, 144, 255, 255);

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

    // Formas como Blender 4: widgets redondeados, areas sin borde separadas
    // por huecos oscuros (el separador de docking hace de hueco).
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBarOverlineSize = 0.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 9.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(10.0f, 4.0f);
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.DockingSeparatorSize = 3.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.TabCloseButtonMinWidthUnselected = 0.0f;

    ImVec4* c = style.Colors;
    const ImVec4 accent = vec(kAccent);
    c[ImGuiCol_Text] = vec(kText);
    c[ImGuiCol_TextDisabled] = vec(kTextDim);
    c[ImGuiCol_WindowBg] = vec(kBg1);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = vec(IM_COL32(24, 24, 24, 255), 0.97f);  // menus de Blender (#181818)
    c[ImGuiCol_Border] = vec(kBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = vec(kBg3);
    c[ImGuiCol_FrameBgHovered] = vec(kBg4);
    c[ImGuiCol_FrameBgActive] = vec(kBg5);
    // Cabecera de las areas (tira de pestanas) y barra superior.
    c[ImGuiCol_TitleBg] = vec(kBg0);
    c[ImGuiCol_TitleBgActive] = vec(kBg0);
    c[ImGuiCol_TitleBgCollapsed] = vec(kBg0);
    c[ImGuiCol_MenuBarBg] = vec(kBg0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = vec(kBg3, 0.8f);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(kBg4);
    c[ImGuiCol_ScrollbarGrabActive] = vec(kBg5);
    c[ImGuiCol_CheckMark] = vec(kText);
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = vec(kAccentHover);
    c[ImGuiCol_Button] = vec(kBg3);
    c[ImGuiCol_ButtonHovered] = vec(kBg4);
    c[ImGuiCol_ButtonActive] = accent;
    // Cabeceras plegables (paneles de Blender) y filas de menus/listas en gris
    // (ImGui usa el mismo color para las dos cosas); la seleccion azul la
    // ponen las listas principales (Jerarquia, Proyecto) con pushSelectionColors().
    c[ImGuiCol_Header] = vec(kHeader);
    c[ImGuiCol_HeaderHovered] = vec(kHeaderHover);
    c[ImGuiCol_HeaderActive] = vec(kHeaderActive);
    c[ImGuiCol_Separator] = vec(kGap);
    c[ImGuiCol_SeparatorHovered] = vec(kAccent, 0.7f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = vec(kAccent, 0.45f);
    c[ImGuiCol_ResizeGripActive] = vec(kAccent, 0.8f);
    c[ImGuiCol_InputTextCursor] = vec(kText);
    c[ImGuiCol_Tab] = vec(kBg0);
    c[ImGuiCol_TabHovered] = vec(kBg2);
    c[ImGuiCol_TabSelected] = vec(kBg1);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = vec(kBg0);
    c[ImGuiCol_TabDimmedSelected] = vec(kBg1);
    c[ImGuiCol_TabDimmedSelectedOverline] = vec(kBg1);
    c[ImGuiCol_DockingPreview] = vec(kAccent, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = vec(kGap);
    c[ImGuiCol_PlotLines] = vec(kAccentHover);
    c[ImGuiCol_PlotLinesHovered] = vec(kYellow);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = vec(kAccentHover);
    c[ImGuiCol_TableHeaderBg] = vec(kBg2);
    c[ImGuiCol_TableBorderStrong] = vec(kBorder);
    c[ImGuiCol_TableBorderLight] = vec(IM_COL32(40, 40, 40, 255));
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);  // filas alternas del Outliner
    c[ImGuiCol_TextLink] = vec(kAccentHover);
    c[ImGuiCol_TextSelectedBg] = vec(kAccent, 0.55f);
    c[ImGuiCol_TreeLines] = vec(kBg3);
    c[ImGuiCol_DragDropTarget] = vec(kYellow);
    c[ImGuiCol_DragDropTargetBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_UnsavedMarker] = vec(kYellow);
    c[ImGuiCol_NavCursor] = vec(kYellow);
    c[ImGuiCol_NavWindowingHighlight] = vec(kText, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

// Seleccion azul para las listas (como el Outliner) (Selectable/TreeNode con Selected): poner
// antes de dibujar la lista y quitar con ImGui::PopStyleColor(3).
inline void pushSelectionColors() {
    ImGui::PushStyleColor(ImGuiCol_Header, kSelection);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHeaderHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSelectionHover);
}

// --- Widgets compartidos (EditorTheme.cpp) -----------------------------------

// Boton principal (relleno azul, texto blanco): la accion importante de un
// panel o dialogo. Uno por zona como mucho.
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// Boton plano (sin fondo hasta pasar el raton): acciones secundarias.
bool ghostButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// Titulo de seccion: texto en seminegrita y una linea fina hasta el borde.
void sectionHeader(const char* text);
// Insignia pequena con fondo de color (texto oscuro sobre amarillo, blanco
// sobre azul o rojo...). En la linea actual.
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
