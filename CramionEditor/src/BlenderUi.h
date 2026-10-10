#ifndef CRAMION_EDITOR_BLENDER_UI_H
#define CRAMION_EDITOR_BLENDER_UI_H

// Piezas de la interfaz al estilo de Blender 4 (sobre el tema de Theme.h):
//
//   - Iconos vectoriales (Glyph): se dibujan con lineas y formas, sin texturas,
//     asi que se ven nitidos a cualquier escala y no dependen de archivos.
//   - Cabecera de area: el boton del tipo de editor (icono y flecha) y los
//     botones de icono planos de las cabeceras.
//   - Estante de herramientas de la vista 3D (columna de botones a la
//     izquierda, la activa en azul).
//   - Gizmo de navegacion (los ejes de la camara arriba a la derecha) y sus
//     botones redondos de acercar, desplazar, camara y perspectiva.
//   - Pestanas verticales del editor de Propiedades.
//   - Pestanas de los espacios de trabajo de la barra superior.
//   - Texto sobre la vista (con sombra, como "Perspectiva del usuario").
//
// Todo es ImGui puro (sin el editor): se prueba con un banco aparte.

#include <imgui.h>

namespace cramion::editor::blender {

enum class Glyph : int {
    // Herramientas
    Select,
    Move,
    Rotate,
    Scale,
    Stamp,
    Brush,
    Modeling,
    // Tipos de editor
    View3D,
    Game,
    Outliner,
    Properties,
    FileBrowser,
    Console,
    Statistics,
    Render,
    Physics,
    Timeline,
    Animation,
    Terminal,
    World,
    // Navegacion de la vista
    Zoom,
    Pan,
    Camera,
    Perspective,
    Orthographic,
    // Sombreado de la vista
    Wireframe,
    Solid,
    Material,
    Rendered,
    // Varios
    Eye,
    EyeClosed,
    Gizmo,
    Magnet,
    Globe,
    Pivot,
    Play,
    Pause,
    Stop,
    StepForward,
    ChevronDown,
    ChevronRight,
    Search,
    Close,
    Plus,
    Object,
    Light,
    Grid,
    Count
};

// Dibuja un icono centrado en `center` dentro de un cuadrado de lado `size`.
void drawGlyph(ImDrawList* draw, Glyph glyph, ImVec2 center, float size, ImU32 color);

// Boton de icono plano (cabeceras): sin fondo hasta pasar el raton; activo,
// relleno azul. `size` 0 = alto de un campo.
bool iconButton(const char* id, Glyph glyph, bool active, const char* tooltip, float size = 0.0f);

// Boton del tipo de editor (icono + flecha, como el de Blender). true al pulsarlo.
bool editorTypeButton(const char* id, Glyph glyph, const char* tooltip);

// Botones unidos (como el modo de seleccion de Blender): devuelve el que se
// pulso o -1. Con etiquetas (`labels`) o solo iconos (`glyphs`).
int segmented(const char* id, const char* const* labels, const Glyph* glyphs, int count, int active,
              const char* const* tooltips = nullptr);

// Boton del estante de herramientas (cuadrado redondeado de `size`, icono;
// activo en azul). Se coloca donde este el cursor.
bool toolButton(const char* id, Glyph glyph, bool active, const char* tooltip, float size);
// Fondo del estante (la columna translucida detras de los botones).
void toolShelfBackground(ImDrawList* draw, ImVec2 min, ImVec2 max);

// Gizmo de navegacion: los seis ejes de la camara. `right`, `up` y `forward`
// son los ejes de la camara en el mundo (Y arriba). Devuelve el eje pulsado
// (0 +X, 1 +Y, 2 +Z, 3 -X, 4 -Y, 5 -Z) o -1; `drag` recibe lo que se
// arrastro este frame (girar la vista) y `dragging` si se esta arrastrando.
struct NavigationResult {
    int axis = -1;
    ImVec2 drag{0.0f, 0.0f};
    bool dragging = false;
    bool hovered = false;
};
NavigationResult navigationGizmo(const char* id, ImVec2 center, float radius, const float right[3], const float up[3],
                                 const float forward[3]);
// Boton redondo de la vista (bajo el gizmo). `drag` recibe el arrastre
// (acercar, desplazar); true al hacer clic sin arrastrar.
bool viewButton(const char* id, Glyph glyph, ImVec2 center, float radius, bool active, const char* tooltip,
                ImVec2* drag = nullptr);

// Pestana vertical del editor de Propiedades (columna izquierda).
bool verticalTab(const char* id, Glyph glyph, bool active, const char* tooltip, float width);
// Fondo de la columna de pestanas.
void verticalTabsBackground(ImDrawList* draw, ImVec2 min, ImVec2 max);

// Pestana de espacio de trabajo (barra superior, como Layout/Modeling): la
// activa con el gris de las areas y las demas solo texto. `close` (si no es
// nulo) muestra la x al pasar el raton y la pone a true si se pulsa.
// Devuelve true si se hizo clic en la pestana.
bool workspaceTab(const char* id, const char* label, bool active, bool dirty, ImU32 accent, bool* close);

// Texto sobre la vista con una sombra (se lee sobre cualquier fondo).
void overlayText(ImDrawList* draw, ImVec2 pos, ImU32 color, const char* text);

// Esquinas redondeadas de un area (como las de Blender): tapa las cuatro
// esquinas del rectangulo con el color de los huecos. Se llama al final de la
// ventana (encima de su contenido).
void areaCorners(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius, ImU32 gap);

}  // namespace cramion::editor::blender

#endif  // CRAMION_EDITOR_BLENDER_UI_H
