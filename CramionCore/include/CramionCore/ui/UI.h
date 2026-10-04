#ifndef CRAMION_CORE_UI_H
#define CRAMION_CORE_UI_H

// Interfaz del juego (como UMG de Unreal / uGUI de Unity):
//
//   Canvas         raiz de una interfaz: resolucion de referencia y escalado
//                  con la pantalla (se ve igual a cualquier resolucion).
//   RectTransform  el rectangulo de cada elemento: anclas (min/max, 0..1 del
//                  padre, arriba-izquierda = 0,0), pivote, posicion y tamano
//                  (con anclas separadas, el tamano se suma al hueco entre
//                  ellas: se estira con el padre).
//   Image, Text, Button, Slider, InputField, Toggle: los controles.
//
// UiSystem calcula los rectangulos, la interaccion (raton y teclado) y una
// lista de dibujo que pinta el editor (vista Juego) o el juego exportado; los
// eventos (clic, valor cambiado, texto enviado) llaman a un metodo del script
// del objeto elegido (o del propio control).
//
// Un Canvas en modo Mundo (como World Space de Unity) es un panel en la
// escena: se ve en VR y en cualquier camara, y se usa con rayos (los mandos
// de VR con XR Interactor: el gatillo es el clic). updateWorld() da su lista
// de dibujo a su propia resolucion; la aplicacion la pinta en una textura que
// el render pone en el mundo.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <array>
#include <string>
#include <vector>

namespace cramion::ui {

enum class ScaleMode : int { ConstantPixelSize = 0, ScaleWithScreen = 1 };
enum class RenderMode : int { ScreenSpace = 0, WorldSpace = 1 };
enum class HAlign : int { Left = 0, Center = 1, Right = 2 };
enum class VAlign : int { Top = 0, Middle = 1, Bottom = 2 };

struct Canvas {
    core::Vec2 reference{1920.0f, 1080.0f};
    ScaleMode scale_mode = ScaleMode::ScaleWithScreen;
    float match = 0.5f;  // 0 = ancho, 1 = alto
    int sort_order = 0;
    // Mundo: un panel de reference / pixels_per_meter metros (por la escala de
    // la entidad), centrado en ella y de cara a su +Z (X derecha, Y arriba).
    RenderMode render_mode = RenderMode::ScreenSpace;
    float pixels_per_meter = 1000.0f;
    void reflect(ecs::PropertyVisitor& v);
};

struct RectTransform {
    core::Vec2 anchor_min{0.5f, 0.5f};
    core::Vec2 anchor_max{0.5f, 0.5f};
    core::Vec2 pivot{0.5f, 0.5f};
    core::Vec2 position{0.0f, 0.0f};  // desde el punto de anclaje
    core::Vec2 size{200.0f, 60.0f};   // con anclas separadas: se suma al hueco
    void reflect(ecs::PropertyVisitor& v);
};

struct Image {
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    std::string texture;  // en Assets (vacia = color liso)
    float corner_radius = 0.0f;
    bool preserve_aspect = false;
    void reflect(ecs::PropertyVisitor& v);
};

struct Text {
    std::string text = "Texto";
    // Clave de la tabla de localizacion (gameplay/Localization.h): si no esta
    // vacia, se muestra su texto en el idioma actual (y cambia con el idioma).
    std::string localization_key;
    float font_size = 32.0f;
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    HAlign h_align = HAlign::Center;
    VAlign v_align = VAlign::Middle;
    bool wrap = false;
    bool shadow = false;
    void reflect(ecs::PropertyVisitor& v);
};

struct Button {
    bool interactable = true;
    core::Vec3 normal{0.16f, 0.36f, 0.72f};
    core::Vec3 hover{0.22f, 0.46f, 0.88f};
    core::Vec3 pressed{0.10f, 0.26f, 0.55f};
    core::Vec3 disabled{0.30f, 0.30f, 0.32f};
    float corner_radius = 8.0f;
    Uuid target{};           // objeto cuyo script recibe el clic (vacio = este)
    std::string on_click;    // metodo del script: function X:OnJugar(boton)
    void reflect(ecs::PropertyVisitor& v);
};

struct Slider {
    bool interactable = true;
    float min = 0.0f;
    float max = 1.0f;
    float value = 0.5f;
    bool whole_numbers = false;
    core::Vec3 track{0.18f, 0.18f, 0.21f};
    core::Vec3 fill{0.20f, 0.52f, 0.95f};
    core::Vec3 handle{0.95f, 0.95f, 0.97f};
    Uuid target{};
    std::string on_change;  // function X:OnVolumen(valor)
    void reflect(ecs::PropertyVisitor& v);
};

struct InputField {
    bool interactable = true;
    std::string text;
    std::string placeholder = "Escribe...";
    float font_size = 26.0f;
    int max_length = 0;  // 0 = sin limite
    bool password = false;
    core::Vec3 background{0.10f, 0.10f, 0.12f};
    core::Vec3 text_color{1.0f, 1.0f, 1.0f};
    Uuid target{};
    std::string on_change;  // function X:OnEscribe(texto)
    std::string on_submit;  // Enter: function X:OnEnviar(texto)
    void reflect(ecs::PropertyVisitor& v);
};

struct Toggle {
    bool interactable = true;
    bool on = false;
    core::Vec3 box{0.15f, 0.15f, 0.18f};
    core::Vec3 check{0.20f, 0.60f, 1.0f};
    Uuid target{};
    std::string on_change;  // function X:OnCasilla(activa)
    void reflect(ecs::PropertyVisitor& v);
};

// --- Sistema ---

struct UiRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct UiDrawCommand {
    enum class Type { Rect, Image, Text, Circle, Line } type = Type::Rect;
    UiRect rect;       // en pixeles de la vista
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float radius = 0.0f;  // esquinas / circulo
    std::string texture;  // Image (ruta en Assets)
    std::string text;
    float font_size = 16.0f;  // ya escalado
    int h_align = 1;
    int v_align = 1;
    bool wrap = false;
    bool shadow = false;
    bool preserve_aspect = false;
    entt::entity entity = entt::null;
};

struct UiInput {
    float mouse_x = -1.0f, mouse_y = -1.0f;  // pixeles de la vista (-1 = fuera)
    bool mouse_down = false;
    bool mouse_pressed = false;
    bool mouse_released = false;
    std::string typed;  // caracteres escritos este frame
    bool backspace = false;
    bool enter = false;
};

// Un rayo que usa la interfaz en el mundo (el de un mando de VR).
struct UiPointer {
    bool valid = false;
    core::Vec3 origin{};
    core::Vec3 direction{0.0f, 0.0f, -1.0f};
    bool down = false;  // gatillo pulsado
};

// Donde corta un rayo a un Canvas en el mundo.
struct UiPointerHit {
    bool hit = false;
    float distance = 0.0f;  // metros por el rayo
    core::Vec3 point{};
    entt::entity canvas = entt::null;
    bool over_control = false;  // sobre un Button, Slider, Toggle o campo
};

// Lo que hay que pintar de un Canvas en el mundo: su lista de dibujo (en
// pixeles de su resolucion de referencia) y donde va el panel.
struct WorldCanvasDraw {
    entt::entity canvas = entt::null;
    float width = 0.0f;   // pixeles
    float height = 0.0f;
    core::Mat4 transform = core::Mat4::identity();  // centro y giro en el mundo (sin escala)
    core::Vec2 size{};                              // metros
    std::vector<UiDrawCommand> commands;
};

struct UiEvent {
    ecs::Entity source;  // el control
    ecs::Entity target;  // quien tiene el script
    std::string method;
    enum class Kind { Click, Number, Text, Bool } kind = Kind::Click;
    float number = 0.0f;
    std::string text;
    bool flag = false;
};

class UiSystem {
public:
    // Rectangulos, interaccion (si `interactive`) y la lista de dibujo para
    // una vista de width x height pixeles.
    void update(ecs::World& world, float width, float height, const UiInput& input, bool interactive, float time);
    const std::vector<UiDrawCommand>& drawList() const { return draw_; }
    // Eventos de este update (para los scripts).
    std::vector<UiEvent> takeEvents();
    // El raton esta sobre algun control o se esta escribiendo (el juego no
    // deberia usar ese clic/teclado).
    bool capturingMouse() const { return capturing_mouse_; }
    bool typing() const { return focused_ != entt::null; }
    // Rectangulo (pixeles) de un elemento en el ultimo update (editor).
    bool rectOf(entt::entity entity, UiRect& rect) const;
    float scaleOf(entt::entity entity) const;  // escala de su Canvas
    // Elemento visible mas arriba bajo un punto (seleccion en el editor).
    entt::entity pick(float x, float y) const;
    // Hay un control que se puede pulsar (Button, Slider, Toggle, InputField)
    // bajo el punto: en moviles ese dedo va a la interfaz, no al joystick.
    bool interactiveAt(const ecs::World& world, float x, float y) const;
    void reset();

    // --- Canvas en el mundo (modo Mundo) ---
    static constexpr std::size_t kMaxPointers = 2;
    // Rectangulos, interaccion con los rayos (Button, Toggle, Slider) y la
    // lista de dibujo de cada uno. Los eventos van con los de update().
    void updateWorld(ecs::World& world, const std::array<UiPointer, kMaxPointers>& pointers, bool interactive,
                     float time);
    const std::vector<WorldCanvasDraw>& worldCanvases() const { return world_canvases_; }
    const std::array<UiPointerHit, kMaxPointers>& pointerHits() const { return pointer_hits_; }

private:
    struct Laid {
        entt::entity entity;
        UiRect rect;
        UiRect parent;
        float scale;
    };
    // Que control tiene encima o pulsado cada puntero (el raton es el 0).
    struct DrawState {
        std::array<entt::entity, kMaxPointers> hovered{entt::null, entt::null};
        std::array<entt::entity, kMaxPointers> pressed{entt::null, entt::null};
        entt::entity focused = entt::null;
    };
    void layoutCanvas(ecs::World& world, const ecs::Entity& canvas, const UiRect& root, float scale,
                      std::vector<Laid>& out) const;
    void buildDraw(ecs::World& world, const std::vector<Laid>& laid, const DrawState& state, const UiInput& input,
                   bool interactive, float time, std::vector<UiDrawCommand>& out);
    ecs::Entity targetOf(ecs::World& world, ecs::Entity source, const Uuid& target) const;
    // Clic de un Button o un Toggle; el valor de un Slider (t = 0..1 de su barra).
    void activate(ecs::World& world, ecs::Entity control);
    void setSlider(ecs::World& world, ecs::Entity control, float t);

    std::vector<Laid> laid_;
    std::vector<UiDrawCommand> draw_;
    std::vector<UiEvent> events_;
    entt::entity pressed_ = entt::null;
    entt::entity dragging_ = entt::null;
    entt::entity focused_ = entt::null;
    bool capturing_mouse_ = false;

    std::vector<WorldCanvasDraw> world_canvases_;
    std::array<UiPointerHit, kMaxPointers> pointer_hits_{};
    struct PointerState {
        bool was_down = false;
        entt::entity pressed = entt::null;
        entt::entity dragging = entt::null;
    };
    std::array<PointerState, kMaxPointers> pointer_states_{};
};

// Rectangulo de un RectTransform dentro del de su padre (unidades del Canvas).
UiRect layoutRect(const RectTransform& rt, const UiRect& parent);

void registerUiComponents();

}  // namespace cramion::ui

#endif  // CRAMION_CORE_UI_H
