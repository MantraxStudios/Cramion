#include "CramionCore/ai/BehaviorTree.h"

// La definicion de ComponentRegistry::registerComponent<T>.
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace cramion::ai {

using nlohmann::json;

namespace {

constexpr float kNotFound = 1.0e6f;  // distancia cuando no hay objetivo

// Bits de VarType para BtParamInfo::key_types.
constexpr int kAnyKey = -1;
constexpr int kBoolKey = 1 << static_cast<int>(VarType::Bool);
constexpr int kNumberKey = (1 << static_cast<int>(VarType::Int)) | (1 << static_cast<int>(VarType::Float));
constexpr int kEntityKey = 1 << static_cast<int>(VarType::Entity);
constexpr int kVectorKey = 1 << static_cast<int>(VarType::Vector);
constexpr int kPlaceKey = kEntityKey | kVectorKey;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "Move To", "move_to", "moveto" -> "moveto" (para leer lo que escriba una persona o una IA).
std::string normalized(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == ' ' || c == '_' || c == '-') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string formatNumber(double n) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", n);
    return buffer;
}

json vec2(const core::Vec2& v) { return json::array({v.x, v.y}); }
json vec3(const core::Vec3& v) { return json::array({v.x, v.y, v.z}); }

core::Vec2 readVec2(const json& j, core::Vec2 fallback) {
    if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number()) {
        return core::Vec2{j[0].get<float>(), j[1].get<float>()};
    }
    return fallback;
}

std::string jsonValueText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number()) return formatNumber(j.get<double>());
    if (j.is_array()) {
        std::string out;
        for (const json& x : j) {
            if (!out.empty()) out += ' ';
            out += x.is_number() ? formatNumber(x.get<double>()) : std::string("0");
        }
        return out;
    }
    return {};
}

json valueJson(const Value& v) {
    switch (v.type) {
        case VarType::Bool: return v.b;
        case VarType::Int: return static_cast<long long>(std::llround(v.n));
        case VarType::Float: return v.n;
        case VarType::Vector: return vec3(v.v);
        default: return v.s;
    }
}

bool writeText(const std::filesystem::path& path, const std::string& text, std::string* error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            if (error) *error = "no se pudo escribir " + path.string();
            return false;
        }
        file << text;
        if (!file) {
            if (error) *error = "error escribiendo " + path.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

// --- Tablas de tipos ---------------------------------------------------------------

struct NodeType {
    BtNodeKind kind;
    const char* key;
    const char* label;
    const char* tooltip;
};

const NodeType kNodeTypes[] = {
    {BtNodeKind::Root, "root", "Raíz", "Donde empieza el árbol (un solo hijo)"},
    {BtNodeKind::Selector, "selector", "Selector",
     "Prueba sus hijos de izquierda a derecha y sale bien con el primero que salga bien (\"o\")"},
    {BtNodeKind::Sequence, "sequence", "Sequence",
     "Ejecuta sus hijos en orden y falla en cuanto uno falle (\"y\")"},
    {BtNodeKind::Parallel, "parallel", "Parallel", "Ejecuta todos sus hijos a la vez (ver la política)"},
    {BtNodeKind::RandomSelector, "random_selector", "Random Selector",
     "Como Selector, pero prueba los hijos en un orden al azar cada vez"},
    {BtNodeKind::MoveTo, "move_to", "Move To",
     "Va hasta una clave (objeto o Vec3) por la malla de navegación (NavAgent); sin NavAgent, en línea recta"},
    {BtNodeKind::Wait, "wait", "Wait", "Espera unos segundos (con una variación al azar opcional)"},
    {BtNodeKind::PlayAnimation, "play_animation", "Play Animation", "Reproduce un clip en el Animator del objeto"},
    {BtNodeKind::SetAnimatorParam, "set_animator_param", "Set Animator Param",
     "Cambia un parámetro del Animator (float, bool, int o trigger)"},
    {BtNodeKind::RotateToFace, "rotate_to_face", "Rotate To Face", "Gira el objeto (en Y) hasta mirar a una clave"},
    {BtNodeKind::SetBlackboard, "set_blackboard", "Set Blackboard", "Pone un valor en una clave de la pizarra"},
    {BtNodeKind::FindRandomPoint, "find_random_point", "Find Random Point",
     "Busca un punto al azar de la malla de navegación alrededor del objeto (o de una clave)"},
    {BtNodeKind::RunScript, "run_script", "Run Script",
     "Llama a una tarea de script: BehaviorTree.registerTask(\"Nombre\", fn) en Lua, o una función global; "
     "devuelve \"success\", \"failure\" o \"running\" (o true/false). Los scripts de C++ terminan con bt:finishTask()"},
    {BtNodeKind::SendMessage, "send_message", "Send Message",
     "Envía un mensaje a los scripts del objeto (o de otro): OnMessage / Script::onMessage en C++"},
    {BtNodeKind::Log, "log", "Log", "Escribe en la Consola ({clave} pone el valor de la pizarra)"},
};

struct DecoratorType {
    BtDecoratorKind kind;
    const char* key;
    const char* label;
    const char* tooltip;
};

const DecoratorType kDecoratorTypes[] = {
    {BtDecoratorKind::Blackboard, "blackboard", "Blackboard", "Entra solo si una clave cumple la comparación"},
    {BtDecoratorKind::ScriptCondition, "script_condition", "Script Condition",
     "Entra solo si una expresión Lua es verdadera (ve las claves por su nombre, self y entity)"},
    {BtDecoratorKind::IsAtLocation, "is_at_location", "Is At Location",
     "Entra solo si el objeto está cerca (o lejos) de una clave"},
    {BtDecoratorKind::Cooldown, "cooldown", "Cooldown", "Después de terminar, no deja volver a entrar durante unos segundos"},
    {BtDecoratorKind::Loop, "loop", "Loop", "Repite la rama N veces (0 = siempre) mientras salga bien"},
    {BtDecoratorKind::TimeLimit, "time_limit", "Time Limit", "Si la rama tarda más de N segundos, se corta y falla"},
    {BtDecoratorKind::Inverter, "inverter", "Inverter", "Bien <-> mal"},
    {BtDecoratorKind::ForceSuccess, "force_success", "Force Success", "Sale bien siempre"},
    {BtDecoratorKind::ForceFailure, "force_failure", "Force Failure", "Falla siempre"},
};

struct ServiceType {
    BtServiceKind kind;
    const char* key;
    const char* label;
    const char* tooltip;
};

const ServiceType kServiceTypes[] = {
    {BtServiceKind::FindNearestWithTag, "find_nearest_with_tag", "Find Nearest With Tag",
     "Guarda el objeto más cercano con un tag (y su distancia)"},
    {BtServiceKind::DistanceTo, "distance_to", "Distance To", "Guarda la distancia hasta una clave (objeto o Vec3)"},
    {BtServiceKind::Sight, "sight", "Sight (vista)",
     "Percepción: el objeto con el tag más cercano dentro del cono de visión y sin paredes en medio (rayo)"},
    {BtServiceKind::Hearing, "hearing", "Hearing (oído)",
     "Percepción: ruidos (BehaviorTree.reportNoise) y objetos con el tag muy cerca"},
    {BtServiceKind::RunScript, "run_script", "Run Script", "Llama a una función de script cada intervalo (sensores propios)"},
};

constexpr const char* kAbortKeys[] = {"none", "self", "lower_priority", "both"};
constexpr const char* kAbortLabels[] = {"Ninguno", "Self", "Lower Priority", "Both"};
constexpr const char* kStatusLabels[] = {"Inactivo", "Corriendo", "Bien", "Falló", "Abortado"};

constexpr const char* kCompareOptions =
    "igual a|distinto de|mayor que|menor que|mayor o igual que|menor o igual que|está puesta (verdadero)|no está puesta (falso)";

using P = BtParamInfo;

const std::vector<BtParamInfo>& noParams() {
    static const std::vector<BtParamInfo> none;
    return none;
}

}  // namespace

// --- Nombres ---------------------------------------------------------------------------

const std::vector<BtNodeKind>& btNodeKinds() {
    static const std::vector<BtNodeKind> kinds = [] {
        std::vector<BtNodeKind> out;
        for (const NodeType& t : kNodeTypes) out.push_back(t.kind);
        return out;
    }();
    return kinds;
}

namespace {
const NodeType& nodeType(BtNodeKind kind) {
    for (const NodeType& t : kNodeTypes) {
        if (t.kind == kind) return t;
    }
    return kNodeTypes[2];
}
}  // namespace

const char* btNodeKey(BtNodeKind kind) { return nodeType(kind).key; }
const char* btNodeLabel(BtNodeKind kind) { return nodeType(kind).label; }
const char* btNodeTooltip(BtNodeKind kind) { return nodeType(kind).tooltip; }

BtNodeKind btNodeFromKey(const std::string& key, BtNodeKind fallback) {
    const std::string k = normalized(key);
    for (const NodeType& t : kNodeTypes) {
        if (k == normalized(t.key) || k == normalized(t.label)) return t.kind;
    }
    if (k == "raiz") return BtNodeKind::Root;
    return fallback;
}

bool btIsComposite(BtNodeKind kind) {
    return kind == BtNodeKind::Selector || kind == BtNodeKind::Sequence || kind == BtNodeKind::Parallel ||
           kind == BtNodeKind::RandomSelector;
}

bool btIsTask(BtNodeKind kind) { return static_cast<int>(kind) >= 10; }

const char* btDecoratorKey(BtDecoratorKind kind) {
    return kDecoratorTypes[std::clamp(static_cast<int>(kind), 0, kBtDecoratorKindCount - 1)].key;
}
const char* btDecoratorLabel(BtDecoratorKind kind) {
    return kDecoratorTypes[std::clamp(static_cast<int>(kind), 0, kBtDecoratorKindCount - 1)].label;
}
const char* btDecoratorTooltip(BtDecoratorKind kind) {
    return kDecoratorTypes[std::clamp(static_cast<int>(kind), 0, kBtDecoratorKindCount - 1)].tooltip;
}
BtDecoratorKind btDecoratorFromKey(const std::string& key, BtDecoratorKind fallback) {
    const std::string k = normalized(key);
    for (const DecoratorType& t : kDecoratorTypes) {
        if (k == normalized(t.key) || k == normalized(t.label)) return t.kind;
    }
    if (k == "condition" || k == "lua") return BtDecoratorKind::ScriptCondition;
    return fallback;
}
bool btIsCondition(BtDecoratorKind kind) {
    return kind == BtDecoratorKind::Blackboard || kind == BtDecoratorKind::ScriptCondition ||
           kind == BtDecoratorKind::IsAtLocation || kind == BtDecoratorKind::Cooldown;
}

const char* btServiceKey(BtServiceKind kind) {
    return kServiceTypes[std::clamp(static_cast<int>(kind), 0, kBtServiceKindCount - 1)].key;
}
const char* btServiceLabel(BtServiceKind kind) {
    return kServiceTypes[std::clamp(static_cast<int>(kind), 0, kBtServiceKindCount - 1)].label;
}
const char* btServiceTooltip(BtServiceKind kind) {
    return kServiceTypes[std::clamp(static_cast<int>(kind), 0, kBtServiceKindCount - 1)].tooltip;
}
BtServiceKind btServiceFromKey(const std::string& key, BtServiceKind fallback) {
    const std::string k = normalized(key);
    for (const ServiceType& t : kServiceTypes) {
        if (k == normalized(t.key) || k == normalized(t.label)) return t.kind;
    }
    if (k == "vista" || k == "sightperception") return BtServiceKind::Sight;
    if (k == "oido" || k == "hearingperception") return BtServiceKind::Hearing;
    return fallback;
}

const char* btAbortKey(BtAbort abort) { return kAbortKeys[std::clamp(static_cast<int>(abort), 0, kBtAbortCount - 1)]; }
const char* btAbortLabel(BtAbort abort) { return kAbortLabels[std::clamp(static_cast<int>(abort), 0, kBtAbortCount - 1)]; }
BtAbort btAbortFromKey(const std::string& key) {
    const std::string k = normalized(key);
    for (int i = 0; i < kBtAbortCount; ++i) {
        if (k == normalized(kAbortKeys[i]) || k == normalized(kAbortLabels[i])) return static_cast<BtAbort>(i);
    }
    if (k == "lower") return BtAbort::LowerPriority;
    return BtAbort::None;
}

const char* btStatusLabel(BtStatus status) { return kStatusLabels[std::clamp(static_cast<int>(status), 0, 4)]; }

// --- Parametros ---------------------------------------------------------------------------

const std::vector<BtParamInfo>& btParamsOf(BtNodeKind kind) {
    static const std::map<BtNodeKind, std::vector<BtParamInfo>> table = {
        {BtNodeKind::Parallel,
         {P{BtField::Option, "policy", "Política",
            "Todos: sale bien cuando todos salen bien y falla si uno falla.\nUno: sale bien en cuanto uno sale bien.\n"
            "Principal: manda el primer hijo; los demás se repiten de fondo y se cortan cuando él termina.",
            0.0f, "Todos|Uno|Principal"}}},
        {BtNodeKind::MoveTo,
         {P{BtField::Key, "target", "Destino", "Clave entity o vec3", 0.0f, nullptr, kPlaceKey},
          P{BtField::Number, "acceptance", "Radio de llegada", "Metros: llega cuando está así de cerca", 0.6f},
          P{BtField::Flag, "track", "Seguir si se mueve", "Si el objetivo se mueve, recalcula el camino", 1.0f},
          P{BtField::Number2, "speed", "Velocidad sin NavAgent", "m/s en línea recta si el objeto no tiene NavAgent", 3.5f}}},
        {BtNodeKind::Wait,
         {P{BtField::Number, "seconds", "Segundos", "", 2.0f},
          P{BtField::Number2, "deviation", "Variación", "+- segundos al azar", 0.0f}}},
        {BtNodeKind::PlayAnimation,
         {P{BtField::Text, "clip", "Clip", "Nombre de la animación del modelo"},
          P{BtField::Flag, "loop", "En bucle", "", 1.0f},
          P{BtField::Number, "wait", "Esperar (s)", "0 = sigue enseguida; si no, espera esos segundos", 0.0f}}},
        {BtNodeKind::SetAnimatorParam,
         {P{BtField::Text, "parameter", "Parámetro", "Nombre del parámetro del Animator Controller"},
          P{BtField::Option, "param_type", "Tipo", "", 0.0f, "Float|Bool|Int|Trigger"},
          P{BtField::Value, "value", "Valor", "Número, true/false o $clave de la pizarra"}}},
        {BtNodeKind::RotateToFace,
         {P{BtField::Key, "target", "Mirar a", "Clave entity o vec3", 0.0f, nullptr, kPlaceKey},
          P{BtField::Number, "turn_speed", "Grados/s", "0 = gira de golpe", 360.0f},
          P{BtField::Number2, "tolerance", "Tolerancia (grados)", "", 5.0f}}},
        {BtNodeKind::SetBlackboard,
         {P{BtField::Key, "key", "Clave", ""},
          P{BtField::Value, "value", "Valor", "Con el tipo de la clave; $otra copia otra clave"},
          P{BtField::Flag, "clear", "Vaciar", "Pone la clave a su valor vacío (false, 0, sin objeto)", 0.0f}}},
        {BtNodeKind::FindRandomPoint,
         {P{BtField::Key, "out", "Guardar en", "Clave vec3", 0.0f, nullptr, kVectorKey},
          P{BtField::Number, "radius", "Radio", "Metros alrededor del centro", 10.0f},
          P{BtField::Key2, "center", "Centro (opcional)", "Clave entity o vec3; vacío = el propio objeto", 0.0f, nullptr, kPlaceKey}}},
        {BtNodeKind::RunScript,
         {P{BtField::Text, "function", "Tarea / función", "Nombre registrado con BehaviorTree.registerTask o una función global"}}},
        {BtNodeKind::SendMessage,
         {P{BtField::Text, "message", "Mensaje", "Nombre del mensaje (OnMessage / onMessage)"},
          P{BtField::Value, "value", "Valor", "Texto que acompaña al mensaje ($clave = el de la pizarra)"},
          P{BtField::Key, "target", "Destino (opcional)", "Clave entity; vacío = el propio objeto", 0.0f, nullptr, kEntityKey}}},
        {BtNodeKind::Log,
         {P{BtField::Text, "text", "Texto", "{clave} se cambia por su valor"},
          P{BtField::Option, "level", "Nivel", "", 0.0f, "Info|Aviso|Error"}}},
    };
    const auto it = table.find(kind);
    return it != table.end() ? it->second : noParams();
}

const std::vector<BtParamInfo>& btParamsOf(BtDecoratorKind kind) {
    static const std::map<BtDecoratorKind, std::vector<BtParamInfo>> table = {
        {BtDecoratorKind::Blackboard,
         {P{BtField::Key, "key", "Clave", ""},
          P{BtField::Option, "compare", "Condición", "", static_cast<float>(Compare::IsTrue), kCompareOptions},
          P{BtField::Value, "value", "Valor", "Con el tipo de la clave; $otra = otra clave"}}},
        {BtDecoratorKind::ScriptCondition,
         {P{BtField::Text, "expression", "Expresión Lua", "p. ej. distancia < 5 and vida > 20"}}},
        {BtDecoratorKind::IsAtLocation,
         {P{BtField::Key, "target", "Lugar", "Clave entity o vec3", 0.0f, nullptr, kPlaceKey},
          P{BtField::Number, "radius", "Radio", "Metros", 1.5f},
          P{BtField::Flag, "inverse", "Invertir (lejos)", "Entra si NO está cerca", 0.0f}}},
        {BtDecoratorKind::Cooldown, {P{BtField::Number, "seconds", "Segundos", "", 5.0f}}},
        {BtDecoratorKind::Loop, {P{BtField::Number, "count", "Veces", "0 = siempre", 3.0f}}},
        {BtDecoratorKind::TimeLimit, {P{BtField::Number, "seconds", "Segundos", "", 10.0f}}},
    };
    const auto it = table.find(kind);
    return it != table.end() ? it->second : noParams();
}

const std::vector<BtParamInfo>& btParamsOf(BtServiceKind kind) {
    static const std::map<BtServiceKind, std::vector<BtParamInfo>> table = {
        {BtServiceKind::FindNearestWithTag,
         {P{BtField::Text, "tag", "Tag", "", 0.0f},
          P{BtField::Key, "out", "Guardar objeto en", "Clave entity", 0.0f, nullptr, kEntityKey},
          P{BtField::Number, "max_distance", "Distancia máxima", "0 = sin límite", 0.0f},
          P{BtField::Key2, "distance_out", "Guardar distancia en", "Clave float (opcional)", 0.0f, nullptr, kNumberKey}}},
        {BtServiceKind::DistanceTo,
         {P{BtField::Key, "target", "Hasta", "Clave entity o vec3", 0.0f, nullptr, kPlaceKey},
          P{BtField::Key2, "out", "Guardar en", "Clave float", 0.0f, nullptr, kNumberKey}}},
        {BtServiceKind::Sight,
         {P{BtField::Text, "tag", "Tag de los objetivos", "", 0.0f},
          P{BtField::Number, "range", "Alcance", "Metros", 20.0f},
          P{BtField::Number2, "angle", "Ángulo de visión", "Grados del cono completo", 120.0f},
          P{BtField::Flag, "line_of_sight", "Línea de vista", "Comprueba con un rayo de física que no haya paredes", 1.0f},
          P{BtField::Number3, "eye_height", "Altura de los ojos", "Metros sobre el pivote", 1.6f},
          P{BtField::Key, "out", "Objetivo visto", "Clave entity (vacía si no ve a nadie)", 0.0f, nullptr, kEntityKey},
          P{BtField::Key2, "last_seen", "Última posición", "Clave vec3 (opcional)", 0.0f, nullptr, kVectorKey},
          P{BtField::Key3, "can_see", "Lo ve", "Clave bool (opcional)", 0.0f, nullptr, kBoolKey}}},
        {BtServiceKind::Hearing,
         {P{BtField::Number, "range", "Oído", "Multiplica el radio de cada ruido (1 = normal)", 1.0f},
          P{BtField::Number3, "memory", "Memoria (s)", "Ruidos de los últimos N segundos", 1.0f},
          P{BtField::Text, "tag", "Tag cercano (opcional)", "Los objetos con este tag se oyen siempre muy cerca"},
          P{BtField::Number2, "proximity", "Cercanía", "Metros a los que se oye a los del tag", 3.0f},
          P{BtField::Key, "out", "Quién", "Clave entity (opcional)", 0.0f, nullptr, kEntityKey},
          P{BtField::Key2, "position", "Dónde", "Clave vec3 (opcional)", 0.0f, nullptr, kVectorKey},
          P{BtField::Key3, "heard", "Oyó algo", "Clave bool (opcional)", 0.0f, nullptr, kBoolKey}}},
        {BtServiceKind::RunScript,
         {P{BtField::Text, "function", "Función", "Se llama con (self, bt) cada intervalo"}}},
    };
    const auto it = table.find(kind);
    return it != table.end() ? it->second : noParams();
}

std::string* btTextField(BtParams& p, BtField field) {
    switch (field) {
        case BtField::Key: return &p.key;
        case BtField::Key2: return &p.key2;
        case BtField::Key3: return &p.key3;
        case BtField::Text: return &p.text;
        case BtField::Value: return &p.value;
        default: return nullptr;
    }
}

float* btNumberField(BtParams& p, BtField field) {
    switch (field) {
        case BtField::Number: return &p.number;
        case BtField::Number2: return &p.number2;
        case BtField::Number3: return &p.number3;
        default: return nullptr;
    }
}

BtParams btDefaultParams(const std::vector<BtParamInfo>& infos) {
    BtParams p;
    for (const BtParamInfo& info : infos) {
        if (float* n = btNumberField(p, info.field)) *n = info.default_number;
        if (info.field == BtField::Option) p.option = static_cast<int>(info.default_number);
        if (info.field == BtField::Flag) p.flag = info.default_number != 0.0f;
    }
    return p;
}

namespace {

std::vector<std::string> splitOptions(const char* options) {
    std::vector<std::string> out;
    if (options == nullptr) return out;
    std::string current;
    for (const char* c = options; *c != 0; ++c) {
        if (*c == '|') {
            out.push_back(current);
            current.clear();
        } else {
            current += *c;
        }
    }
    out.push_back(current);
    return out;
}

json paramsJson(const BtParams& p, const std::vector<BtParamInfo>& infos) {
    json j = json::object();
    BtParams copy = p;
    for (const BtParamInfo& info : infos) {
        if (const std::string* s = btTextField(copy, info.field)) {
            j[info.json] = *s;
        } else if (const float* n = btNumberField(copy, info.field)) {
            j[info.json] = *n;
        } else if (info.field == BtField::Flag) {
            j[info.json] = p.flag;
        } else if (info.field == BtField::Option) {
            if (std::string(info.json) == "compare") j[info.json] = compareKey(static_cast<Compare>(p.option));
            else j[info.json] = p.option;
        }
    }
    return j;
}

BtParams readParams(const json& j, const std::vector<BtParamInfo>& infos) {
    BtParams p = btDefaultParams(infos);
    if (!j.is_object()) return p;
    for (const BtParamInfo& info : infos) {
        const auto it = j.find(info.json);
        if (it == j.end()) continue;
        const json& x = *it;
        if (std::string* s = btTextField(p, info.field)) {
            *s = jsonValueText(x);
        } else if (float* n = btNumberField(p, info.field)) {
            *n = x.is_number() ? x.get<float>() : static_cast<float>(std::strtod(jsonValueText(x).c_str(), nullptr));
        } else if (info.field == BtField::Flag) {
            if (x.is_boolean()) p.flag = x.get<bool>();
            else if (x.is_number()) p.flag = x.get<double>() != 0.0;
            else p.flag = Value::parse(VarType::Bool, jsonValueText(x)).b;
        } else if (info.field == BtField::Option) {
            const std::vector<std::string> names = splitOptions(info.options);
            const int count = std::max(1, static_cast<int>(names.size()));
            if (x.is_number()) {
                p.option = std::clamp(x.get<int>(), 0, count - 1);
            } else if (std::string(info.json) == "compare") {
                p.option = static_cast<int>(compareFromKey(jsonValueText(x), Compare::IsTrue));
            } else {
                const std::string wanted = normalized(jsonValueText(x));
                for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                    const std::string name = normalized(names[i]);
                    if (name == wanted || (!wanted.empty() && name.rfind(wanted, 0) == 0)) {
                        p.option = i;
                        break;
                    }
                }
            }
        }
    }
    return p;
}

}  // namespace

// --- Asset ------------------------------------------------------------------------------

std::string BtNode::title() const { return name.empty() ? std::string(btNodeLabel(kind)) : name; }

void BehaviorTreeAsset::ensureRoot() {
    if (!nodes.empty() && nodes[0].kind == BtNodeKind::Root) return;
    BtNode root;
    root.kind = BtNodeKind::Root;
    root.position = core::Vec2{0.0f, 0.0f};
    if (nodes.empty()) {
        nodes.push_back(std::move(root));
        return;
    }
    // Habia nodos sin raiz: la raiz va delante (todos los indices +1).
    for (BtNode& n : nodes) {
        for (int& c : n.children) ++c;
    }
    root.children.push_back(1);
    nodes.insert(nodes.begin(), std::move(root));
}

const Variable* BehaviorTreeAsset::findKey(const std::string& name) const {
    for (const Variable& v : blackboard) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

Variable* BehaviorTreeAsset::findKey(const std::string& name) {
    for (Variable& v : blackboard) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

int BehaviorTreeAsset::parentOf(int node) const {
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        for (const int c : nodes[i].children) {
            if (c == node) return i;
        }
    }
    return -1;
}

int BehaviorTreeAsset::addNode(BtNodeKind kind, int parent, core::Vec2 position) {
    ensureRoot();
    BtNode n;
    n.kind = kind;
    n.position = position;
    n.params = btDefaultParams(btParamsOf(kind));
    nodes.push_back(std::move(n));
    const int index = static_cast<int>(nodes.size()) - 1;
    if (parent >= 0) attach(index, parent);
    return index;
}

namespace {
// Rehace el vector sin los nodos marcados y arregla los indices de los hijos.
void compact(std::vector<BtNode>& nodes, const std::vector<bool>& removed) {
    std::vector<int> remap(nodes.size(), -1);
    int next = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!removed[i]) remap[i] = next++;
    }
    std::vector<BtNode> kept;
    kept.reserve(static_cast<std::size_t>(next));
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (removed[i]) continue;
        BtNode n = std::move(nodes[i]);
        std::vector<int> children;
        for (const int c : n.children) {
            if (c >= 0 && c < static_cast<int>(remap.size()) && remap[c] >= 0) children.push_back(remap[c]);
        }
        n.children = std::move(children);
        kept.push_back(std::move(n));
    }
    nodes = std::move(kept);
}
}  // namespace

void BehaviorTreeAsset::removeSubtree(int node) {
    if (node <= 0 || node >= static_cast<int>(nodes.size())) return;
    std::vector<bool> removed(nodes.size(), false);
    std::vector<int> stack{node};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (n < 0 || n >= static_cast<int>(nodes.size()) || removed[n]) continue;
        removed[n] = true;
        for (const int c : nodes[n].children) stack.push_back(c);
    }
    compact(nodes, removed);
}

void BehaviorTreeAsset::removeNode(int node) {
    if (node <= 0 || node >= static_cast<int>(nodes.size())) return;
    std::vector<bool> removed(nodes.size(), false);
    removed[node] = true;
    compact(nodes, removed);
}

void BehaviorTreeAsset::detach(int child) {
    for (BtNode& n : nodes) std::erase(n.children, child);
}

bool BehaviorTreeAsset::attach(int child, int parent, int index) {
    const int count = static_cast<int>(nodes.size());
    if (child <= 0 || child >= count || parent < 0 || parent >= count || child == parent) return false;
    const BtNodeKind kind = nodes[parent].kind;
    if (kind != BtNodeKind::Root && !btIsComposite(kind)) return false;
    // Ciclo: el hijo no puede ser antepasado del padre.
    for (int p = parent, guard = 0; p >= 0 && guard <= count; p = parentOf(p), ++guard) {
        if (p == child) return false;
    }
    detach(child);
    std::vector<int>& list = nodes[parent].children;
    if (kind == BtNodeKind::Root) list.clear();  // la raiz tiene un solo hijo
    if (index < 0 || index > static_cast<int>(list.size())) list.push_back(child);
    else list.insert(list.begin() + index, child);
    return true;
}

void BehaviorTreeAsset::sortChildrenByPosition() {
    for (BtNode& n : nodes) {
        std::stable_sort(n.children.begin(), n.children.end(), [&](int a, int b) {
            const float ax = a >= 0 && a < static_cast<int>(nodes.size()) ? nodes[a].position.x : 0.0f;
            const float bx = b >= 0 && b < static_cast<int>(nodes.size()) ? nodes[b].position.x : 0.0f;
            return ax < bx;
        });
    }
}

void BehaviorTreeAsset::autoLayout() {
    ensureRoot();
    constexpr float kColumn = 250.0f;
    constexpr float kRow = 170.0f;
    const int count = static_cast<int>(nodes.size());
    std::vector<float> width(nodes.size(), 1.0f);
    std::vector<bool> placed(nodes.size(), false);
    // Ancho de cada subarbol (en columnas).
    std::function<float(int, int)> measure = [&](int n, int depth) -> float {
        if (n < 0 || n >= count || depth > count) return 0.0f;
        float total = 0.0f;
        for (const int c : nodes[n].children) total += measure(c, depth + 1);
        width[n] = std::max(1.0f, total);
        return width[n];
    };
    std::function<void(int, float, float, int)> place = [&](int n, float left, float y, int depth) {
        if (n < 0 || n >= count || depth > count || placed[n]) return;
        placed[n] = true;
        nodes[n].position = core::Vec2{(left + width[n] * 0.5f) * kColumn, y};
        float x = left;
        for (const int c : nodes[n].children) {
            place(c, x, y + kRow, depth + 1);
            if (c >= 0 && c < count) x += width[c];
        }
    };
    measure(0, 0);
    place(0, -width[0] * 0.5f, 0.0f, 0);
    // Los sueltos (sin padre): a la derecha.
    float right = width[0] * 0.5f + 0.5f;
    for (int i = 1; i < count; ++i) {
        if (placed[i] || parentOf(i) >= 0) continue;
        measure(i, 0);
        place(i, right, 0.0f, 0);
        right += width[i];
    }
}

void BehaviorTreeAsset::renameKey(const std::string& from, const std::string& to) {
    const auto fix = [&](BtParams& p) {
        for (std::string* s : {&p.key, &p.key2, &p.key3}) {
            if (*s == from) *s = to;
        }
        if (p.value == "$" + from) p.value = "$" + to;
    };
    for (Variable& v : blackboard) {
        if (v.name == from) v.name = to;
    }
    for (BtNode& n : nodes) {
        fix(n.params);
        for (BtDecorator& d : n.decorators) fix(d.params);
        for (BtService& s : n.services) fix(s.params);
    }
}

std::vector<int> BehaviorTreeAsset::executionOrder() const {
    std::vector<int> out;
    std::vector<bool> seen(nodes.size(), false);
    std::function<void(int)> walk = [&](int n) {
        if (n < 0 || n >= static_cast<int>(nodes.size()) || seen[n]) return;
        seen[n] = true;
        out.push_back(n);
        for (const int c : nodes[n].children) walk(c);
    };
    walk(0);
    return out;
}

// --- JSON ---------------------------------------------------------------------------------

namespace {

json nodeJson(const BtNode& n) {
    json j = paramsJson(n.params, btParamsOf(n.kind));
    j["type"] = btNodeKey(n.kind);
    if (!n.name.empty()) j["name"] = n.name;
    if (!n.comment.empty()) j["comment"] = n.comment;
    j["position"] = vec2(n.position);
    j["children"] = n.children;
    if (!n.decorators.empty()) {
        json list = json::array();
        for (const BtDecorator& d : n.decorators) {
            json x = paramsJson(d.params, btParamsOf(d.kind));
            x["type"] = btDecoratorKey(d.kind);
            if (btIsCondition(d.kind)) x["abort"] = btAbortKey(d.abort);
            list.push_back(std::move(x));
        }
        j["decorators"] = std::move(list);
    }
    if (!n.services.empty()) {
        json list = json::array();
        for (const BtService& s : n.services) {
            json x = paramsJson(s.params, btParamsOf(s.kind));
            x["type"] = btServiceKey(s.kind);
            x["interval"] = s.interval;
            x["deviation"] = s.deviation;
            list.push_back(std::move(x));
        }
        j["services"] = std::move(list);
    }
    return j;
}

// Un nodo del JSON (sin los hijos).
BtNode readNode(const json& j) {
    BtNode n;
    n.kind = btNodeFromKey(j.value("type", std::string("sequence")));
    n.name = j.value("name", std::string{});
    n.comment = j.value("comment", std::string{});
    if (const auto it = j.find("position"); it != j.end()) n.position = readVec2(*it, n.position);
    n.params = readParams(j, btParamsOf(n.kind));
    if (const auto it = j.find("decorators"); it != j.end() && it->is_array()) {
        for (const json& x : *it) {
            if (!x.is_object()) continue;
            BtDecorator d;
            d.kind = btDecoratorFromKey(x.value("type", std::string("blackboard")));
            d.params = readParams(x, btParamsOf(d.kind));
            if (const auto a = x.find("abort"); a != x.end()) {
                d.abort = a->is_number() ? static_cast<BtAbort>(std::clamp(a->get<int>(), 0, kBtAbortCount - 1))
                                         : btAbortFromKey(jsonValueText(*a));
            }
            if (!btIsCondition(d.kind)) d.abort = BtAbort::None;
            n.decorators.push_back(std::move(d));
        }
    }
    if (const auto it = j.find("services"); it != j.end() && it->is_array()) {
        for (const json& x : *it) {
            if (!x.is_object()) continue;
            BtService s;
            s.kind = btServiceFromKey(x.value("type", std::string("distance_to")));
            s.params = readParams(x, btParamsOf(s.kind));
            if (const auto v = x.find("interval"); v != x.end() && v->is_number()) s.interval = std::max(0.0f, v->get<float>());
            if (const auto v = x.find("deviation"); v != x.end() && v->is_number()) s.deviation = std::max(0.0f, v->get<float>());
            n.services.push_back(std::move(s));
        }
    }
    return n;
}

// Arbol anidado ({"root": {type, children: [{...}, {...}]}}) -> lista plana.
int flatten(const json& j, std::vector<BtNode>& out, int depth) {
    if (!j.is_object() || depth > 64) return -1;
    out.push_back(readNode(j));
    const int index = static_cast<int>(out.size()) - 1;
    if (const auto it = j.find("children"); it != j.end() && it->is_array()) {
        for (const json& c : *it) {
            const int child = flatten(c, out, depth + 1);
            if (child >= 0) out[index].children.push_back(child);
        }
    }
    return index;
}

}  // namespace

std::string behaviorTreeToJson(const BehaviorTreeAsset& tree) {
    json j;
    j["uuid"] = (tree.uuid.valid() ? tree.uuid : Uuid::generate()).toString();
    j["type"] = "behavior_tree";
    json board = json::array();
    for (const Variable& v : tree.blackboard) {
        board.push_back(json{{"name", v.name}, {"type", varTypeKey(v.value.type)}, {"value", valueJson(v.value)}});
    }
    j["blackboard"] = std::move(board);
    json nodes = json::array();
    for (const BtNode& n : tree.nodes) nodes.push_back(nodeJson(n));
    j["nodes"] = std::move(nodes);
    return j.dump(2);
}

bool behaviorTreeFromJson(const std::string& text, BehaviorTreeAsset& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        if (error) *error = "JSON invalido";
        return false;
    }
    BehaviorTreeAsset t;
    if (const auto it = j.find("uuid"); it != j.end() && it->is_string()) t.uuid = Uuid::parse(it->get<std::string>());
    // Pizarra: [{name, type, value}] o {nombre: valor}.
    const auto board = j.find("blackboard");
    if (board != j.end() && board->is_array()) {
        for (const json& x : *board) {
            if (!x.is_object()) continue;
            Variable v;
            v.name = x.value("name", std::string{});
            if (v.name.empty()) continue;
            const VarType type = varTypeFromKey(x.value("type", std::string("float")));
            v.value = Value::parse(type, x.contains("value") ? jsonValueText(x["value"]) : std::string{});
            t.blackboard.push_back(std::move(v));
        }
    } else if (board != j.end() && board->is_object()) {
        for (const auto& [name, x] : board->items()) {
            Variable v;
            v.name = name;
            VarType type = VarType::String;
            if (x.is_boolean()) type = VarType::Bool;
            else if (x.is_number_integer()) type = VarType::Int;
            else if (x.is_number()) type = VarType::Float;
            else if (x.is_array()) type = VarType::Vector;
            v.value = Value::parse(type, jsonValueText(x));
            t.blackboard.push_back(std::move(v));
        }
    }
    if (const auto it = j.find("nodes"); it != j.end() && it->is_array()) {
        const int count = static_cast<int>(it->size());
        for (const json& x : *it) {
            BtNode n = x.is_object() ? readNode(x) : BtNode{};
            if (x.is_object()) {
                if (const auto c = x.find("children"); c != x.end() && c->is_array()) {
                    for (const json& child : *c) {
                        if (child.is_number_integer()) {
                            const int index = child.get<int>();
                            if (index >= 0 && index < count) n.children.push_back(index);
                        }
                    }
                }
            }
            t.nodes.push_back(std::move(n));
        }
    } else if (const auto it = j.find("root"); it != j.end() && it->is_object()) {
        // Anidado. Si "root" no es la Raiz, se pone una delante.
        if (btNodeFromKey(it->value("type", std::string("sequence"))) != BtNodeKind::Root) {
            BtNode root;
            root.kind = BtNodeKind::Root;
            t.nodes.push_back(std::move(root));
            const int child = flatten(*it, t.nodes, 1);
            if (child >= 0) t.nodes[0].children.push_back(child);
        } else {
            flatten(*it, t.nodes, 0);
        }
        // Sin posiciones: se colocan solas.
        bool any_position = false;
        std::function<void(const json&)> scan = [&](const json& n) {
            if (!n.is_object()) return;
            any_position = any_position || n.contains("position");
            if (const auto c = n.find("children"); c != n.end() && c->is_array()) {
                for (const json& x : *c) scan(x);
            }
        };
        scan(*it);
        t.ensureRoot();
        if (!any_position) t.autoLayout();
    }
    t.ensureRoot();
    // La raiz como mucho un hijo; ningun nodo puede apuntar a la raiz ni a si mismo.
    if (t.nodes[0].children.size() > 1) t.nodes[0].children.resize(1);
    for (int i = 0; i < static_cast<int>(t.nodes.size()); ++i) {
        std::erase_if(t.nodes[i].children, [&](int c) { return c <= 0 || c == i; });
        if (btIsTask(t.nodes[i].kind)) t.nodes[i].children.clear();
    }
    out = std::move(t);
    return true;
}

bool loadBehaviorTree(const std::filesystem::path& path, BehaviorTreeAsset& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    if (!behaviorTreeFromJson(ss.str(), out, nullptr)) {
        if (error) *error = "JSON danado en " + path.string();
        return false;
    }
    return true;
}

bool saveBehaviorTree(const BehaviorTreeAsset& tree, const std::filesystem::path& path, std::string* error) {
    return writeText(path, behaviorTreeToJson(tree), error);
}

std::vector<std::string> validateBehaviorTree(const BehaviorTreeAsset& t) {
    std::vector<std::string> problems;
    if (t.nodes.empty() || t.nodes[0].kind != BtNodeKind::Root) {
        problems.push_back("No hay Raiz");
        return problems;
    }
    if (t.nodes[0].children.empty()) problems.push_back("La Raiz no tiene hijo");
    std::set<std::string> names;
    for (const Variable& v : t.blackboard) {
        if (v.name.empty()) problems.push_back("Una clave de la pizarra no tiene nombre");
        else if (!names.insert(v.name).second) problems.push_back("Hay dos claves \"" + v.name + "\"");
    }
    const auto checkParams = [&](const std::string& where, const BtParams& p, const std::vector<BtParamInfo>& infos) {
        BtParams copy = p;
        for (const BtParamInfo& info : infos) {
            if (info.field != BtField::Key && info.field != BtField::Key2 && info.field != BtField::Key3) continue;
            const std::string& key = *btTextField(copy, info.field);
            if (key.empty()) continue;
            const Variable* v = t.findKey(key);
            if (v == nullptr) {
                problems.push_back(where + ": no existe la clave \"" + key + "\"");
            } else if (info.key_types != kAnyKey && (info.key_types & (1 << static_cast<int>(v->value.type))) == 0) {
                problems.push_back(where + ": la clave \"" + key + "\" es " + varTypeLabel(v->value.type) + " (" +
                                   info.label + " no la acepta)");
            }
        }
        if (copy.value.size() > 1 && copy.value[0] == '$' && t.findKey(copy.value.substr(1)) == nullptr) {
            problems.push_back(where + ": no existe la clave \"" + copy.value.substr(1) + "\"");
        }
    };
    std::vector<bool> reachable(t.nodes.size(), false);
    for (const int n : t.executionOrder()) reachable[n] = true;
    for (int i = 0; i < static_cast<int>(t.nodes.size()); ++i) {
        const BtNode& n = t.nodes[i];
        const std::string where = n.title() + " (#" + std::to_string(i) + ")";
        if (btIsComposite(n.kind) && n.children.empty()) problems.push_back(where + ": compuesto sin hijos");
        if (i > 0 && !reachable[i] && t.parentOf(i) < 0) problems.push_back(where + ": suelto (no cuelga de la Raiz)");
        if (n.kind == BtNodeKind::RunScript && n.params.text.empty()) problems.push_back(where + ": falta la tarea");
        if (n.kind == BtNodeKind::MoveTo && n.params.key.empty()) problems.push_back(where + ": falta el destino");
        checkParams(where, n.params, btParamsOf(n.kind));
        for (const BtDecorator& d : n.decorators) checkParams(where + " / " + btDecoratorLabel(d.kind), d.params, btParamsOf(d.kind));
        for (const BtService& s : n.services) checkParams(where + " / " + btServiceLabel(s.kind), s.params, btParamsOf(s.kind));
    }
    return problems;
}

BehaviorTreeAsset exampleGuardBehaviorTree() {
    BehaviorTreeAsset t;
    const auto key = [&](const char* name, VarType type, const char* value) {
        Variable v;
        v.name = name;
        v.value = Value::parse(type, value);
        t.blackboard.push_back(std::move(v));
    };
    key("objetivo", VarType::Entity, "");
    key("loVe", VarType::Bool, "false");
    key("ultimaPosicion", VarType::Vector, "0 0 0");
    key("buscando", VarType::Bool, "false");
    key("distancia", VarType::Float, "1000000");
    key("rangoAtaque", VarType::Float, "2");
    key("puntoPatrulla", VarType::Vector, "0 0 0");
    t.ensureRoot();

    const auto params = [](BtNodeKind kind) { return btDefaultParams(btParamsOf(kind)); };
    const auto condition = [](const char* k, Compare compare, const char* value, BtAbort abort) {
        BtDecorator d;
        d.kind = BtDecoratorKind::Blackboard;
        d.abort = abort;
        d.params = btDefaultParams(btParamsOf(d.kind));
        d.params.key = k;
        d.params.option = static_cast<int>(compare);
        d.params.value = value;
        return d;
    };

    const int top = t.addNode(BtNodeKind::Selector, 0);
    t.nodes[top].name = "Guardia";
    {
        BtService sight;
        sight.kind = BtServiceKind::Sight;
        sight.interval = 0.2f;
        sight.deviation = 0.05f;
        sight.params = btDefaultParams(btParamsOf(sight.kind));
        sight.params.text = "Player";
        sight.params.key = "objetivo";
        sight.params.key2 = "ultimaPosicion";
        sight.params.key3 = "loVe";
        t.nodes[top].services.push_back(std::move(sight));
    }

    // 1. Lo ve: perseguir y atacar (corta las ramas de la derecha al verlo).
    const int chase = t.addNode(BtNodeKind::Sequence, top);
    t.nodes[chase].name = "Perseguir y atacar";
    t.nodes[chase].decorators.push_back(condition("loVe", Compare::IsTrue, "", BtAbort::Both));
    {
        BtService distance;
        distance.kind = BtServiceKind::DistanceTo;
        distance.interval = 0.15f;
        distance.deviation = 0.0f;
        distance.params = btDefaultParams(btParamsOf(distance.kind));
        distance.params.key = "objetivo";
        distance.params.key2 = "distancia";
        t.nodes[chase].services.push_back(std::move(distance));
    }
    const int remember = t.addNode(BtNodeKind::SetBlackboard, chase);
    t.nodes[remember].params.key = "buscando";
    t.nodes[remember].params.value = "true";
    const int act = t.addNode(BtNodeKind::Selector, chase);
    const int attack = t.addNode(BtNodeKind::Sequence, act);
    t.nodes[attack].name = "Atacar";
    t.nodes[attack].decorators.push_back(condition("distancia", Compare::LessEqual, "$rangoAtaque", BtAbort::Self));
    const int face = t.addNode(BtNodeKind::RotateToFace, attack);
    t.nodes[face].params.key = "objetivo";
    const int hit = t.addNode(BtNodeKind::SendMessage, attack);
    t.nodes[hit].params.text = "Atacar";
    {
        BtDecorator cooldown;
        cooldown.kind = BtDecoratorKind::Cooldown;
        cooldown.params = btDefaultParams(btParamsOf(cooldown.kind));
        cooldown.params.number = 1.0f;
        t.nodes[hit].decorators.push_back(std::move(cooldown));
    }
    const int pause = t.addNode(BtNodeKind::Wait, attack);
    t.nodes[pause].params.number = 0.4f;
    const int go = t.addNode(BtNodeKind::MoveTo, act);
    t.nodes[go].params.key = "objetivo";
    t.nodes[go].params.number = 1.5f;

    // 2. Lo perdio: ir a donde lo vio por ultima vez y mirar.
    const int search = t.addNode(BtNodeKind::Sequence, top);
    t.nodes[search].name = "Buscar";
    t.nodes[search].decorators.push_back(condition("buscando", Compare::IsTrue, "", BtAbort::None));
    {
        BtDecorator limit;
        limit.kind = BtDecoratorKind::TimeLimit;
        limit.params = btDefaultParams(btParamsOf(limit.kind));
        limit.params.number = 12.0f;
        t.nodes[search].decorators.push_back(std::move(limit));
    }
    const int last = t.addNode(BtNodeKind::MoveTo, search);
    t.nodes[last].params.key = "ultimaPosicion";
    t.nodes[last].params.flag = false;
    const int look = t.addNode(BtNodeKind::Wait, search);
    t.nodes[look].params.number = 2.0f;
    t.nodes[look].params.number2 = 0.5f;
    const int forget = t.addNode(BtNodeKind::SetBlackboard, search);
    t.nodes[forget].params.key = "buscando";
    t.nodes[forget].params.value = "false";

    // 3. Nada: patrullar puntos al azar de la malla.
    const int patrol = t.addNode(BtNodeKind::Sequence, top);
    t.nodes[patrol].name = "Patrullar";
    const int point = t.addNode(BtNodeKind::FindRandomPoint, patrol);
    t.nodes[point].params.key = "puntoPatrulla";
    t.nodes[point].params.number = 12.0f;
    const int walk = t.addNode(BtNodeKind::MoveTo, patrol);
    t.nodes[walk].params.key = "puntoPatrulla";
    t.nodes[walk].params.flag = false;
    const int idle = t.addNode(BtNodeKind::Wait, patrol);
    t.nodes[idle].params = params(BtNodeKind::Wait);
    t.nodes[idle].params.number = 1.5f;
    t.nodes[idle].params.number2 = 1.0f;
    t.autoLayout();
    return t;
}

// --- Ejecucion -------------------------------------------------------------------------

Value* BtRuntime::find(const std::string& name) {
    for (Variable& v : vars) {
        if (v.name == name) return &v.value;
    }
    return nullptr;
}

const Value* BtRuntime::find(const std::string& name) const {
    for (const Variable& v : vars) {
        if (v.name == name) return &v.value;
    }
    return nullptr;
}

std::uint32_t btResolveEntity(const ecs::World& world, const std::string& text) {
    if (text.empty()) return kNoEntity;
    ecs::Entity e;
    if (const Uuid uuid = Uuid::parse(text); uuid.valid()) e = world.find(uuid);
    if (!e.valid()) e = world.findByName(text);
    if (!e.valid()) e = world.findWithTag(text);
    return e.valid() ? static_cast<std::uint32_t>(entt::to_integral(e.handle())) : kNoEntity;
}

namespace {

ecs::Entity entityOf(const ecs::World* world, std::uint32_t handle) {
    if (world == nullptr || handle == kNoEntity) return {};
    const entt::entity h = static_cast<entt::entity>(handle);
    if (!world->valid(h)) return {};
    return world->wrap(h);
}

void setEntity(Value& v, const ecs::Entity& e) {
    if (v.type != VarType::Entity) return;
    v.entity = e.valid() ? static_cast<std::uint32_t>(entt::to_integral(e.handle())) : kNoEntity;
    v.s = e.valid() ? e.name() : std::string{};
}

void clearValue(Value& v) {
    v.b = false;
    v.n = 0.0;
    v.s.clear();
    v.v = core::Vec3{};
    v.entity = kNoEntity;
}

float horizontalDistance(const core::Vec3& a, const core::Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float wrapDegrees(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

// Yaw (grados) para que -Z mire en la direccion d.
float yawOf(const core::Vec3& d) { return std::atan2(-d.x, -d.z) * 180.0f / core::kPi; }

}  // namespace

bool btKeyPosition(const ecs::World& world, const BtRuntime& runtime, const std::string& key, core::Vec3& out) {
    const Value* v = runtime.find(key);
    if (v == nullptr) return false;
    if (v->type == VarType::Vector) {
        out = v->v;
        return true;
    }
    if (v->type == VarType::Entity) {
        const ecs::Entity e = entityOf(&world, v->entity);
        if (!e.valid()) return false;
        out = e.worldPosition();
        return true;
    }
    return false;
}

bool btCompare(const std::vector<Variable>& vars, const std::string& key, Compare compare, const std::string& value) {
    const auto find = [&](const std::string& name) -> const Value* {
        for (const Variable& v : vars) {
            if (v.name == name) return &v.value;
        }
        return nullptr;
    };
    const Value* left = find(key);
    if (left == nullptr) return false;
    if (compare == Compare::IsTrue) return left->truthy();
    if (compare == Compare::IsFalse) return !left->truthy();
    Value right;
    const bool other = value.size() > 1 && value[0] == '$';
    if (other) {
        const Value* r = find(value.substr(1));
        if (r == nullptr) return false;
        right = *r;
    } else {
        right = Value::parse(left->type, value);
    }
    int order = 0;
    switch (left->type) {
        case VarType::String: {
            const std::string r = right.type == VarType::String || right.type == VarType::Entity ? right.s : right.text();
            order = left->s.compare(r);
            break;
        }
        case VarType::Entity: {
            if (compare != Compare::Equal && compare != Compare::NotEqual) return false;
            if (other && right.type == VarType::Entity) order = left->entity == right.entity ? 0 : 1;
            else order = left->s.compare(right.s);
            break;
        }
        case VarType::Vector: {
            if (compare == Compare::Equal || compare == Compare::NotEqual) {
                const core::Vec3 r = right.type == VarType::Vector ? right.v : core::Vec3{};
                const bool same = std::abs(left->v.x - r.x) < 1e-4f && std::abs(left->v.y - r.y) < 1e-4f &&
                                  std::abs(left->v.z - r.z) < 1e-4f;
                return (compare == Compare::Equal) == same;
            }
            const double a = left->number();
            const double b = right.type == VarType::Vector ? right.number() : std::strtod(value.c_str(), nullptr);
            order = a < b ? -1 : (a > b ? 1 : 0);
            break;
        }
        default: {
            const double a = left->number();
            const double b = right.number();
            if (left->type == VarType::Float && (compare == Compare::Equal || compare == Compare::NotEqual)) {
                order = std::abs(a - b) < 1e-6 ? 0 : (a < b ? -1 : 1);
            } else {
                order = a < b ? -1 : (a > b ? 1 : 0);
            }
            break;
        }
    }
    switch (compare) {
        case Compare::Equal: return order == 0;
        case Compare::NotEqual: return order != 0;
        case Compare::Greater: return order > 0;
        case Compare::Less: return order < 0;
        case Compare::GreaterEqual: return order >= 0;
        case Compare::LessEqual: return order <= 0;
        default: return false;
    }
}

void resetBehaviorTree(const BehaviorTreeAsset& tree, BtRuntime& runtime, const std::vector<VariableOverride>& overrides,
                       const ecs::World* world) {
    const bool running = runtime.running;
    const std::mt19937 rng = runtime.rng;
    runtime = BtRuntime{};
    runtime.running = running;
    runtime.rng = rng;
    runtime.vars = tree.blackboard;
    for (const VariableOverride& o : overrides) {
        if (Value* v = runtime.find(o.name)) *v = Value::parse(v->type, o.value);
    }
    if (world != nullptr) {
        for (Variable& v : runtime.vars) {
            if (v.value.type == VarType::Entity) v.value.entity = btResolveEntity(*world, v.value.s);
        }
    }
    runtime.nodes.assign(tree.nodes.size(), BtNodeState{});
}

namespace {

// Ejecuta un frame del arbol para un objeto.
class Executor {
public:
    Executor(const BehaviorTreeAsset& tree, BtRuntime& rt, BtContext& ctx) : t_(tree), rt_(rt), ctx_(ctx) {}

    BtStatus tick(int n, int depth = 0) {
        if (n < 0 || n >= count() || depth > 64) return BtStatus::Failure;
        BtNodeState& s = rt_.nodes[n];
        const BtNode& node = t_.nodes[n];
        s.service_next.resize(node.services.size(), 0.0f);
        s.cooldown_until.resize(node.decorators.size(), -1.0e9f);

        if (!s.active) {
            // Entrar: todas las condiciones tienen que cumplirse.
            for (int k = 0; k < static_cast<int>(node.decorators.size()); ++k) {
                if (btIsCondition(node.decorators[k].kind) && !condition(n, k)) {
                    s.status = BtStatus::Failure;
                    s.finish_time = rt_.time;
                    return BtStatus::Failure;
                }
            }
            enter(n);
        } else {
            // Observer abort "Self": la condicion dejo de cumplirse.
            for (int k = 0; k < static_cast<int>(node.decorators.size()); ++k) {
                const BtDecorator& d = node.decorators[k];
                if (!btIsCondition(d.kind) || (d.abort != BtAbort::Self && d.abort != BtAbort::Both)) continue;
                if (!condition(n, k)) {
                    note(node.title() + ": abortado (" + btDecoratorLabel(d.kind) + " ya no se cumple)");
                    abortSubtree(n);
                    startCooldowns(n);
                    return BtStatus::Failure;
                }
            }
        }
        // Time Limit.
        for (const BtDecorator& d : node.decorators) {
            if (d.kind == BtDecoratorKind::TimeLimit && rt_.time - s.enter_time >= d.params.number) {
                note(node.title() + ": tiempo agotado");
                abortSubtree(n);
                startCooldowns(n);
                return BtStatus::Failure;
            }
        }
        // Servicios (el primero al entrar).
        for (int k = 0; k < static_cast<int>(node.services.size()); ++k) {
            if (rt_.time + 1e-6f < s.service_next[k]) continue;
            const BtService& service = node.services[k];
            runService(n, k);
            std::uniform_real_distribution<float> spread(-service.deviation, service.deviation);
            const float jitter = service.deviation > 0.0f ? spread(rt_.rng) : 0.0f;
            rt_.nodes[n].service_next[k] = rt_.time + std::max(0.0f, service.interval + jitter);
        }

        BtStatus r = run(n, depth);
        rt_.nodes[n].fresh = false;
        if (r == BtStatus::Running) return r;

        // Loop: vuelve a empezar la rama mientras salga bien.
        for (const BtDecorator& d : node.decorators) {
            if (d.kind != BtDecoratorKind::Loop || r != BtStatus::Success) continue;
            BtNodeState& state = rt_.nodes[n];
            const int times = static_cast<int>(std::lround(d.params.number));
            ++state.loops;
            if (times <= 0 || state.loops < times) {
                const int loops = state.loops;
                resetChildren(n);
                restart(n);
                rt_.nodes[n].loops = loops;
                return BtStatus::Running;
            }
        }
        return finish(n, r);
    }

    void abortSubtree(int n, int depth = 0) {
        if (n < 0 || n >= count() || depth > 64) return;
        BtNodeState& s = rt_.nodes[n];
        for (const int c : t_.nodes[n].children) abortSubtree(c, depth + 1);
        if (!s.active) return;
        exitTask(n, true);
        s.active = false;
        s.status = BtStatus::Aborted;
        s.finish_time = rt_.time;
    }

private:
    const BehaviorTreeAsset& t_;
    BtRuntime& rt_;
    BtContext& ctx_;

    int count() const { return static_cast<int>(t_.nodes.size()); }

    ecs::Entity self() const { return entityOf(ctx_.world, ctx_.self); }

    void note(const std::string& text) {
        rt_.history.push_back(text);
        if (rt_.history.size() > 16) rt_.history.erase(rt_.history.begin());
        if (ctx_.debug && ctx_.log) ctx_.log(0, "[Behavior Tree] " + text);
    }

    void enter(int n) {
        BtNodeState& s = rt_.nodes[n];
        s.active = true;
        s.fresh = true;
        s.status = BtStatus::Running;
        s.enter_time = rt_.time;
        s.child = 0;
        s.loops = 0;
        s.done.clear();
        s.moving = false;
        ++s.runs;
        for (float& next : s.service_next) next = rt_.time;  // los servicios corren al entrar
        const BtNode& node = t_.nodes[n];
        s.order.resize(node.children.size());
        for (std::size_t i = 0; i < s.order.size(); ++i) s.order[i] = static_cast<int>(i);
        if (node.kind == BtNodeKind::RandomSelector) std::shuffle(s.order.begin(), s.order.end(), rt_.rng);
        if (node.kind == BtNodeKind::RunScript) rt_.pending_finish = BtStatus::Idle;
    }

    // Loop: la misma rama otra vez (sin volver a mirar las condiciones).
    void restart(int n) {
        BtNodeState& s = rt_.nodes[n];
        s.fresh = true;
        s.child = 0;
        s.done.clear();
        s.moving = false;
        s.enter_time = rt_.time;
        const BtNode& node = t_.nodes[n];
        if (node.kind == BtNodeKind::RandomSelector) std::shuffle(s.order.begin(), s.order.end(), rt_.rng);
    }

    void resetChildren(int n) {
        for (const int c : t_.nodes[n].children) abortSubtree(c);
    }

    void startCooldowns(int n) {
        const BtNode& node = t_.nodes[n];
        BtNodeState& s = rt_.nodes[n];
        for (int k = 0; k < static_cast<int>(node.decorators.size()); ++k) {
            if (node.decorators[k].kind == BtDecoratorKind::Cooldown) {
                s.cooldown_until[k] = rt_.time + node.decorators[k].params.number;
            }
        }
    }

    BtStatus finish(int n, BtStatus r) {
        const BtNode& node = t_.nodes[n];
        startCooldowns(n);
        for (const BtDecorator& d : node.decorators) {
            if (d.kind == BtDecoratorKind::Inverter) {
                r = r == BtStatus::Success ? BtStatus::Failure : BtStatus::Success;
            } else if (d.kind == BtDecoratorKind::ForceSuccess) {
                r = BtStatus::Success;
            } else if (d.kind == BtDecoratorKind::ForceFailure) {
                r = BtStatus::Failure;
            }
        }
        exitTask(n, false);
        BtNodeState& s = rt_.nodes[n];
        s.active = false;
        s.status = r;
        s.finish_time = rt_.time;
        if (r == BtStatus::Failure && btIsTask(node.kind) && ctx_.debug) note(node.title() + ": fallo");
        return r;
    }

    void stopMoving(int n) {
        BtNodeState& s = rt_.nodes[n];
        if (!s.moving) return;
        s.moving = false;
        const ecs::Entity me = self();
        if (ctx_.navigation != nullptr && me.valid()) ctx_.navigation->stop(me);
    }

    void exitTask(int n, bool aborted) {
        const BtNode& node = t_.nodes[n];
        if (node.kind == BtNodeKind::MoveTo) stopMoving(n);
        if (aborted && node.kind == BtNodeKind::RunScript && ctx_.script_abort) ctx_.script_abort(n, node.params.text);
    }

    // --- Decoradores ---
    bool condition(int n, int k) {
        const BtDecorator& d = t_.nodes[n].decorators[k];
        switch (d.kind) {
            case BtDecoratorKind::Blackboard:
                return btCompare(rt_.vars, d.params.key, static_cast<Compare>(std::clamp(d.params.option, 0, kCompareCount - 1)),
                                 d.params.value);
            case BtDecoratorKind::ScriptCondition:
                return ctx_.script_condition ? ctx_.script_condition(n, k, d.params.text) : false;
            case BtDecoratorKind::IsAtLocation: {
                const ecs::Entity me = self();
                core::Vec3 target{};
                if (!me.valid() || ctx_.world == nullptr || !btKeyPosition(*ctx_.world, rt_, d.params.key, target)) {
                    return d.params.flag;
                }
                const bool near_enough = horizontalDistance(me.worldPosition(), target) <= d.params.number;
                return d.params.flag ? !near_enough : near_enough;
            }
            case BtDecoratorKind::Cooldown: {
                const BtNodeState& s = rt_.nodes[n];
                return k >= static_cast<int>(s.cooldown_until.size()) || rt_.time >= s.cooldown_until[k];
            }
            default: return true;
        }
    }

    bool allConditions(int n) {
        BtNodeState& s = rt_.nodes[n];
        const BtNode& node = t_.nodes[n];
        s.cooldown_until.resize(node.decorators.size(), -1.0e9f);
        for (int k = 0; k < static_cast<int>(node.decorators.size()); ++k) {
            if (btIsCondition(node.decorators[k].kind) && !condition(n, k)) return false;
        }
        return true;
    }

    bool observesLowerPriority(int n) const {
        for (const BtDecorator& d : t_.nodes[n].decorators) {
            if (btIsCondition(d.kind) && (d.abort == BtAbort::LowerPriority || d.abort == BtAbort::Both)) return true;
        }
        return false;
    }

    // --- Nodos ---
    int childAt(int n, int position) const {
        const BtNode& node = t_.nodes[n];
        const BtNodeState& s = rt_.nodes[n];
        if (position < 0 || position >= static_cast<int>(node.children.size())) return -1;
        const int slot = position < static_cast<int>(s.order.size()) ? s.order[position] : position;
        return slot >= 0 && slot < static_cast<int>(node.children.size()) ? node.children[slot] : -1;
    }

    BtStatus run(int n, int depth) {
        const BtNode& node = t_.nodes[n];
        switch (node.kind) {
            case BtNodeKind::Root:
                return node.children.empty() ? BtStatus::Failure : tick(node.children[0], depth + 1);
            case BtNodeKind::Selector:
            case BtNodeKind::RandomSelector: return runSelector(n, depth);
            case BtNodeKind::Sequence: return runSequence(n, depth);
            case BtNodeKind::Parallel: return runParallel(n, depth);
            default: return runTask(n);
        }
    }

    BtStatus runSelector(int n, int depth) {
        const int children = static_cast<int>(t_.nodes[n].children.size());
        // Observer abort "Lower Priority": una rama de la izquierda vuelve a poder entrar.
        {
            BtNodeState& s = rt_.nodes[n];
            const int current = childAt(n, s.child);
            if (current >= 0 && rt_.nodes[current].active) {
                for (int j = 0; j < s.child; ++j) {
                    const int c = childAt(n, j);
                    if (c < 0 || !observesLowerPriority(c) || !allConditions(c)) continue;
                    note(t_.nodes[c].title() + ": interrumpe a " + t_.nodes[current].title() + " (Lower Priority)");
                    abortSubtree(current);
                    rt_.nodes[n].child = j;
                    break;
                }
            }
        }
        while (rt_.nodes[n].child < children) {
            const int c = childAt(n, rt_.nodes[n].child);
            const BtStatus r = tick(c, depth + 1);
            if (r == BtStatus::Running) return r;
            if (r == BtStatus::Success) return r;
            ++rt_.nodes[n].child;
        }
        return BtStatus::Failure;
    }

    BtStatus runSequence(int n, int depth) {
        const int children = static_cast<int>(t_.nodes[n].children.size());
        if (children == 0) return BtStatus::Failure;
        while (rt_.nodes[n].child < children) {
            const int c = childAt(n, rt_.nodes[n].child);
            const BtStatus r = tick(c, depth + 1);
            if (r == BtStatus::Running) return r;
            if (r != BtStatus::Success) return BtStatus::Failure;
            ++rt_.nodes[n].child;
        }
        return BtStatus::Success;
    }

    BtStatus runParallel(int n, int depth) {
        const BtNode& node = t_.nodes[n];
        const int children = static_cast<int>(node.children.size());
        if (children == 0) return BtStatus::Failure;
        if (static_cast<int>(rt_.nodes[n].done.size()) != children) rt_.nodes[n].done.assign(children, BtStatus::Idle);
        const int policy = std::clamp(node.params.option, 0, 2);
        for (int i = 0; i < children; ++i) {
            BtStatus& done = rt_.nodes[n].done[i];
            const bool finished = done == BtStatus::Success || done == BtStatus::Failure;
            // Principal: los de fondo se repiten mientras el primero sigue.
            if (finished && policy == 2 && i > 0) done = BtStatus::Idle;
            else if (finished) continue;
            const BtStatus r = tick(node.children[i], depth + 1);
            rt_.nodes[n].done[i] = r;
        }
        const std::vector<BtStatus>& done = rt_.nodes[n].done;
        const auto abortRunning = [&] {
            for (int i = 0; i < children; ++i) abortSubtree(node.children[i]);
        };
        if (policy == 2) {
            if (done[0] == BtStatus::Success || done[0] == BtStatus::Failure) {
                const BtStatus r = done[0];
                abortRunning();
                return r;
            }
            return BtStatus::Running;
        }
        int ok = 0;
        int bad = 0;
        for (const BtStatus r : done) {
            ok += r == BtStatus::Success ? 1 : 0;
            bad += r == BtStatus::Failure ? 1 : 0;
        }
        if (policy == 0) {
            if (bad > 0) {
                abortRunning();
                return BtStatus::Failure;
            }
            return ok == children ? BtStatus::Success : BtStatus::Running;
        }
        if (ok > 0) {
            abortRunning();
            return BtStatus::Success;
        }
        return bad == children ? BtStatus::Failure : BtStatus::Running;
    }

    // "{clave}" -> su valor.
    std::string interpolate(const std::string& text) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '{') {
                const std::size_t close = text.find('}', i);
                if (close != std::string::npos) {
                    const std::string key = text.substr(i + 1, close - i - 1);
                    if (const Value* v = rt_.find(key)) {
                        out += v->type == VarType::Entity && v->entity == kNoEntity ? std::string("(ninguno)") : v->text();
                        i = close;
                        continue;
                    }
                }
            }
            out += text[i];
        }
        return out;
    }

    // "$clave" -> el texto de su valor; si no, el texto tal cual.
    std::string resolved(const std::string& value) const {
        if (value.size() > 1 && value[0] == '$') {
            if (const Value* v = rt_.find(value.substr(1))) return v->text();
        }
        return value;
    }

    void face(ecs::Entity& me, const core::Vec3& direction) {
        const core::Vec3 flat{direction.x, 0.0f, direction.z};
        if (core::length(flat) < 1e-4f) return;
        const core::Vec3 forward = me.forward();
        const float delta = wrapDegrees(yawOf(flat) - yawOf(core::Vec3{forward.x, 0.0f, forward.z}));
        core::Vec3 euler = me.localEulerDegrees();
        euler.y += delta;
        me.setLocalEulerDegrees(euler);
    }

    BtStatus runTask(int n) {
        const BtNode& node = t_.nodes[n];
        const BtParams& p = node.params;
        BtNodeState& s = rt_.nodes[n];
        ecs::Entity me = self();
        switch (node.kind) {
            case BtNodeKind::Wait: {
                if (s.fresh) {
                    std::uniform_real_distribution<float> spread(-p.number2, p.number2);
                    s.wait = std::max(0.0f, p.number + (p.number2 > 0.0f ? spread(rt_.rng) : 0.0f));
                }
                return rt_.time - s.enter_time + 1e-4f >= s.wait ? BtStatus::Success : BtStatus::Running;
            }
            case BtNodeKind::MoveTo: {
                if (!me.valid() || ctx_.world == nullptr) return BtStatus::Failure;
                core::Vec3 target{};
                if (!btKeyPosition(*ctx_.world, rt_, p.key, target)) return BtStatus::Failure;
                if (s.fresh) s.move_target = target;
                else if (!p.flag) target = s.move_target;  // sin seguir: el destino del principio
                const float accept = std::max(p.number, 0.05f);
                const core::Vec3 position = me.worldPosition();
                const float distance = horizontalDistance(position, target);
                if (distance <= accept) {
                    stopMoving(n);
                    return BtStatus::Success;
                }
                const navigation::NavAgent* agent = me.tryGet<navigation::NavAgent>();
                if (ctx_.navigation != nullptr && agent != nullptr) {
                    const bool moved = horizontalDistance(target, s.move_target) > std::max(0.5f, accept * 0.5f);
                    if (!s.moving || (p.flag && moved)) {
                        if (!ctx_.navigation->moveTo(me, target)) {
                            note(node.title() + ": no hay camino");
                            return BtStatus::Failure;
                        }
                        s.moving = true;
                        s.move_target = target;
                        s.wait = rt_.time;  // cuando se pidio
                        return BtStatus::Running;
                    }
                    if (!ctx_.navigation->isMoving(me) && rt_.time - s.wait > 0.5f) {
                        s.moving = false;
                        // Se paro: llego (a su distancia de parada) o no puede seguir.
                        return distance <= accept + agent->stopping_distance + 0.35f ? BtStatus::Success : BtStatus::Failure;
                    }
                    return BtStatus::Running;
                }
                // Sin NavAgent: en linea recta, mirando hacia donde va.
                const float speed = p.number2 > 0.0f ? p.number2 : 3.5f;
                const core::Vec3 direction = core::normalize(core::Vec3{target.x - position.x, 0.0f, target.z - position.z});
                const float step = std::min(speed * ctx_.dt, distance);
                me.setWorldPosition(position + direction * step);
                face(me, direction);
                return horizontalDistance(me.worldPosition(), target) <= accept ? BtStatus::Success : BtStatus::Running;
            }
            case BtNodeKind::PlayAnimation: {
                ecs::Animator* animator = me.valid() ? me.tryGet<ecs::Animator>() : nullptr;
                if (animator == nullptr) return BtStatus::Failure;
                if (s.fresh) {
                    animator->clip_name = p.text;
                    animator->loop = p.flag;
                    animator->playing = true;
                    animator->time = 0.0f;
                }
                if (p.number <= 0.0f) return BtStatus::Success;
                return rt_.time - s.enter_time + 1e-4f >= p.number ? BtStatus::Success : BtStatus::Running;
            }
            case BtNodeKind::SetAnimatorParam: {
                ecs::Animator* animator = me.valid() ? me.tryGet<ecs::Animator>() : nullptr;
                if (animator == nullptr || p.text.empty()) return BtStatus::Failure;
                const std::string value = resolved(p.value);
                switch (std::clamp(p.option, 0, 3)) {
                    case 0: animator->setFloat(p.text, static_cast<float>(std::strtod(value.c_str(), nullptr))); break;
                    case 1: animator->setBool(p.text, Value::parse(VarType::Bool, value).b); break;
                    case 2: animator->setInt(p.text, static_cast<int>(std::lround(std::strtod(value.c_str(), nullptr)))); break;
                    default: animator->setTrigger(p.text); break;
                }
                return BtStatus::Success;
            }
            case BtNodeKind::RotateToFace: {
                if (!me.valid() || ctx_.world == nullptr) return BtStatus::Failure;
                core::Vec3 target{};
                if (!btKeyPosition(*ctx_.world, rt_, p.key, target)) return BtStatus::Failure;
                const core::Vec3 position = me.worldPosition();
                const core::Vec3 flat{target.x - position.x, 0.0f, target.z - position.z};
                if (core::length(flat) < 1e-4f) return BtStatus::Success;
                const core::Vec3 forward = me.forward();
                const float delta = wrapDegrees(yawOf(flat) - yawOf(core::Vec3{forward.x, 0.0f, forward.z}));
                const float tolerance = std::max(p.number2, 0.1f);
                if (std::abs(delta) <= tolerance) return BtStatus::Success;
                const float limit = p.number > 0.0f ? p.number * ctx_.dt : 360.0f;
                const float step = std::clamp(delta, -limit, limit);
                core::Vec3 euler = me.localEulerDegrees();
                euler.y += step;
                me.setLocalEulerDegrees(euler);
                return std::abs(delta - step) <= tolerance ? BtStatus::Success : BtStatus::Running;
            }
            case BtNodeKind::SetBlackboard: {
                Value* v = rt_.find(p.key);
                if (v == nullptr) return BtStatus::Failure;
                if (p.flag) {
                    clearValue(*v);
                } else if (p.value.size() > 1 && p.value[0] == '$') {
                    const Value* other = rt_.find(p.value.substr(1));
                    if (other == nullptr) return BtStatus::Failure;
                    if (other->type == v->type) *v = *other;
                    else *v = Value::parse(v->type, other->text());
                } else {
                    *v = Value::parse(v->type, p.value);
                    if (v->type == VarType::Entity && ctx_.world != nullptr) v->entity = btResolveEntity(*ctx_.world, v->s);
                }
                return BtStatus::Success;
            }
            case BtNodeKind::FindRandomPoint: {
                Value* out = rt_.find(p.key);
                if (out == nullptr || out->type != VarType::Vector || !me.valid() || ctx_.world == nullptr) return BtStatus::Failure;
                core::Vec3 center = me.worldPosition();
                if (!p.key2.empty()) btKeyPosition(*ctx_.world, rt_, p.key2, center);
                const float radius = std::max(p.number, 0.1f);
                core::Vec3 point{};
                if (ctx_.navigation != nullptr && ctx_.navigation->ready() && ctx_.navigation->randomPoint(center, radius, point)) {
                    out->v = point;
                    return BtStatus::Success;
                }
                std::uniform_real_distribution<float> unit(0.0f, 1.0f);
                const float angle = unit(rt_.rng) * 2.0f * core::kPi;
                const float r = radius * std::sqrt(unit(rt_.rng));
                out->v = core::Vec3{center.x + std::cos(angle) * r, center.y, center.z + std::sin(angle) * r};
                return BtStatus::Success;
            }
            case BtNodeKind::RunScript: {
                if (!s.fresh && rt_.pending_finish != BtStatus::Idle) {
                    const BtStatus r = rt_.pending_finish;
                    rt_.pending_finish = BtStatus::Idle;
                    return r == BtStatus::Success ? BtStatus::Success : BtStatus::Failure;
                }
                if (!ctx_.script_task || p.text.empty()) return BtStatus::Failure;
                const BtStatus r = ctx_.script_task(n, p.text, s.fresh);
                if (r == BtStatus::Idle) {
                    note(node.title() + ": no existe la tarea \"" + p.text + "\"");
                    return BtStatus::Failure;
                }
                if (r == BtStatus::Running && rt_.pending_finish != BtStatus::Idle) {
                    const BtStatus done = rt_.pending_finish;
                    rt_.pending_finish = BtStatus::Idle;
                    return done == BtStatus::Success ? BtStatus::Success : BtStatus::Failure;
                }
                return r;
            }
            case BtNodeKind::SendMessage: {
                std::uint32_t target = ctx_.self;
                if (!p.key.empty()) {
                    const Value* v = rt_.find(p.key);
                    if (v == nullptr || v->type != VarType::Entity || v->entity == kNoEntity) return BtStatus::Failure;
                    target = v->entity;
                }
                if (ctx_.send_message) ctx_.send_message(target, p.text, resolved(p.value));
                return BtStatus::Success;
            }
            case BtNodeKind::Log: {
                if (ctx_.log) ctx_.log(std::clamp(p.option, 0, 2), interpolate(p.text));
                return BtStatus::Success;
            }
            default: return BtStatus::Failure;
        }
    }

    // --- Servicios ---
    void runService(int n, int k) {
        const BtService& service = t_.nodes[n].services[k];
        const BtParams& p = service.params;
        const ecs::Entity me = self();
        if (ctx_.world == nullptr || !me.valid()) return;
        const core::Vec3 position = me.worldPosition();
        switch (service.kind) {
            case BtServiceKind::FindNearestWithTag: {
                ecs::Entity best;
                float best_distance = kNotFound;
                if (!p.text.empty()) {
                    for (const ecs::Entity& e : ctx_.world->findAllWithTag(p.text)) {
                        if (e == me || !e.activeInHierarchy()) continue;
                        const float d = core::length(e.worldPosition() - position);
                        if (p.number > 0.0f && d > p.number) continue;
                        if (d < best_distance) {
                            best_distance = d;
                            best = e;
                        }
                    }
                }
                if (Value* v = rt_.find(p.key)) setEntity(*v, best);
                if (Value* v = rt_.find(p.key2); v != nullptr && (v->type == VarType::Float || v->type == VarType::Int)) {
                    v->n = best_distance;
                }
                break;
            }
            case BtServiceKind::DistanceTo: {
                core::Vec3 target{};
                const float d = btKeyPosition(*ctx_.world, rt_, p.key, target) ? core::length(target - position) : kNotFound;
                if (Value* v = rt_.find(p.key2); v != nullptr && (v->type == VarType::Float || v->type == VarType::Int)) {
                    v->n = v->type == VarType::Int ? static_cast<double>(std::lround(d)) : static_cast<double>(d);
                }
                break;
            }
            case BtServiceKind::Sight: {
                const core::Vec3 eye = position + core::Vec3{0.0f, p.number3, 0.0f};
                const core::Vec3 forward = core::normalize(me.forward());
                const float half_angle = std::clamp(p.number2, 1.0f, 360.0f) * 0.5f;
                const float cos_limit = std::cos(half_angle * core::kPi / 180.0f);
                ecs::Entity best;
                float best_distance = kNotFound;
                core::Vec3 best_position{};
                if (!p.text.empty()) {
                    for (const ecs::Entity& e : ctx_.world->findAllWithTag(p.text)) {
                        if (e == me || !e.activeInHierarchy()) continue;
                        const core::Vec3 target = e.worldPosition() + core::Vec3{0.0f, std::min(p.number3, 1.0f), 0.0f};
                        const core::Vec3 to = target - eye;
                        const float d = core::length(to);
                        if (d > p.number || d >= best_distance) continue;
                        if (half_angle < 180.0f && d > 1e-3f && core::dot(forward, to * (1.0f / d)) < cos_limit) continue;
                        if (p.flag && ctx_.physics != nullptr && d > 1e-3f) {
                            physics::QueryFilter filter;
                            filter.ignore = me;
                            filter.record = false;
                            physics::RaycastHit hit;
                            if (ctx_.physics->raycast(eye, to * (1.0f / d), d, hit, filter)) {
                                const bool same = hit.entity == e || e.isAncestorOf(hit.entity) || hit.entity.isAncestorOf(e) ||
                                                  me.isAncestorOf(hit.entity);
                                if (!same && hit.distance < d - 0.3f) continue;  // una pared en medio
                            }
                        }
                        best = e;
                        best_distance = d;
                        best_position = e.worldPosition();
                    }
                }
                if (Value* v = rt_.find(p.key)) setEntity(*v, best);
                if (best.valid()) {
                    if (Value* v = rt_.find(p.key2); v != nullptr && v->type == VarType::Vector) v->v = best_position;
                }
                if (Value* v = rt_.find(p.key3); v != nullptr && v->type == VarType::Bool) v->b = best.valid();
                break;
            }
            case BtServiceKind::Hearing: {
                bool heard = false;
                ecs::Entity who;
                core::Vec3 where{};
                float best = kNotFound;
                if (ctx_.noises != nullptr) {
                    for (const BtNoise& noise : *ctx_.noises) {
                        if (ctx_.now - noise.time > std::max(p.number3, 0.0f) + 1e-4f) continue;
                        if (noise.instigator == ctx_.self) continue;
                        const float d = core::length(noise.position - position);
                        if (d > noise.radius * std::max(p.number, 0.0f) || d >= best) continue;
                        heard = true;
                        best = d;
                        where = noise.position;
                        who = entityOf(ctx_.world, noise.instigator);
                    }
                }
                if (!p.text.empty() && p.number2 > 0.0f) {
                    for (const ecs::Entity& e : ctx_.world->findAllWithTag(p.text)) {
                        if (e == me || !e.activeInHierarchy()) continue;
                        const float d = core::length(e.worldPosition() - position);
                        if (d > p.number2 || d >= best) continue;
                        heard = true;
                        best = d;
                        where = e.worldPosition();
                        who = e;
                    }
                }
                if (heard) {
                    if (Value* v = rt_.find(p.key)) setEntity(*v, who);
                    if (Value* v = rt_.find(p.key2); v != nullptr && v->type == VarType::Vector) v->v = where;
                }
                if (Value* v = rt_.find(p.key3); v != nullptr && v->type == VarType::Bool) v->b = heard;
                break;
            }
            case BtServiceKind::RunScript:
                if (ctx_.script_service && !p.text.empty()) ctx_.script_service(n, k, p.text);
                break;
        }
    }
};

void collectActive(const BehaviorTreeAsset& tree, BtRuntime& rt, int n, int depth) {
    if (n < 0 || n >= static_cast<int>(tree.nodes.size()) || depth > 64 || !rt.nodes[n].active) return;
    rt.active_path.push_back(n);
    if (btIsTask(tree.nodes[n].kind)) rt.active_task = n;
    for (const int c : tree.nodes[n].children) collectActive(tree, rt, c, depth + 1);
}

}  // namespace

BtStatus tickBehaviorTree(const BehaviorTreeAsset& tree, BtRuntime& rt, BtContext& ctx) {
    if (tree.nodes.empty() || tree.nodes[0].kind != BtNodeKind::Root) return BtStatus::Failure;
    if (rt.nodes.size() != tree.nodes.size()) rt.nodes.assign(tree.nodes.size(), BtNodeState{});
    if (!rt.running) return BtStatus::Idle;
    rt.time += ctx.dt;
    ++rt.ticks;
    rt.started = true;
    Executor exec(tree, rt, ctx);
    const BtStatus r = exec.tick(0);
    if (r != BtStatus::Running) ++rt.cycles;
    rt.active_path.clear();
    rt.active_task = -1;
    collectActive(tree, rt, 0, 0);
    return r;
}

void abortBehaviorTree(const BehaviorTreeAsset& tree, BtRuntime& rt, BtContext& ctx) {
    if (tree.nodes.empty() || rt.nodes.size() != tree.nodes.size()) return;
    Executor exec(tree, rt, ctx);
    exec.abortSubtree(0);
    rt.active_path.clear();
    rt.active_task = -1;
}

// --- Componente ------------------------------------------------------------------------

void BehaviorTree::reflect(ecs::PropertyVisitor& v) {
    v.asset({"tree", "Behavior Tree", "Asset .crbt: el árbol (compuestos, tareas, decoradores y servicios) y su pizarra"}, tree,
            assets::AssetType::BehaviorTree);
    v.field({"start_active", "Empieza activo", "Si no, espera a bt:start() desde un script"}, start_active);
    v.field({"tick_interval", "Intervalo (s)", "0 = cada frame; más = menos CPU con muchos enemigos"}, tick_interval,
            ecs::FloatRange{0.0f, 2.0f, 0.01f, "%.2f"});
    v.field({"debug", "Depurar", "Abortos y fallos a la Consola"}, debug);
    // El Inspector los dibuja aparte (con el control del tipo de cada clave).
    if (v.wantsAllFields()) {
        ecs::listField(v, {"variables", "Pizarra"}, variables, [](VariableOverride& o, ecs::PropertyVisitor& item) {
            item.field({"name", "Nombre"}, o.name);
            item.field({"type", "Tipo"}, o.type, 0, kVarTypeCount - 1);
            o.type = std::clamp(o.type, 0, kVarTypeCount - 1);
            item.field({"value", "Valor"}, o.value);
        });
    }
}

void registerBehaviorTreeComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("BehaviorTree") == nullptr) {
        registry.registerComponent<BehaviorTree>("BehaviorTree", "Behavior Tree (IA)", "Scripting");
    }
}

}  // namespace cramion::ai
