#ifndef CRAMION_CORE_GAMEPLAY_DIALOGUE_H
#define CRAMION_CORE_GAMEPLAY_DIALOGUE_H

// Dialogos ramificados (como Yarn Spinner / Dialogue System de Unity o los
// Dialogue Trees de Unreal). Un asset .crdialog (JSON) es un grafo de nodos:
//
//   Inicio      donde empieza
//   Linea       quien habla, el texto (literal o una clave de localizacion),
//               un audio opcional y si avanza solo tras unos segundos
//   Opciones    el jugador elige (cada opcion puede tener una condicion)
//   Condicion   va por "si" o por "no" segun las variables
//   Variable    cambia una variable (=, +=, -=, alternar)
//   Evento      avisa a los scripts (Dialogue.onEvent) y/o llama a un metodo
//               de un objeto de la escena
//   Saltar      a otro nodo o a otro .crdialog
//   Fin
//
// DialogueSystem ejecuta uno a la vez: emite eventos (linea, opciones, fin...)
// que leen Lua (Dialogue.onLine...) y la caja de dialogo de la UI
// (componente DialogueBox). Las variables del dialogo se guardan con las
// partidas (SaveGame). En los textos, {$oro} pone el valor de una variable.

#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::gameplay {

inline constexpr const char* kDialogueExtension = ".crdialog";

enum class DialogueNodeType : int { Start = 0, Line, Choice, Condition, SetVariable, Event, Jump, End };
inline constexpr int kDialogueNodeTypeCount = 8;
const char* dialogueNodeTypeName(DialogueNodeType type);  // "Linea"...
const char* dialogueNodeTypeKey(DialogueNodeType type);   // "line"... (archivo)

// "oro >= 10", "visto == true", "nombre != $otro" (con $: otra variable).
struct DialogueCondition {
    std::string variable;
    std::string op = "==";  // ==, !=, <, <=, >, >=
    std::string value;
};

struct DialogueOption {
    std::string text;
    std::string text_key;  // clave de localizacion (gana a `text`)
    DialogueCondition condition;  // variable vacia = siempre visible
    bool hide_if_false = true;    // false: se ve deshabilitada
    int next = -1;
};

struct DialogueNode {
    int id = 0;
    DialogueNodeType type = DialogueNodeType::Line;
    core::Vec2 position{};  // en el editor
    std::string comment;

    // Linea
    std::string speaker;
    std::string text;
    std::string text_key;
    std::string audio;       // clip en Assets
    float auto_advance = 0;  // segundos (0 = espera al jugador)

    // Opciones
    std::vector<DialogueOption> options;

    // Condicion
    std::vector<DialogueCondition> conditions;
    bool require_all = true;
    int next_false = -1;

    // Variable
    std::string variable;
    std::string op = "=";  // =, +=, -=, toggle
    std::string value;

    // Evento
    std::string event;
    std::string argument;
    std::string target;  // nombre del objeto de la escena (opcional)
    std::string method;  // metodo de su script (opcional)

    // Saltar
    int jump_node = -1;
    std::string jump_dialogue;  // otro .crdialog (vacio = este)

    int next = -1;  // siguiente (en Condicion: el de "si")
};

struct DialogueVariable {
    std::string name;
    nlohmann::json value = false;  // valor inicial (bool, numero o texto)
};

struct DialogueAsset {
    Uuid uuid{};
    std::string name;
    std::vector<DialogueNode> nodes;
    std::vector<DialogueVariable> variables;
    int next_id = 1;

    DialogueNode* find(int id);
    const DialogueNode* find(int id) const;
    const DialogueNode* start() const;  // el nodo Inicio (o el primero)
    DialogueNode& add(DialogueNodeType type, core::Vec2 position = {});
    void remove(int id);  // y los enlaces que iban a el
};

bool parseDialogue(const std::string& text, DialogueAsset& out, std::string* error = nullptr);
std::string serializeDialogue(const DialogueAsset& asset);
bool loadDialogue(const std::filesystem::path& file, DialogueAsset& out, std::string* error = nullptr);
bool saveDialogue(const std::filesystem::path& file, const DialogueAsset& asset, std::string* error = nullptr);
// Un dialogo nuevo de ejemplo (Inicio -> saludo -> opciones -> ...).
DialogueAsset makeExampleDialogue(const std::string& name);

// --- Ejecucion ---

struct DialogueLine {
    int node = -1;
    std::string speaker;
    std::string text;  // ya localizado y con las variables puestas
    std::string audio;
    float auto_advance = 0.0f;
};

struct DialogueChoice {
    int index = 0;  // 0..n-1 (en Lua 1..n)
    std::string text;
    bool enabled = true;
};

struct DialogueEvent {
    enum class Kind { Started, Line, Choices, Event, Ended } kind = Kind::Line;
    std::string dialogue;  // nombre del dialogo
    DialogueLine line;
    std::vector<DialogueChoice> choices;
    std::string name;      // Evento
    std::string argument;
    std::string target;
    std::string method;
};

class DialogueSystem {
public:
    // Encuentra el archivo de un dialogo por nombre o ruta en Assets.
    using Resolver = std::function<std::filesystem::path(const std::string& name)>;
    void setResolver(Resolver resolver) { resolver_ = std::move(resolver); }

    bool start(const std::string& dialogue, std::string* error = nullptr);
    bool start(const DialogueAsset& asset, int from_node = -1);
    // Sigue tras una linea (si hay opciones, no hace nada).
    void advance();
    // Elige una opcion (0..n-1). false si no hay opciones o no vale.
    bool choose(int index);
    void stop();

    bool active() const { return active_; }
    bool waitingChoice() const { return active_ && !choices_.empty(); }
    const DialogueLine& line() const { return line_; }
    bool hasLine() const { return active_ && line_.node >= 0; }
    const std::vector<DialogueChoice>& choices() const { return choices_; }
    const std::string& dialogueName() const { return asset_.name; }
    const DialogueAsset& asset() const { return asset_; }
    // Segundos desde que salio la linea (efecto maquina de escribir).
    float lineTime() const { return line_time_; }
    // El jugador pidio ver la linea entera (salta la maquina de escribir).
    bool lineRevealed() const { return revealed_; }
    void revealLine() { revealed_ = true; }
    // Lo hace avanzar solo (auto_advance) y cuenta lineTime.
    void update(float dt);
    std::vector<DialogueEvent> takeEvents();
    // Sube con cada linea u opciones nuevas (la UI la usa para reiniciar).
    std::uint64_t revision() const { return revision_; }

    // Variables: compartidas por todos los dialogos y guardadas en las partidas.
    nlohmann::json& variables() { return variables_; }
    const nlohmann::json& variables() const { return variables_; }
    void setVariable(const std::string& name, const nlohmann::json& value) { variables_[name] = value; }
    nlohmann::json variable(const std::string& name) const;
    bool evaluate(const DialogueCondition& condition) const;
    // "{$oro}" -> valor de la variable.
    std::string interpolate(const std::string& text) const;

private:
    void run(int node_id);  // ejecuta hasta la siguiente parada
    void emitEnd();
    std::string optionText(const DialogueOption& option) const;

    DialogueAsset asset_;
    Resolver resolver_;
    bool active_ = false;
    DialogueLine line_;
    std::vector<DialogueChoice> choices_;
    std::vector<int> choice_targets_;
    int pending_next_ = -1;  // tras la linea actual
    float line_time_ = 0.0f;
    bool revealed_ = false;
    std::uint64_t revision_ = 0;
    std::vector<DialogueEvent> events_;
    nlohmann::json variables_ = nlohmann::json::object();
};

// El sistema de dialogos del juego (lo pone el ScriptSystem en Play) para la
// caja de dialogo de la UI. nullptr si no hay juego corriendo.
DialogueSystem* activeDialogue();
void setActiveDialogue(DialogueSystem* system);

// --- Caja de dialogo por defecto (componente de UI) ---
// Va en un objeto con RectTransform dentro de un Canvas: dibuja el panel, el
// nombre de quien habla, el texto (con efecto maquina de escribir) y las
// opciones. Clic / Espacio / Enter = seguir; 1..9 o clic = elegir.
struct DialogueBox {
    core::Vec3 panel_color{0.06f, 0.07f, 0.09f};
    float panel_alpha = 0.88f;
    float corner_radius = 14.0f;
    core::Vec3 speaker_color{1.0f, 0.78f, 0.35f};
    core::Vec3 text_color{0.96f, 0.96f, 0.97f};
    core::Vec3 choice_color{0.16f, 0.36f, 0.72f};
    core::Vec3 choice_hover{0.22f, 0.46f, 0.88f};
    float font_size = 30.0f;
    float speaker_size = 26.0f;
    float chars_per_second = 45.0f;  // 0 = todo de golpe
    bool click_to_continue = true;
    bool show_hint = true;
    void reflect(ecs::PropertyVisitor& v);
};

void registerDialogueComponents();

}  // namespace cramion::gameplay

#endif  // CRAMION_CORE_GAMEPLAY_DIALOGUE_H
