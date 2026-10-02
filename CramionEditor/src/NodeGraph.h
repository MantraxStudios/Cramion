#ifndef CRAMION_EDITOR_NODE_GRAPH_H
#define CRAMION_EDITOR_NODE_GRAPH_H

// Lienzo de nodos reutilizable (Shader Graph, Visual Scripting, Dialogos,
// Behavior Trees): rejilla con desplazamiento y zoom, nodos con pines de
// colores, enlaces bezier, seleccion por caja, mover, borrar, comentarios y
// un buscador para crear nodos. No sabe nada del modelo: quien lo usa le da
// cada frame una vista (NodeView/LinkView) y aplica lo que devuelve (Events).
//
//   nodegraph::Canvas canvas;                 // guarda pan/zoom/seleccion
//   std::vector<nodegraph::NodeView> nodes = ...;  // position apunta al modelo
//   nodegraph::Events ev = canvas.draw("grafo", nodes, links, size, &inline_editor);
//   if (ev.link) graph.connect(ev.link->from_node, ...);
//
// Teclas: Supr borrar, Ctrl+C/V/D copiar/pegar/duplicar, F encuadrar, Espacio
// o clic derecho buscar nodos, rueda zoom, boton central o Alt+arrastrar
// desplazar.

#include <CramionFX/core/Math.h>

#include <imgui.h>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cramion::editor::nodegraph {

struct PinView {
    std::string label;
    ImU32 color = IM_COL32(200, 200, 200, 255);
    bool exec = false;        // pin de ejecucion (triangulo blanco)
    bool connected = false;
    std::string tooltip;      // valor en vivo, tipo...
    bool inline_value = false;  // dibujar un control para su valor (sin enlace)
};

struct NodeView {
    int id = 0;
    std::string title;
    std::string subtitle;
    ImU32 header = IM_COL32(70, 90, 130, 255);
    core::Vec2* position = nullptr;  // del modelo (el lienzo lo mueve)
    float width = 190.0f;
    std::vector<PinView> inputs;
    std::vector<PinView> outputs;
    std::string tooltip;
    std::string error;            // borde rojo y texto al pasar el raton
    float highlight = 0.0f;       // 0..1: se ejecuto hace poco (Play)
    bool breakpoint = false;      // punto rojo
    bool active = false;          // halo azul (el nodo en marcha: dialogo, arbol)
    ImU32 status_color = 0;       // barra de estado abajo (BT: exito/fallo); 0 = ninguna
    // Comentario: caja de fondo redimensionable (sin pines).
    bool comment = false;
    core::Vec2* comment_size = nullptr;
    std::string body;             // texto dentro del nodo (debajo de los pines)
    float extra_height = 0.0f;    // espacio bajo los pines para `draw_body`
};

struct LinkView {
    int from_node = 0;
    int from_pin = 0;
    int to_node = 0;
    int to_pin = 0;
    ImU32 color = IM_COL32(200, 200, 200, 255);
    bool exec = false;
    float flow = 0.0f;  // 0..1: corrio hace poco (puntos que avanzan)
};

struct PinRef {
    int node = 0;
    int pin = 0;
    bool output = false;
};

struct Events {
    // Enlace nuevo (de una salida a una entrada).
    std::optional<LinkView> link;
    // Quitar el enlace que llega a una entrada (Alt+clic o arrastrar fuera).
    std::optional<PinRef> unlink;
    // Se solto un enlace en el vacio: abrir el buscador para crear un nodo
    // conectado a este pin.
    std::optional<PinRef> dropped;
    // Nodos a borrar (Supr o menu).
    std::vector<int> erase;
    bool moved = false;         // termino un arrastre de nodos (guardar / deshacer)
    bool copy = false;
    bool paste = false;
    bool duplicate = false;
    // Menu contextual del fondo (crear nodo) o de un nodo, con la posicion
    // del lienzo donde se abrio.
    bool background_menu = false;
    int node_menu = 0;          // id del nodo (0 = ninguno)
    core::Vec2 menu_position{};
    int double_clicked = 0;     // id del nodo
    int clicked = 0;            // id del nodo que se acaba de seleccionar
    std::optional<PinRef> hovered_pin;
    int hovered_node = 0;
};

// Dibuja el control de un valor de entrada dentro del nodo (ImGui en la
// posicion actual del cursor, `width` pixeles). Devuelve si cambio.
using InlineEditor = std::function<bool(int node, int pin, float width)>;
// Dibuja el cuerpo de un nodo (extra_height) en la posicion del cursor.
using BodyDrawer = std::function<void(int node, ImVec2 min, ImVec2 max)>;

class Canvas {
public:
    core::Vec2 pan{80.0f, 80.0f};
    float zoom = 1.0f;
    std::set<int> selection;
    bool vertical = false;  // entradas arriba y salidas abajo (arboles)
    bool snap = false;      // a la rejilla al soltar

    Events draw(const char* id, std::vector<NodeView>& nodes, const std::vector<LinkView>& links, ImVec2 size,
                const InlineEditor* inline_editor = nullptr, const BodyDrawer* body_drawer = nullptr);

    // Coordenadas del lienzo <-> pantalla (del ultimo draw).
    core::Vec2 toCanvas(ImVec2 screen) const;
    ImVec2 toScreen(core::Vec2 canvas) const;
    // Centro de lo visible (para crear nodos con un boton).
    core::Vec2 viewCenter() const;
    // Encuadra estos nodos (o todos si no hay seleccion).
    void frame(const std::vector<NodeView>& nodes);
    void select(int id, bool add = false);

    // Enlace en curso (arrastrando desde un pin).
    bool linking() const { return link_from_.has_value(); }

private:
    ImVec2 origin_{};
    ImVec2 size_{};
    std::optional<PinRef> link_from_;
    bool dragging_nodes_ = false;
    bool box_selecting_ = false;
    ImVec2 box_start_{};
    int resizing_comment_ = 0;
    bool pending_frame_ = false;
};

// --- Buscador de nodos (popup) ---------------------------------------------------
struct SearchItem {
    std::string category;
    std::string title;
    std::string keywords;
    std::string description;
    int index = 0;  // del que llama
};

// Popup "Crear nodo" con caja de busqueda y categorias. Devuelve el indice
// del elemento elegido (SearchItem::index) o -1. Llamar cada frame tras
// ImGui::OpenPopup(popup_id).
int searchPopup(const char* popup_id, const std::vector<SearchItem>& items, std::string& filter);

// Coincidencia sin mayusculas ni tildes de todas las palabras de `filter`.
bool fuzzyMatch(const std::string& text, const std::string& filter);

}  // namespace cramion::editor::nodegraph

#endif  // CRAMION_EDITOR_NODE_GRAPH_H
